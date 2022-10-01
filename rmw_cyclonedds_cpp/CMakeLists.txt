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

cmake_minimum_required(VERSION 3.5)

project(rmw_cyclonedds_cpp)

# Default to C++14
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 14)
endif()

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic -Wimplicit-fallthrough)
endif()

find_package(ament_cmake_ros REQUIRED)

find_package(rcutils REQUIRED)
find_package(rcpputils REQUIRED)
find_package(tracetools REQUIRED)

#find_package(cyclonedds_cmake_module REQUIRED)
find_package(CycloneDDS QUIET CONFIG)
if(CycloneDDS_FOUND)
  # Support for shared memory in RMW depends on support for shared memory being compiled in
  # Cyclone DDS and iceoryx_binding_c being available
  get_target_property(_cyclonedds_has_shm CycloneDDS::ddsc SHM_SUPPORT_IS_AVAILABLE)
  if(_cyclonedds_has_shm)
    find_package(iceoryx_binding_c REQUIRED)
  else()
    message(STATUS "Cyclone DDS is NOT compiled with support for shared memory")
  endif()
else()
  message(WARNING "Could not find Eclipse Cyclone DDS - skipping '${PROJECT_NAME}'")
  ament_package()
  return()
endif()

find_package(rmw REQUIRED)
find_package(rmw_dds_common REQUIRED)
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
ament_export_dependencies(rosidl_typesupport_introspection_c)
ament_export_dependencies(rosidl_typesupport_introspection_cpp)
ament_export_dependencies(tracetools)

add_library(rmw_cyclonedds_cpp
  src/rmw_get_network_flow_endpoints.cpp
  src/rmw_node.cpp
  src/serdata.cpp
  src/serdes.cpp
  src/u16string.cpp
  src/exception.cpp
  src/demangle.cpp
  src/deserialization_exception.cpp
  src/Serialization.cpp
  src/TypeSupport2.cpp
  src/TypeSupport.cpp)

target_link_libraries(rmw_cyclonedds_cpp PRIVATE
  CycloneDDS::ddsc)

if(_cyclonedds_has_shm)
  target_link_libraries(rmw_cyclonedds_cpp PRIVATE iceoryx_binding_c::iceoryx_binding_c)
endif()

target_link_libraries(rmw_cyclonedds_cpp PUBLIC
  rmw::rmw)
target_link_libraries(rmw_cyclonedds_cpp PRIVATE
  rcutils::rcutils
  rcpputils::rcpputils
  rosidl_typesupport_introspection_c::rosidl_typesupport_introspection_c
  rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp
  rmw_dds_common::rmw_dds_common_library
  rosidl_runtime_c::rosidl_runtime_c
  tracetools::tracetools)

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
)

ament_export_targets(export_rmw_cyclonedds_cpp)

register_rmw_implementation(
  "c:rosidl_typesupport_c:rosidl_typesupport_introspection_c"
  "cpp:rosidl_typesupport_cpp:rosidl_typesupport_introspection_cpp")

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
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
