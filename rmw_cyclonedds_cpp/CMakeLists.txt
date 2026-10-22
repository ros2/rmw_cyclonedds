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
find_package(rosidl_buffer REQUIRED)
find_package(rosidl_buffer_backend REQUIRED)
find_package(rosidl_buffer_backend_registry REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_introspection_c REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)
# rmw_node.cpp spawns std::thread in two places -- its own
# discovery_thread_start(), and the discovery-retry threads the
# buffer-backend mechanism adds -- so this package needs pthread symbols
# for both. Nothing here previously guaranteed they were linked; they
# happened to come in transitively from another dependency. Made explicit
# here so that stops being an assumption.
find_package(Threads REQUIRED)

if(_cyclonedds_has_shm)
  ament_export_dependencies(iceoryx_binding_c)
endif()
ament_export_dependencies(CycloneDDS)
ament_export_dependencies(rcutils)
ament_export_dependencies(rcpputils)
ament_export_dependencies(rmw)
ament_export_dependencies(rosidl_buffer)
ament_export_dependencies(rosidl_buffer_backend)
ament_export_dependencies(rosidl_buffer_backend_registry)
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
  src/TypeSupport2.cpp
  src/BufferEndpointDiscovery.cpp
  src/type_name.cpp
  src/dyntype.cpp
  src/dyntype_helper.c)

configure_file("src/cdds_version.hpp.in" "src/cdds_version.hpp")
target_include_directories(rmw_cyclonedds_cpp PRIVATE ${CMAKE_BINARY_DIR}/src)

target_link_libraries(rmw_cyclonedds_cpp PUBLIC
  rmw::rmw)

target_link_libraries(rmw_cyclonedds_cpp PRIVATE
  ament_cmake_ros_core::ament_ros_cxx_standard
  CycloneDDS::ddsc
  rcutils::rcutils
  rcpputils::rcpputils
  rmw_dds_common::rmw_dds_common_library
  rmw_security_common::rmw_security_common_library
  rosidl_buffer::rosidl_buffer
  rosidl_buffer_backend::rosidl_buffer_backend
  rosidl_buffer_backend_registry::rosidl_buffer_backend_registry
  rosidl_typesupport_introspection_c::rosidl_typesupport_introspection_c
  rosidl_typesupport_introspection_cpp::rosidl_typesupport_introspection_cpp
  rosidl_runtime_c::rosidl_runtime_c
  tracetools::tracetools
  Threads::Threads
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
)

ament_export_targets(export_rmw_cyclonedds_cpp)

register_rmw_implementation(
  "c:rosidl_typesupport_introspection_c"
  "cpp:rosidl_typesupport_introspection_cpp")

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()

  # First-time gtest coverage for this package -- see test/CMakeLists.txt's
  # own header comment for the shape.
  #
  # The test plugin's pluginlib_export_plugin_description_file() call lives
  # HERE, at top level, rather than inside test/CMakeLists.txt's own
  # add_subdirectory(test) below -- measured live: called from the
  # subdirectory, the resulting shared library and plugins.xml both
  # installed correctly, but the ament index resource
  # (share/ament_index/resource_index/rosidl_buffer_backend__pluginlib__
  # plugin/...) that pluginlib::ClassLoader actually scans at runtime never
  # appeared, and backend_locally_available()'s own gtest failed
  # ("Actual: false, Expected: true") even after install. ament_cmake's
  # resource registration macros queue into a directory-scoped variable
  # that ament_package() reads back from ITS OWN (top-level) scope, not
  # from a child added via add_subdirectory() -- so the call has to happen
  # before ament_package() runs, in the same directory scope it runs in.
  find_package(pluginlib REQUIRED)
  find_package(rosidl_typesupport_cpp REQUIRED)
  find_package(std_msgs REQUIRED)

  add_library(rmw_cyclonedds_cpp_test_buffer_backend_plugin SHARED
    test/cyclonedds_test_buffer_backend.cpp
  )
  target_include_directories(rmw_cyclonedds_cpp_test_buffer_backend_plugin PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/test
  )
  target_link_libraries(rmw_cyclonedds_cpp_test_buffer_backend_plugin
    ament_cmake_ros_core::ament_ros_cxx_standard
    pluginlib::pluginlib
    rmw::rmw
    rosidl_buffer::rosidl_buffer
    rosidl_buffer_backend::rosidl_buffer_backend
    rosidl_typesupport_cpp::rosidl_typesupport_cpp
    ${std_msgs_TARGETS}
  )
  install(TARGETS rmw_cyclonedds_cpp_test_buffer_backend_plugin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
  )
  # The plugin_category ("rosidl_buffer_backend") MUST match the first
  # argument BufferBackendRegistry's constructor passes to
  # pluginlib::ClassLoader<rosidl::BufferBackend> -- same requirement
  # cuda_buffer_backend's own CMakeLists.txt documents.
  pluginlib_export_plugin_description_file(
    rosidl_buffer_backend test/cyclonedds_test_buffer_plugin.xml)

  add_subdirectory(test)
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
