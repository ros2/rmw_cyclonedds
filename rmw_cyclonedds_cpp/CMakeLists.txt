# Copyright 2018 ADLINK Technology Limited.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20)

project(rmw_cyclonedds_cpp)

option(USE_NEW_CDR_IMPL "Use the new CDRSerializer/CDRDeserializer instead of the old CDRWriter/CDRReader" ON)

# Default to C++20
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wimplicit-fallthrough)
endif()

find_package(ament_cmake_ros_core REQUIRED)

find_package(rcutils REQUIRED)
find_package(rcpputils REQUIRED)
find_package(tracetools REQUIRED)

find_package(CycloneDDS QUIET CONFIG)
if(CycloneDDS_FOUND)
  if(CycloneDDS_VERSION VERSION_LESS 0.11)
    # Support for shared memory in RMW depends on support for shared memory being compiled
    # in Cyclone DDS for Cyclone version 0.10.x (and older) and iceoryx_binding_c being
    # available
    get_target_property(_cyclonedds_has_shm CycloneDDS::ddsc SHM_SUPPORT_IS_AVAILABLE)
    if(_cyclonedds_has_shm)
      find_package(iceoryx_binding_c REQUIRED)
    else()
      message(STATUS "Cyclone DDS is NOT compiled with support for shared memory")
    endif()
  endif()
else()
  message(WARNING "Could not find Eclipse Cyclone DDS - skipping '${PROJECT_NAME}'")
  ament_package()
  return()
endif()

find_package(rmw REQUIRED)
find_package(rmw_dds_common REQUIRED)
find_package(rmw_security_common REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_introspection_c REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)

if(_cyclonedds_has_shm)
  ament_export_dependencies(iceoryx_binding_c)
endif()
ament_export_dependencies(CycloneDDS)
ament_export_dependencies(rcutils)
ament_export_dependencies(rcpputils)
ament_export_dependencies(rmw)
ament_export_dependencies(rosidl_runtime_c)
ament_export_dependencies(rmw_dds_common)
ament_export_dependencies(rmw_security_common)
ament_export_dependencies(rosidl_typesupport_introspection_c)
ament_export_dependencies(rosidl_typesupport_introspection_cpp)
ament_export_dependencies(tracetools)

add_library(rmw_cyclonedds_cpp
  src/rmw_get_network_flow_endpoints.cpp
  src/rmw_node.cpp
  src/serdata.cpp
  src/u16string.cpp
  src/demangle.cpp
  src/Serialization.cpp
  src/SizeTypeSupport.cpp
  src/SerTypeSupport.cpp
  src/DeserTypeSupport.cpp
  src/TypeSupport2.cpp
  src/type_name.cpp
  src/dyntype.cpp
  src/dyntype_helper.c)

configure_file("src/cdds_version.hpp.in" "src/cdds_version.hpp")
target_include_directories(rmw_cyclonedds_cpp PRIVATE ${CMAKE_BINARY_DIR}/src)

target_link_libraries(rmw_cyclonedds_cpp PUBLIC
  rmw::rmw)

target_link_libraries(rmw_cyclonedds_cpp PRIVATE
  CycloneDDS::ddsc
  rcutils::rcutils
  rcpputils::rcpputils
  rmw_dds_common::rmw_dds_common_library
  rmw_security_common::rmw_security_common_library
  rosidl_typesupport_introspection_c::rosidl_typesupport_introspection_c
  rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp
  rosidl_runtime_c::rosidl_runtime_c
  tracetools::tracetools
)

if(_cyclonedds_has_shm)
  target_link_libraries(rmw_cyclonedds_cpp PRIVATE iceoryx_binding_c::iceoryx_binding_c)
endif()

if(CMAKE_GENERATOR_PLATFORM)
  set(TARGET_ARCH "${CMAKE_GENERATOR_PLATFORM}")
else()
  set(TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_COMPILER_IS_GNUCXX AND TARGET_ARCH MATCHES "^(riscv|RISCV)64$")
  # using GCC, libatomic is not automatically linked for RISC-V
  target_link_libraries(rmw_cyclonedds_cpp PUBLIC -latomic)
endif()

