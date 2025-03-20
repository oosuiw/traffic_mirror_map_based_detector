cmake_minimum_required(VERSION 3.14)
project(traffic_mirror_map_based_detector)

find_package(autoware_cmake REQUIRED)
autoware_package()

find_package(Eigen3 REQUIRED)
find_package(tier4_perception_msgs REQUIRED)
find_package(lanelet2_core REQUIRED)
find_package(lanelet2_extension REQUIRED)

include_directories(
  SYSTEM
    ${EIGEN3_INCLUDE_DIR}
    ${lanelet2_core_INCLUDE_DIRS}
    ${lanelet2_extension_INCLUDE_DIRS}
)

ament_auto_add_library(traffic_mirror_map_based_detector SHARED
  src/node.cpp
)

target_link_libraries(traffic_mirror_map_based_detector
  ${lanelet2_core_LIBRARIES}
  ${lanelet2_extension_LIBRARIES}  # 변수로 수정
)

rclcpp_components_register_node(traffic_mirror_map_based_detector
  PLUGIN "traffic_mirror::MapBasedDetector"
  EXECUTABLE traffic_mirror_map_based_detector_node
)

ament_auto_package(INSTALL_TO_SHARE
  launch
  config
)