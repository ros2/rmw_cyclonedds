find_package(launch_testing_ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rcl_interfaces REQUIRED)

add_executable(test_rclcpp_list_parameters_client
  helpers/rclcpp_list_parameters_client.cpp)
ament_target_dependencies(test_rclcpp_list_parameters_client
  rclcpp
  rcl_interfaces)

set(TEST_HELPERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/helpers")
set(TEST_RCLCPP_LIST_PARAMETERS_CLIENT_EXECUTABLE
  "$<TARGET_FILE:test_rclcpp_list_parameters_client>")

function(configure_standard_service_launch_test test_name template_name)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/${template_name}"
    "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.py.configure"
    @ONLY
  )
  file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${test_name}_$<CONFIG>.py"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/${test_name}.py.configure"
  )
  add_launch_test(
    "${CMAKE_CURRENT_BINARY_DIR}/${test_name}_$<CONFIG>.py"
    TARGET "${test_name}"
    TIMEOUT 60
  )
endfunction()

configure_standard_service_launch_test(
  test_standard_cyclone_client_to_fastrtps_service
  launch/test_standard_cyclone_client_to_fastrtps_service.py.in
)
configure_standard_service_launch_test(
  test_standard_fastrtps_client_to_cyclone_service
  launch/test_standard_fastrtps_client_to_cyclone_service.py.in
)