configure_rmw_library(rmw_cyclonedds_cpp)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE
    RMW_VERSION_MAJOR=${rmw_VERSION_MAJOR}
    RMW_VERSION_MINOR=${rmw_VERSION_MINOR}
    RMW_VERSION_PATCH=${rmw_VERSION_PATCH}
    $<$<BOOL:${USE_NEW_CDR_IMPL}>:USE_NEW_CDR_IMPL>
)

ament_export_targets(export_rmw_cyclonedds_cpp)

register_rmw_implementation(
  "c:rosidl_typesupport_introspection_c"
  "cpp:rosidl_typesupport_introspection_cpp")

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()

  find_package(ament_cmake_gtest REQUIRED)
  find_package(ament_cmake_google_benchmark REQUIRED)
  find_package(std_msgs REQUIRED)
  find_package(visualization_msgs REQUIRED)
  find_package(rcl_interfaces REQUIRED)
  find_package(example_interfaces REQUIRED)
  find_package(sensor_msgs REQUIRED)
  find_package(test_msgs REQUIRED)
  find_package(iceoryx_binding_c REQUIRED)

  # Static library with serialization sources shared across all test targets.
  add_library(ts_test_support STATIC
    src/SizeTypeSupport.cpp
    src/SerTypeSupport.cpp
    src/DeserTypeSupport.cpp
    src/TypeSupport2.cpp
    src/Serialization.cpp)
  target_include_directories(ts_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    ${CMAKE_BINARY_DIR}/src)
  target_link_libraries(ts_test_support PUBLIC
    CycloneDDS::ddsc
    iceoryx_binding_c::iceoryx_binding_c
    rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp
    rosidl_typesupport_introspection_c::rosidl_typesupport_introspection_c)

  # -- std_msgs + visualization_msgs tests ------------------------------------
  ament_add_gtest(test_ts_std_msgs test/test_std_msgs.cpp)
  target_link_libraries(test_ts_std_msgs
    ts_test_support
    ${std_msgs_LIBRARIES}
    ${visualization_msgs_LIBRARIES})
  ament_target_dependencies(test_ts_std_msgs std_msgs visualization_msgs)

  # -- rcl_interfaces tests ---------------------------------------------------
  ament_add_gtest(test_ts_rcl_interfaces test/test_rcl_interfaces.cpp)
  target_link_libraries(test_ts_rcl_interfaces
    ts_test_support
    ${rcl_interfaces_LIBRARIES})
  ament_target_dependencies(test_ts_rcl_interfaces rcl_interfaces)

  # -- wstring (example_interfaces) tests -------------------------------------
  ament_add_gtest(test_ts_wstring test/test_wstring.cpp)
  target_link_libraries(test_ts_wstring
    ts_test_support
    ${example_interfaces_LIBRARIES})
  ament_target_dependencies(test_ts_wstring example_interfaces)

  # -- keyed message tests (test_msgs) ----------------------------------------
  ament_add_gtest(test_ts_keyed_msgs test/test_keyed_msgs.cpp)
  target_link_libraries(test_ts_keyed_msgs
    ts_test_support
    ${test_msgs_LIBRARIES})
  ament_target_dependencies(test_ts_keyed_msgs test_msgs)

  # -- test_msgs serialization tests ------------------------------------------
  ament_add_gtest(test_ts_test_msgs test/test_test_msgs.cpp)
  target_link_libraries(test_ts_test_msgs
    ts_test_support
    ${test_msgs_LIBRARIES}
    ${rmw_dds_common_LIBRARIES})
  ament_target_dependencies(test_ts_test_msgs test_msgs rmw_dds_common)

  # -- benchmark --------------------------------------------------------------
  ament_add_google_benchmark(benchmark_size_computation
    test/benchmark_size_computation.cpp)
  target_link_libraries(benchmark_size_computation
    ts_test_support
    rmw_cyclonedds_cpp
    ${std_msgs_LIBRARIES}
    ${visualization_msgs_LIBRARIES}
    ${sensor_msgs_LIBRARIES})
  ament_target_dependencies(benchmark_size_computation
    std_msgs visualization_msgs sensor_msgs)
endif()

ament_package()

install(
  TARGETS rmw_cyclonedds_cpp
  EXPORT export_rmw_cyclonedds_cpp
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

include(cmake/get_rmw_cyclonedds_output_filter.cmake)
get_rmw_cyclonedds_output_filter(rmw_cyclonedds_output_patterns)
ament_index_register_resource("rmw_output_patterns" CONTENT "${rmw_cyclonedds_output_patterns}")
