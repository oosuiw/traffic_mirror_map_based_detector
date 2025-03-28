// Copyright 2023 TIER IV, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "traffic_mirror_map_based_detector/node.hpp"

#include <lanelet2_extension/utility/message_conversion.hpp>
#include <lanelet2_extension/utility/utilities.hpp>
#include <lanelet2_extension/visualization/visualization.hpp>
#include <tier4_autoware_utils/math/normalization.hpp>
#include <tier4_autoware_utils/math/unit_conversion.hpp>

#include "tier4_perception_msgs/msg/traffic_mirror_roi_array.hpp"

#include <lanelet2_core/Exceptions.h>
#include <lanelet2_core/geometry/Point.h>
#include <lanelet2_projection/UTM.h>
#include <lanelet2_routing/RoutingGraphContainer.h>
#include <tf2/LinearMath/Matrix3x3.h>
#include <tf2/LinearMath/Transform.h>

#define EIGEN_MPL2_ONLY
#include <Eigen/Core>
#include <Eigen/Geometry>

#ifdef ROS_DISTRO_GALACTIC
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>
#else
#include <tf2_geometry_msgs/tf2_geometry_msgs.hpp>
#endif

namespace
{
cv::Point2d calcRawImagePointFromPoint3D(
  const image_geometry::PinholeCameraModel & pinhole_camera_model, const cv::Point3d & point3d)
{
  cv::Point2d rectified_image_point = pinhole_camera_model.project3dToPixel(point3d);
  return pinhole_camera_model.unrectifyPoint(rectified_image_point);
}

cv::Point2d calcRawImagePointFromPoint3D(
  const image_geometry::PinholeCameraModel & pinhole_camera_model, const tf2::Vector3 & point3d)
{
  return calcRawImagePointFromPoint3D(
    pinhole_camera_model, cv::Point3d(point3d.x(), point3d.y(), point3d.z()));
}

void roundInImageFrame(
  const image_geometry::PinholeCameraModel & pinhole_camera_model, cv::Point2d & point)
{
  const sensor_msgs::msg::CameraInfo camera_info = pinhole_camera_model.cameraInfo();
  point.x =
    std::max(std::min(point.x, static_cast<double>(static_cast<int>(camera_info.width) - 1)), 0.0);
  point.y =
    std::max(std::min(point.y, static_cast<double>(static_cast<int>(camera_info.height) - 1)), 0.0);
}

bool isInDistanceRange(
  const tf2::Vector3 & p1, const tf2::Vector3 & p2, const double max_distance_range)
{
  const double sq_dist =
    (p1.x() - p2.x()) * (p1.x() - p2.x()) + (p1.y() - p2.y()) * (p1.y() - p2.y());
  return sq_dist < (max_distance_range * max_distance_range);
}

bool isInAngleRange(const double & tl_yaw, const double & camera_yaw, const double max_angle_range)
{
  Eigen::Vector2d vec1, vec2;
  vec1 << std::cos(tl_yaw), std::sin(tl_yaw);
  vec2 << std::cos(camera_yaw), std::sin(camera_yaw);
  const double diff_angle = std::acos(vec1.dot(vec2));
  return std::fabs(diff_angle) < max_angle_range;
}

bool isInImageFrame(
  const image_geometry::PinholeCameraModel & pinhole_camera_model, const tf2::Vector3 & point)
{
  if (point.z() <= 0.0) {
    return false;
  }

  cv::Point2d point2d = calcRawImagePointFromPoint3D(pinhole_camera_model, point);
  if (0 <= point2d.x && point2d.x < pinhole_camera_model.cameraInfo().width) {
    if (0 <= point2d.y && point2d.y < pinhole_camera_model.cameraInfo().height) {
      return true;
    }
  }
  return false;
}

tf2::Vector3 getTrafficMirrorTopLeft(const lanelet::ConstLineString3d & traffic_mirror)
{
  const auto & tl_bl = traffic_mirror.front();
  const double tl_height = traffic_mirror.attributeOr("height", 0.0);
  return tf2::Vector3(tl_bl.x(), tl_bl.y(), tl_bl.z() + tl_height);
}

tf2::Vector3 getTrafficMirrorBottomRight(const lanelet::ConstLineString3d & traffic_mirror)
{
  const auto & tl_bl = traffic_mirror.back();
  return tf2::Vector3(tl_bl.x(), tl_bl.y(), tl_bl.z());
}

tf2::Vector3 getTrafficMirrorCenter(const lanelet::ConstLineString3d & traffic_mirror)
{
  tf2::Vector3 top_left = getTrafficMirrorTopLeft(traffic_mirror);
  tf2::Vector3 bottom_right = getTrafficMirrorBottomRight(traffic_mirror);
  return (top_left + bottom_right) / 2;
}

}  // namespace

