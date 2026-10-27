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

#include "cyclonedds_test_buffer_backend.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <pluginlib/class_list_macros.hpp>

#include "rosidl_typesupport_cpp/message_type_support.hpp"
#include "std_msgs/msg/u_int8_multi_array.hpp"

#include "cyclonedds_test_buffer_impl.hpp"

namespace rmw_cyclonedds_cpp
{
namespace test
{

namespace
{
std::atomic<std::size_t> g_get_descriptor_type_support_calls{0};
std::atomic<std::size_t> g_create_descriptor_calls{0};
std::atomic<std::int32_t> g_last_descriptor_value{-1};
}  // namespace

std::size_t get_descriptor_type_support_call_count()
{
  return g_get_descriptor_type_support_calls.load(std::memory_order_relaxed);
}

std::size_t create_descriptor_with_endpoint_call_count()
{
  return g_create_descriptor_calls.load(std::memory_order_relaxed);
}

std::int32_t last_created_descriptor_value()
{
  return g_last_descriptor_value.load(std::memory_order_relaxed);
}

void reset_test_backend_call_counters()
{
  g_get_descriptor_type_support_calls.store(0, std::memory_order_relaxed);
  g_create_descriptor_calls.store(0, std::memory_order_relaxed);
  g_last_descriptor_value.store(-1, std::memory_order_relaxed);
}

const rosidl_message_type_support_t * CyclonddsTestBufferBackend::get_descriptor_type_support()
const
{
  g_get_descriptor_type_support_calls.fetch_add(1, std::memory_order_relaxed);
  return rosidl_typesupport_cpp::get_message_type_support_handle<std_msgs::msg::UInt8MultiArray>();
}

std::shared_ptr<void> CyclonddsTestBufferBackend::create_empty_descriptor() const
{
  return std::make_shared<std_msgs::msg::UInt8MultiArray>();
}

std::shared_ptr<void> CyclonddsTestBufferBackend::create_descriptor_with_endpoint(
  const void * impl,
  const rmw_topic_endpoint_info_t & endpoint_info) const
{
  (void)endpoint_info;
  // The descriptor carries the buffer's REAL CONTENT, copied byte-for-byte,
  // not just its element count -- see this file's header comment. `impl`
  // is the type-erased BufferImplBase<uint8_t> pointer rmw_publish()
  // extracted from the actual ROS message; every producer this test suite
  // uses is a CyclonddsTestBufferImpl<uint8_t> (see
  // cyclonedds_test_buffer_impl.hpp), so this static_cast is safe FOR THIS
  // TEST SUITE'S OWN traffic only -- a real backend would need a documented
  // contract with its own producer side about what impl type to expect,
  // same as every other BufferBackend.
  const auto * test_impl = static_cast<const CyclonddsTestBufferImpl<std::uint8_t> *>(impl);
  auto descriptor = std::make_shared<std_msgs::msg::UInt8MultiArray>();
  descriptor->data = test_impl->get_storage();
  // last_created_descriptor_value() keeps its original element-count
  // semantics (the length of the real data the descriptor carries) so
  // tests that only need to confirm the call happened, and with what
  // count, are unaffected by carrying the real content too.
  g_last_descriptor_value.store(
    static_cast<std::int32_t>(descriptor->data.size()), std::memory_order_relaxed);
  g_create_descriptor_calls.fetch_add(1, std::memory_order_relaxed);
  return descriptor;
}

std::unique_ptr<void, void (*)(void *)> CyclonddsTestBufferBackend::from_descriptor_with_endpoint(
  const void * descriptor,
  const rmw_topic_endpoint_info_t & endpoint_info) const
{
  (void)endpoint_info;
  // Reconstructs the PUBLISHED bytes, not a same-sized zero-filled buffer
  // -- see this file's header comment. This is what makes a byte-for-byte
  // accuracy test possible across the real negotiate -> wire -> install
  // path.
  const auto & desc = *static_cast<const std_msgs::msg::UInt8MultiArray *>(descriptor);
  auto impl = std::make_unique<CyclonddsTestBufferImpl<std::uint8_t>>(desc.data);
  return {
    impl.release(),
    [](void * p) {delete static_cast<rosidl::BufferImplBase<std::uint8_t> *>(p);}
  };
}

}  // namespace test
}  // namespace rmw_cyclonedds_cpp

PLUGINLIB_EXPORT_CLASS(
  rmw_cyclonedds_cpp::test::CyclonddsTestBufferBackend,
  rosidl::BufferBackend)
