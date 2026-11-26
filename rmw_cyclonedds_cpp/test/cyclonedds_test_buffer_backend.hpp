// Copyright 2026 Open Source Robotics Foundation, Inc.
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
//
// Throwaway/minimal real rosidl::BufferBackend test plugin for this
// package's own gtest suite -- distinct from cuda_buffer_backend
// (get_backend_type() "cyclonedds_test", not "cuda"; a different
// pluginlib class name; its own plugins.xml). Its descriptor type is
// std_msgs/msg/UInt8MultiArray, not a purpose-built message, so this
// package does not need its own rosidl_generate_interfaces() step just to
// get a working gtest -- std_msgs is already a properly-exported, standard
// ament package.
//
// The descriptor copies the buffer's real content both ways (not just its
// element count), so test_buffer_descriptor_accuracy.cpp can assert
// byte-for-byte equality through the real negotiate -> wire -> install
// path, not just a count match. last_created_descriptor_value() keeps
// returning the element count (data.size()) so tests that only need to
// confirm the call happened, and with what count, are unaffected.

#ifndef CYCLONEDDS_TEST_BUFFER_BACKEND_HPP_
#define CYCLONEDDS_TEST_BUFFER_BACKEND_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "rmw/topic_endpoint_info.h"
#include "rosidl_buffer_backend/buffer_backend.hpp"

namespace rmw_cyclonedds_cpp
{
namespace test
{

/// Process-wide, thread-safe call counters -- the actual "did the production
/// code call our backend, and with what" observation surface for
/// test/test_buffer_descriptor_publish.cpp's two named tests. This is the
/// alternative to adding a test-only introspection hook to rmw_node.cpp
/// itself: the private buffer-backend topic these two tests exercise is not
/// reachable from ANY public rmw/rclcpp API (see that test file's own header
/// comment), but the BACKEND CONTRACT -- which methods rmw_node.cpp calls,
/// in what order, with what arguments -- is directly observable from inside
/// the backend plugin itself, with zero changes to production code.
///
/// get_descriptor_type_support() is called exactly once per newly-matched
/// peer, from finish_publication_match()/finish_subscription_match(), to
/// resolve the private topic's sertype -- a call count of zero after a real
/// match means the code path that is supposed to use this backend's
/// descriptor type never ran (e.g. a regression reverting to the original
/// message type instead, which never calls this method at all).
std::size_t get_descriptor_type_support_call_count();

/// create_descriptor_with_endpoint() is called once per publish, per
/// negotiated peer -- see rmw_publish()'s own comment on why. Records both
/// how many times it fired and the last value it computed, so a test can
/// assert both "it was called" and "it was called with the right content".
std::size_t create_descriptor_with_endpoint_call_count();
std::int32_t last_created_descriptor_value();

void reset_test_backend_call_counters();

class CyclonddsTestBufferBackend : public rosidl::BufferBackend
{
public:
  CyclonddsTestBufferBackend() = default;
  ~CyclonddsTestBufferBackend() override = default;

  std::string get_backend_type() const override {return "cyclonedds_test";}

  const rosidl_message_type_support_t * get_descriptor_type_support() const override;

  std::shared_ptr<void> create_empty_descriptor() const override;

  std::shared_ptr<void> create_descriptor_with_endpoint(
    const void * impl,
    const rmw_topic_endpoint_info_t & endpoint_info) const override;

  std::unique_ptr<void, void (*)(void *)> from_descriptor_with_endpoint(
    const void * descriptor,
    const rmw_topic_endpoint_info_t & endpoint_info) const override;
};

}  // namespace test
}  // namespace rmw_cyclonedds_cpp

#endif  // CYCLONEDDS_TEST_BUFFER_BACKEND_HPP_