namespace traffic_mirror
{
MapBasedDetector::MapBasedDetector(const rclcpp::NodeOptions & node_options)
: Node("traffic_mirror_map_based_detector", node_options),
  tf_buffer_(this->get_clock()),
  tf_listener_(tf_buffer_)
{
  using std::placeholders::_1;

  // parameter declaration needs default values: are 0.0 good defaults for this?
  config_.max_vibration_pitch = declare_parameter<double>("max_vibration_pitch", 0.0);
  config_.max_vibration_yaw = declare_parameter<double>("max_vibration_yaw", 0.0);
  config_.max_vibration_height = declare_parameter<double>("max_vibration_height", 0.0);
  config_.max_vibration_width = declare_parameter<double>("max_vibration_width", 0.0);
  config_.max_vibration_depth = declare_parameter<double>("max_vibration_depth", 0.0);
  config_.min_timestamp_offset = declare_parameter<double>("min_timestamp_offset", 0.0);
  config_.max_timestamp_offset = declare_parameter<double>("max_timestamp_offset", 0.0);
  config_.timestamp_sample_len = declare_parameter<double>("timestamp_sample_len", 0.01);
  config_.max_detection_range = declare_parameter<double>("max_detection_range", 200.0);

  // 디버깅을 위한 파라미터 출력 추가 #KMS_250318
  RCLCPP_INFO(get_logger(),
              "Config values: max_vibration_pitch: %f, max_vibration_yaw: %f, max_vibration_height: %f, max_vibration_width: %f, max_vibration_depth: %f, min_timestamp_offset: %f, max_timestamp_offset: %f, timestamp_sample_len: %f, max_detection_range: %f",
              config_.max_vibration_pitch, config_.max_vibration_yaw, config_.max_vibration_height,
              config_.max_vibration_width, config_.max_vibration_depth, config_.min_timestamp_offset,
              config_.max_timestamp_offset, config_.timestamp_sample_len, config_.max_detection_range); //KMS_250318

  if (config_.max_detection_range <= 0) {
    RCLCPP_ERROR_STREAM(
      get_logger(), "Invalid param max_detection_range = " << config_.max_detection_range
                                                           << ", set to default value = 200");
    config_.max_detection_range = 200.0;
  }
  if (config_.timestamp_sample_len <= 0) {
    RCLCPP_ERROR_STREAM(
      get_logger(), "Invalid param timestamp_sample_len = " << config_.timestamp_sample_len
                                                            << ", set to default value = 0.01");
    config_.timestamp_sample_len = 0.01;
  }
  // 수정: 값이 동일한 경우는 허용 (작은 경우에만 0으로 세팅) #KMS_250318
  if (config_.max_timestamp_offset < config_.min_timestamp_offset) { //KMS_250318
    RCLCPP_ERROR_STREAM(
      get_logger(), "max_timestamp_offset < min_timestamp_offset. Set both to 0"); //KMS_250318
    config_.max_timestamp_offset = 0.0; //KMS_250318
    config_.min_timestamp_offset = 0.0; //KMS_250318
  }

  // subscribers
  map_sub_ = create_subscription<autoware_auto_mapping_msgs::msg::HADMapBin>(
    "~/input/vector_map", rclcpp::QoS{1}.transient_local(),
    std::bind(&MapBasedDetector::mapCallback, this, _1));
  camera_info_sub_ = create_subscription<sensor_msgs::msg::CameraInfo>(
    "~/input/camera_info", rclcpp::SensorDataQoS(),
    std::bind(&MapBasedDetector::cameraInfoCallback, this, _1));
  route_sub_ = create_subscription<autoware_planning_msgs::msg::LaneletRoute>(
    "~/input/route", rclcpp::QoS{1}.transient_local(),
    std::bind(&MapBasedDetector::routeCallback, this, _1));

  // publishers
  roi_pub_ = this->create_publisher<tier4_perception_msgs::msg::TrafficMirrorRoiArray>(
    "~/output/mirror_rois", 1);
  expect_roi_pub_ =
    this->create_publisher<tier4_perception_msgs::msg::TrafficMirrorRoiArray>("~/expect/rois", 1);
  viz_pub_ = this->create_publisher<visualization_msgs::msg::MarkerArray>("~/debug/markers", 1);
}

bool MapBasedDetector::getTransform(
  const rclcpp::Time & t, const std::string & frame_id, tf2::Transform & tf) const
{
  try {
    geometry_msgs::msg::TransformStamped transform =
      tf_buffer_.lookupTransform("map", frame_id, t, rclcpp::Duration::from_seconds(0.2));
    tf2::fromMsg(transform.transform, tf);
  } catch (tf2::TransformException & ex) {
    return false;
  }
  return true;
}

void MapBasedDetector::cameraInfoCallback(
  const sensor_msgs::msg::CameraInfo::ConstSharedPtr input_msg)
{
  if (all_traffic_mirrors_ptr_ == nullptr && route_traffic_mirrors_ptr_ == nullptr) {
    RCLCPP_DEBUG(get_logger(), "No traffic mirror data available, skipping camera callback"); //KMS_250318
    return;
  }

  image_geometry::PinholeCameraModel pinhole_camera_model;
  pinhole_camera_model.fromCameraInfo(*input_msg);

  tier4_perception_msgs::msg::TrafficMirrorRoiArray output_msg;
  output_msg.header = input_msg->header;
  tier4_perception_msgs::msg::TrafficMirrorRoiArray expect_roi_msg;
  expect_roi_msg = output_msg;

  /* Camera pose in the period*/
  std::vector<tf2::Transform> tf_map2camera_vec;
  rclcpp::Time t1 = rclcpp::Time(input_msg->header.stamp) +
                    rclcpp::Duration::from_seconds(config_.min_timestamp_offset);
  rclcpp::Time t2 = rclcpp::Time(input_msg->header.stamp) +
                    rclcpp::Duration::from_seconds(config_.max_timestamp_offset);
  rclcpp::Duration interval = rclcpp::Duration::from_seconds(0.01);
  for (auto t = t1; t <= t2; t += interval) {
    tf2::Transform tf;
    if (getTransform(t, input_msg->header.frame_id, tf)) {
      tf_map2camera_vec.push_back(tf);
    }
  }
  /* camera pose at the exact moment*/
  tf2::Transform tf_map2camera;
  if (!getTransform(
        rclcpp::Time(input_msg->header.stamp), input_msg->header.frame_id, tf_map2camera)) {
    RCLCPP_WARN_THROTTLE(
      get_logger(), *get_clock(), 5000, "cannot get transform from map frame to camera frame");
    return;
  }
  if (tf_map2camera_vec.empty()) {
    tf_map2camera_vec.push_back(tf_map2camera);
  }

  /*
   * visible_traffic_mirrors : for each traffic mirror in map check if in range and in view angle of
   * camera
   */
  std::vector<lanelet::ConstLineString3d> visible_traffic_mirrors;
  // If get a route, use only traffic mirrors on the route.
  if (route_traffic_mirrors_ptr_ != nullptr) {
    getVisibleTrafficMirrors(
      *route_traffic_mirrors_ptr_, tf_map2camera_vec, pinhole_camera_model, visible_traffic_mirrors);
    // If don't get a route, use the traffic mirrors around ego vehicle.
  } else if (all_traffic_mirrors_ptr_ != nullptr) {
    getVisibleTrafficMirrors(
      *all_traffic_mirrors_ptr_, tf_map2camera_vec, pinhole_camera_model, visible_traffic_mirrors);
    // This shouldn't run.
  } else {
    return;
  }

  /*
   * Get the ROI from the lanelet and the intrinsic matrix of camera to determine where it appears
   * in image.
   */
  Config expect_roi_cfg = config_;
  expect_roi_cfg.max_vibration_depth = 0;
  expect_roi_cfg.max_vibration_height = 0;
  expect_roi_cfg.max_vibration_width = 0;
  expect_roi_cfg.max_vibration_yaw = 0;
  expect_roi_cfg.max_vibration_pitch = 0;
  for (const auto & traffic_mirror : visible_traffic_mirrors) {
    tier4_perception_msgs::msg::TrafficMirrorRoi rough_roi, expect_roi;
    if (!getTrafficMirrorRoi(
          tf_map2camera, pinhole_camera_model, traffic_mirror, expect_roi_cfg, expect_roi)) {
      continue;
    }
    if (!getTrafficMirrorRoi(
          tf_map2camera_vec, pinhole_camera_model, traffic_mirror, config_, rough_roi)) {
      continue;
    }
    output_msg.rois.push_back(rough_roi);
    expect_roi_msg.rois.push_back(expect_roi);
  }

  roi_pub_->publish(output_msg);
  expect_roi_pub_->publish(expect_roi_msg);
  publishVisibleTrafficMirrors(
    tf_map2camera_vec[0], input_msg->header, visible_traffic_mirrors, viz_pub_);
}

bool MapBasedDetector::getTrafficMirrorRoi(
  const tf2::Transform & tf_map2camera,
  const image_geometry::PinholeCameraModel & pinhole_camera_model,
  const lanelet::ConstLineString3d traffic_mirror, const Config & config,
  tier4_perception_msgs::msg::TrafficMirrorRoi & roi) const
{
  roi.traffic_mirror_id = traffic_mirror.id();

  // for roi.x_offset and roi.y_offset
  {
    tf2::Vector3 map2tl = getTrafficMirrorTopLeft(traffic_mirror);
    tf2::Vector3 camera2tl = tf_map2camera.inverse() * map2tl;
    // max vibration
    const double max_vibration_x =
      std::sin(config.max_vibration_yaw * 0.5) * camera2tl.z() + config.max_vibration_width * 0.5;
    const double max_vibration_y = std::sin(config.max_vibration_pitch * 0.5) * camera2tl.z() +
                                   config.max_vibration_height * 0.5;
    const double max_vibration_z = config.max_vibration_depth * 0.5;
    // enlarged target position in camera coordinate
    {
      tf2::Vector3 point3d =
        camera2tl - tf2::Vector3(max_vibration_x, max_vibration_y, max_vibration_z);
      if (point3d.z() <= 0.0) {
        return false;
      }
      cv::Point2d point2d = calcRawImagePointFromPoint3D(pinhole_camera_model, point3d);
      roundInImageFrame(pinhole_camera_model, point2d);
      roi.roi.x_offset = point2d.x;
      roi.roi.y_offset = point2d.y;
    }
  }

  // for roi.width and roi.height
  {
    tf2::Vector3 map2tl = getTrafficMirrorBottomRight(traffic_mirror);
    tf2::Vector3 camera2tl = tf_map2camera.inverse() * map2tl;
    // max vibration
    const double max_vibration_x =
      std::sin(config.max_vibration_yaw * 0.5) * camera2tl.z() + config.max_vibration_width * 0.5;
    const double max_vibration_y = std::sin(config.max_vibration_pitch * 0.5) * camera2tl.z() +
                                   config.max_vibration_height * 0.5;
    const double max_vibration_z = config.max_vibration_depth * 0.5;
    // enlarged target position in camera coordinate
    {
      tf2::Vector3 point3d =
        camera2tl + tf2::Vector3(max_vibration_x, max_vibration_y, -max_vibration_z);
      if (point3d.z() <= 0.0) {
        return false;
      }
      cv::Point2d point2d = calcRawImagePointFromPoint3D(pinhole_camera_model, point3d);
      roundInImageFrame(pinhole_camera_model, point2d);
      roi.roi.width = point2d.x - roi.roi.x_offset;
      roi.roi.height = point2d.y - roi.roi.y_offset;
    }

    if (roi.roi.width < 1 || roi.roi.height < 1) {
      return false;
    }
  }
  return true;
}

bool MapBasedDetector::getTrafficMirrorRoi(
  const std::vector<tf2::Transform> & tf_map2camera_vec,
  const image_geometry::PinholeCameraModel & pinhole_camera_model,
  const lanelet::ConstLineString3d traffic_mirror, const Config & config,
  tier4_perception_msgs::msg::TrafficMirrorRoi & out_roi) const
{
  std::vector<tier4_perception_msgs::msg::TrafficMirrorRoi> rois;
  for (const auto & tf_map2camera : tf_map2camera_vec) {
    tier4_perception_msgs::msg::TrafficMirrorRoi roi;
    if (getTrafficMirrorRoi(tf_map2camera, pinhole_camera_model, traffic_mirror, config, roi)) {
      rois.push_back(roi);
    }
  }
  if (rois.empty()) {
    return false;
  }
  out_roi = rois.front();
  /**
   * get the maximum possible rough roi among all the tf
   */
  uint32_t x1 = pinhole_camera_model.cameraInfo().width - 1;
  uint32_t x2 = 0;
  uint32_t y1 = pinhole_camera_model.cameraInfo().height - 1;
  uint32_t y2 = 0;
  for (const auto & roi : rois) {
    x1 = std::min(x1, roi.roi.x_offset);
    x2 = std::max(x2, roi.roi.x_offset + roi.roi.width);
    y1 = std::min(y1, roi.roi.y_offset);
    y2 = std::max(y2, roi.roi.y_offset + roi.roi.height);
  }
  out_roi.roi.x_offset = x1;
  out_roi.roi.y_offset = y1;
  out_roi.roi.width = x2 - x1;
  out_roi.roi.height = y2 - y1;
  return true;
}

void MapBasedDetector::mapCallback(
  const autoware_auto_mapping_msgs::msg::HADMapBin::ConstSharedPtr input_msg)
{
  lanelet_map_ptr_ = std::make_shared<lanelet::LaneletMap>();

  lanelet::utils::conversion::fromBinMsg(*input_msg, lanelet_map_ptr_);
  lanelet::ConstLanelets all_lanelets = lanelet::utils::query::laneletLayer(lanelet_map_ptr_);
  std::vector<lanelet::AutowareTrafficMirrorConstPtr> all_lanelet_traffic_mirrors =
    lanelet::utils::query::autowareTrafficMirrors(all_lanelets);
  all_traffic_mirrors_ptr_ = std::make_shared<MapBasedDetector::TrafficMirrorSet>();
  for (auto tl_itr = all_lanelet_traffic_mirrors.begin(); tl_itr != all_lanelet_traffic_mirrors.end();
       ++tl_itr) {
    lanelet::AutowareTrafficMirrorConstPtr tl = *tl_itr;
    // RegulatoryElement의 getParameters()를 통해 traffic_mirrors 접근
    const auto & params = tl->getParameters();
    auto traffic_mirrors_it = params.find("traffic_mirrors");
    if (traffic_mirrors_it != params.end()) {
      for (const auto & lsp : traffic_mirrors_it->second) {
        if (const auto * ls = boost::get<lanelet::ConstLineString3d>(&lsp)) {
          all_traffic_mirrors_ptr_->insert(*ls);
        }
      }
    }
  }
}

void MapBasedDetector::routeCallback(
  const autoware_planning_msgs::msg::LaneletRoute::ConstSharedPtr input_msg)
{
  if (lanelet_map_ptr_ == nullptr) {
    RCLCPP_WARN(get_logger(), "cannot set traffic mirror in route because don't receive map");
    return;
  }
  lanelet::ConstLanelets route_lanelets;
  for (const auto & segment : input_msg->segments) {
    for (const auto & primitive : segment.primitives) {
      try {
        route_lanelets.push_back(lanelet_map_ptr_->laneletLayer.get(primitive.id));
      } catch (const lanelet::NoSuchPrimitiveError & ex) {
        RCLCPP_ERROR(get_logger(), "%s", ex.what());
        return;
      }
    }
  }
  std::vector<lanelet::AutowareTrafficMirrorConstPtr> route_lanelet_traffic_mirrors =
    lanelet::utils::query::autowareTrafficMirrors(route_lanelets);
  route_traffic_mirrors_ptr_ = std::make_shared<MapBasedDetector::TrafficMirrorSet>();
  for (auto tl_itr = route_lanelet_traffic_mirrors.begin();
       tl_itr != route_lanelet_traffic_mirrors.end(); ++tl_itr) {
    lanelet::AutowareTrafficMirrorConstPtr tl = *tl_itr;
    // RegulatoryElement의 getParameters()를 통해 traffic_mirrors 접근
    const auto & params = tl->getParameters();
    auto traffic_mirrors_it = params.find("traffic_mirrors");
    if (traffic_mirrors_it != params.end()) {
      for (const auto & lsp : traffic_mirrors_it->second) {
        if (const auto * ls = boost::get<lanelet::ConstLineString3d>(&lsp)) {
          route_traffic_mirrors_ptr_->insert(*ls);
        }
      }
    }
  }
}

void MapBasedDetector::getVisibleTrafficMirrors(
  const MapBasedDetector::TrafficMirrorSet & all_traffic_mirrors,
  const std::vector<tf2::Transform> & tf_map2camera_vec,
  const image_geometry::PinholeCameraModel & pinhole_camera_model,
  std::vector<lanelet::ConstLineString3d> & visible_traffic_mirrors) const
{
  for (const auto & traffic_mirror : all_traffic_mirrors) {
    // some "Traffic Mirror" are actually not traffic mirrors
    if (
      traffic_mirror.hasAttribute("subtype") == false ||
      traffic_mirror.attribute("subtype").value() == "solid") {
      continue;
    }
    // traffic mirror bottom left
    const auto & tl_bl = traffic_mirror.front();
    // traffic mirror bottom right
    const auto & tl_br = traffic_mirror.back();
    // check distance range
    tf2::Vector3 tl_center = getTrafficMirrorCenter(traffic_mirror);
    // for every possible transformation, check if the tl is visible.
    // If under any tf the tl is visible, keep it
    for (const auto & tf_map2camera : tf_map2camera_vec) {
      if (!isInDistanceRange(tl_center, tf_map2camera.getOrigin(), config_.max_detection_range)) {
        continue;
      }

      // check angle range
      const double tl_yaw = tier4_autoware_utils::normalizeRadian(
        std::atan2(tl_br.y() - tl_bl.y(), tl_br.x() - tl_bl.x()) + M_PI_2);
      constexpr double max_angle_range = tier4_autoware_utils::deg2rad(40.0);

      // get direction of z axis
      tf2::Vector3 camera_z_dir(0, 0, 1);
      tf2::Matrix3x3 camera_rotation_matrix(tf_map2camera.getRotation());
      camera_z_dir = camera_rotation_matrix * camera_z_dir;
      double camera_yaw = std::atan2(camera_z_dir.y(), camera_z_dir.x());
      camera_yaw = tier4_autoware_utils::normalizeRadian(camera_yaw);
      if (!isInAngleRange(tl_yaw, camera_yaw, max_angle_range)) {
        continue;
      }

      // check within image frame
      tf2::Vector3 tf_camera2tltl = tf_map2camera.inverse() * getTrafficMirrorTopLeft(traffic_mirror);
      tf2::Vector3 tf_camera2tlbr =
        tf_map2camera.inverse() * getTrafficMirrorBottomRight(traffic_mirror);
      if (
        !isInImageFrame(pinhole_camera_model, tf_camera2tltl) &&
        !isInImageFrame(pinhole_camera_model, tf_camera2tlbr)) {
        continue;
      }
      visible_traffic_mirrors.push_back(traffic_mirror);
      break;
    }
  }
}

void MapBasedDetector::publishVisibleTrafficMirrors(
  const tf2::Transform & tf_map2camera, const std_msgs::msg::Header & cam_info_header,
  const std::vector<lanelet::ConstLineString3d> & visible_traffic_mirrors,
  const rclcpp::Publisher<visualization_msgs::msg::MarkerArray>::SharedPtr pub)
{
  visualization_msgs::msg::MarkerArray output_msg;
  for (const auto & traffic_mirror : visible_traffic_mirrors) {
    const int id = traffic_mirror.id();
    tf2::Vector3 tl_central_point = getTrafficMirrorCenter(traffic_mirror);
    tf2::Vector3 camera2tl = tf_map2camera.inverse() * tl_central_point;

    visualization_msgs::msg::Marker marker;
    marker.header = cam_info_header;
    marker.id = id;
    marker.type = visualization_msgs::msg::Marker::LINE_LIST;
    marker.ns = std::string("beam");
    marker.scale.x = 0.05;
    marker.action = visualization_msgs::msg::Marker::MODIFY;
    marker.pose.position.x = 0.0;
    marker.pose.position.y = 0.0;
    marker.pose.position.z = 0.0;
    marker.pose.orientation.x = 0.0;
    marker.pose.orientation.y = 0.0;
    marker.pose.orientation.z = 0.0;
    marker.pose.orientation.w = 1.0;
    geometry_msgs::msg::Point point;
    point.x = 0.0;
    point.y = 0.0;
    point.z = 0.0;
    marker.points.push_back(point);
    point.x = camera2tl.x();
    point.y = camera2tl.y();
    point.z = camera2tl.z();
    marker.points.push_back(point);

    marker.lifetime = rclcpp::Duration::from_seconds(0.2);
    marker.color.a = 0.999;  // Don't forget to set the alpha!
    marker.color.r = 0.0;
    marker.color.g = 1.0;
    marker.color.b = 0.0;

    output_msg.markers.push_back(marker);
  }
  pub->publish(output_msg);
}
}  // namespace traffic_mirror

#include <rclcpp_components/register_node_macro.hpp>
RCLCPP_COMPONENTS_REGISTER_NODE(traffic_mirror::MapBasedDetector)
