# First-time gtest infrastructure for rmw_cyclonedds_cpp -- see this
# package's own top-level CMakeLists.txt for the BUILD_TESTING-gated
# add_subdirectory(test) that reaches this file.
#
# Four binaries, in increasing order of what they need:
#   1. test_buffer_endpoint_discovery_unit  -- Tier 1, no RMW involved.
#   2. test_cyclonedds_buffer_backend_registered -- proves dynamic pluginlib
#      discovery of this package's own throwaway test plugin, mirroring
#      cuda_buffer_backend's test_cuda_backend_registered.cpp.
#   3. test_buffer_descriptor_publish -- two named tests against the real
#      publish-side code path via two ordinary rclcpp endpoints in one
#      process.
#   4. test_buffer_descriptor_accuracy -- byte-for-byte content through the
#      full publish -> wire -> receive path, reachable in one process
#      because the deadlock fix removed the same-process-pub-sub deadlock.
#
# 2, 3 and 4 all need the test plugin's ament-index registration resolved at
# RUNTIME (pluginlib::ClassLoader scans AMENT_PREFIX_PATH, not the build
# tree -- see cuda_buffer_backend/CMakeLists.txt's own comment on this,
# measured there as "0 backend(s) found" from a pre-install ctest run), so
# a consumer of this package's tests needs an install-then-test flow, not a
# pre-install ctest run.
#
# test_rosidl_buffer (3 and 4's own dependency, below) is deliberately NOT
# find_package(... REQUIRED)'d: package.xml's own comment already explains
# why it cannot be a normal test_depend (that package's build itself
# test-depends on rmw_cyclonedds_cpp, a circular dependency the rosdep/
# colcon graph cannot express). A REQUIRED find_package on an unresolvable
# circular dependency does not fail just those two targets -- it fails
# this whole file's cmake configure, which fails the WHOLE package's build,
# on any environment that resolves test dependencies the ordinary way and
# so never obtains it. 3 and 4 are gated on finding it; 1 and 2, which do
# not need it, build and run regardless.

find_package(ament_cmake_gtest REQUIRED)
find_package(rclcpp REQUIRED)
find_package(test_rosidl_buffer QUIET)
if(NOT test_rosidl_buffer_FOUND)
  message(STATUS
    "test_rosidl_buffer not found -- skipping test_buffer_descriptor_publish "
    "and test_buffer_descriptor_accuracy (see this file's own comment on "
    "why that package cannot be a normal find_package(... REQUIRED))")
endif()

# The throwaway test plugin itself (rmw_cyclonedds_cpp_test_buffer_backend_
# plugin, distinct from cuda_buffer_backend's "cuda" -- see
# cyclonedds_test_buffer_backend.hpp's own comment) is defined in the
# TOP-LEVEL CMakeLists.txt, not here -- see that file's own comment on why
# pluginlib_export_plugin_description_file() has to run in the same
# directory scope ament_package() does.

# 1. Tier 1: plain free-function unit tests, no plugin loading except the
# very last two cases (backend_locally_available), which is why this binary
# is ALSO run only after install rather than via a pre-install ctest -- see
# the file header above.
# Compiles BufferEndpointDiscovery.cpp a SECOND time, directly into this
# executable, rather than linking rmw_cyclonedds_cpp's own shared library:
# this project's rmw_* libraries default to hidden ELF visibility (see
# src/rmw_node.cpp's own `#pragma GCC visibility push (default)` around the
# handful of symbols it deliberately exports), and BufferEndpointDiscovery's
# free functions are not among them -- confirmed by an actual link failure
# ("undefined reference to rmw_cyclonedds_cpp::guid_to_hex"), not
# anticipated. Every function this binary tests is either pure (guid_to_hex,
# private_topic_name, advertises_non_cpu_backend, all_non_cpu_backend_types)
# or, for backend_locally_available, backed by a registry that is
# process-local regardless of which translation unit instantiated its
# static singleton -- so a second, statically-linked copy of this one file
# is a correctness-neutral way to reach symbols the shared library keeps
# private, without loosening that library's visibility for anyone else.
ament_add_gtest(test_buffer_endpoint_discovery_unit
  test_buffer_endpoint_discovery_unit.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/BufferEndpointDiscovery.cpp
)
if(TARGET test_buffer_endpoint_discovery_unit)
  target_include_directories(test_buffer_endpoint_discovery_unit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
  target_link_libraries(test_buffer_endpoint_discovery_unit
    ament_cmake_ros_core::ament_ros_cxx_standard
    CycloneDDS::ddsc
    rmw::rmw
    rosidl_buffer_backend::rosidl_buffer_backend
    rosidl_buffer_backend_registry::rosidl_buffer_backend_registry
  )
  add_dependencies(
    test_buffer_endpoint_discovery_unit rmw_cyclonedds_cpp_test_buffer_backend_plugin)
  install(TARGETS test_buffer_endpoint_discovery_unit
    DESTINATION lib/${PROJECT_NAME}
  )
endif()

# 2. Proves dynamic discovery via the real BufferBackendRegistry. Never
# links the plugin library or names its C++ class -- see the test file's
# own header comment for why that would defeat the point.
ament_add_gtest(test_cyclonedds_buffer_backend_registered
  test_cyclonedds_buffer_backend_registered.cpp
)
if(TARGET test_cyclonedds_buffer_backend_registered)
  target_link_libraries(test_cyclonedds_buffer_backend_registered
    rosidl_buffer_backend_registry::rosidl_buffer_backend_registry
  )
  add_dependencies(
    test_cyclonedds_buffer_backend_registered rmw_cyclonedds_cpp_test_buffer_backend_plugin)
  install(TARGETS test_cyclonedds_buffer_backend_registered
    DESTINATION lib/${PROJECT_NAME}
  )
endif()

# 3. The two named tests, against real rclcpp endpoints using
# test_rosidl_buffer's own ByteArray message (its `data` field is already a
# proven-compatible rosidl::Buffer<uint8_t> under rmw_cyclonedds_cpp, with
# the ordinary CPU-fallback scenario exercised for this RMW). Links the
# test backend plugin's counters DIRECTLY (not just via pluginlib dlopen)
# so this binary can read them -- the SAME shared object gets dlopen'd by
# BufferBackendRegistry at runtime, and a same-path dlopen of an
# already-loaded library reuses the existing image rather than mapping a
# second, disconnected copy of its statics.
# Default ament_add_gtest timeout (60s) is too tight for two TEST_F cases
# that can each legitimately wait up to kMatchTimeout for DDS discovery --
# measured live: the default cut the binary off mid-first-test with no
# gtest PASS/FAIL output at all, just "Timeout", which is a much less
# useful diagnostic than an actual failing assertion.
#
# SetUp() runs once PER TEST_F, not once for the whole binary, so its own
# one kMatchTimeout (15s) wait is paid once per test, not once overall.
# Worst case per test: test_private_topic_carries_descriptor_type pays
# SetUp's one wait plus its own one wait, two waits total (2 * 15s = 30s);
# test_publish_calls_create_descriptor_with_endpoint pays SetUp's one wait
# plus its own two waits, three waits total (3 * 15s = 45s). Both TEST_Fs
# run in the SAME ctest invocation for this binary, so the two totals ADD:
# 30s + 45s = 75s worst case. 90s leaves too little margin for that under
# real load; 150s gives roughly 2x headroom.
if(test_rosidl_buffer_FOUND)
  ament_add_gtest(test_buffer_descriptor_publish
    test_buffer_descriptor_publish.cpp
    TIMEOUT 150
  )
  if(TARGET test_buffer_descriptor_publish)
    target_include_directories(test_buffer_descriptor_publish PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(test_buffer_descriptor_publish
      rclcpp::rclcpp
      rmw_cyclonedds_cpp_test_buffer_backend_plugin
      ${test_rosidl_buffer_TARGETS}
    )
    install(TARGETS test_buffer_descriptor_publish
      DESTINATION lib/${PROJECT_NAME}
    )
  endif()
endif()

# 4. Byte-for-byte accuracy through the full publish -> wire -> receive
# path, possible in one process because the deadlock fix (this package's
# own base branch) removed the same-process pub+sub deadlock. Same linkage
# reasoning as target 3 above -- see its own comment.
# An earlier TIMEOUT 300 here undercounted -- publish_via_buffer_backend()'s
# retry loop checks its kFallbackRetryBudget (30s) deadline only at the TOP
# of each iteration, so a call that starts one more iteration just under
# the deadline can still run its own inner wait_for(..., kMatchTimeout)
# (15s) to completion -- worst case ~45s per call, not 30s.
# sequential_messages_do_not_reuse_stale_data calls it 5 times (SetUp's 15s
# wait + 5*45s = 240s); single_message_arrives_byte_for_byte calls it once
# (SetUp's 15s wait + 45s = 60s). Both TEST_Fs run in the one ctest
# invocation for this binary: worst case 300s, exactly at the old TIMEOUT
# with zero margin. 400s gives real headroom.
if(test_rosidl_buffer_FOUND)
  ament_add_gtest(test_buffer_descriptor_accuracy
    test_buffer_descriptor_accuracy.cpp
    TIMEOUT 400
  )
  if(TARGET test_buffer_descriptor_accuracy)
    target_include_directories(test_buffer_descriptor_accuracy PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(test_buffer_descriptor_accuracy
      rclcpp::rclcpp
      rmw_cyclonedds_cpp_test_buffer_backend_plugin
      ${test_rosidl_buffer_TARGETS}
    )
    install(TARGETS test_buffer_descriptor_accuracy
      DESTINATION lib/${PROJECT_NAME}
    )
  endif()
endif()
