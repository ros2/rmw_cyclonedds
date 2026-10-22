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
// Two named tests -- test_private_topic_carries_descriptor_type and
// test_publish_calls_create_descriptor_with_endpoint -- exercised against
// the REAL rmw_cyclonedds_cpp publish-side descriptor-path code
// (finish_publication_match()/rmw_publish() in src/rmw_node.cpp), through
// two ordinary rclcpp endpoints, not a mock.
//
// WHY THIS DOES NOT SNOOP THE PRIVATE DDS TOPIC DIRECTLY: the private topic
// this mechanism creates (private_topic_name() in BufferEndpointDiscovery.
// hpp: base_topic + "/_buf/" + peer-guid-hex, built directly from the
// negotiated backend's own type support, bypassing rmw_create_subscription's
// name-mangling entirely) has no public rmw/rclcpp entry point that can
// subscribe to it from outside this RMW -- there is no "create a raw,
// unmangled-name subscription with this exact sertype" call in the public
// API, and create_sertype()/create_msg_dds_dynamic_type() are file-static to
// rmw_node.cpp, not exported. Building one would mean either duplicating a
// meaningful slice of this file's own internals in test code (itself a
// maintenance and correctness risk -- the duplicate would have to be kept
// in sync with rmw_node.cpp by hand) or adding a new test-only exported
// symbol to a hot, hardened, and heavily-reviewed production file.
//
// Instead, this test observes the SAME contract from the other side: the
// BufferBackend interface these two production functions actually call.
// cyclonedds_test_buffer_backend.{hpp,cpp} is test-only code (nothing here
// touches src/), and its call counters directly answer both named
// questions:
//   - get_descriptor_type_support() is called (from finish_publication_
//     match(), the deferred-thread body on_publication_matched() dispatches
//     to -- see its own comment) ONCE per newly-matched peer, to build
//     the private topic's sertype. A regression that reverts to using the
//     original message type's sertype instead never calls this method at
//     all -- a call count of zero after a real match is a direct,
//     unambiguous witness of exactly that regression class. Combined with
//     cyclonedds_test_buffer_backend.cpp's own source (not a separate test
//     -- nothing in this test suite asserts the return type independently)
//     always returning std_msgs/msg/UInt8MultiArray's real type support
//     (never nullptr, never the buffer-backed message's own type), a
//     nonzero call count here is "the private writer was built from the
//     resolved backend's descriptor type" stated the way the production
//     code can actually be observed making that choice.
//   - create_descriptor_with_endpoint() is called (from rmw_publish(), see
//     its own comment) once per publish per negotiated peer, with the
//     REAL BufferImplBase<uint8_t> pulled out of the REAL published
//     message via find_buffer_impl(). Asserting both the call count and
//     the value it computed pins the actual data path, including the
//     deliberate "only the first buffer-backed field, one descriptor per
//     write" scope boundary -- a regression truncating input differently,
//     or skipping the call outright, changes what this test observes.
//
// Both tests below previously hung in SetUp() -- creating a publisher and a
// subscription for the SAME buffer-backed topic in ONE process, with a
// non-CPU backend actually negotiated, deadlocked inside
// rmw_create_subscription() on every run. Root cause: a LOCAL
// (same-process) peer match dispatches on_publication_matched()/
// on_subscription_matched() synchronously, reentrant, on the thread already
// inside the outer dds_create_reader()/dds_create_writer() call -- and the
// match handler's own nested entity creation for the private topic then
// tried to re-enter CycloneDDS's internal, non-reentrant entity-creation
// lock that same thread already held. Fixed by having on_publication_
// matched()/on_subscription_matched() defer finish_publication_match()/
// finish_subscription_match() onto a detached background thread (see their
// own comments), the same pattern the deferred discovery-retry mechanism
// already used.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rclcpp/rclcpp.hpp"
#include "rosidl_buffer/buffer.hpp"
#include "test_rosidl_buffer/msg/byte_array.hpp"

#include "cyclonedds_test_buffer_backend.hpp"
#include "cyclonedds_test_buffer_impl.hpp"

namespace
{

constexpr auto kPollInterval = std::chrono::milliseconds(50);
// The deferred background-retry mechanism can take several
// graph-cache-refresh cycles beyond the raw DDS match; generous on purpose
// so this test's own timeout is never the flaky part.
constexpr auto kMatchTimeout = std::chrono::seconds(15);

template<typename Predicate>
bool wait_for(rclcpp::Node::SharedPtr node, Predicate predicate, std::chrono::seconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    rclcpp::spin_some(node);
    std::this_thread::sleep_for(kPollInterval);
  }
  return predicate();
}

class BufferDescriptorPublishTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    // Read once, at endpoint-creation time, by create_readwrite_qos() in
    // rmw_node.cpp -- must be set before the publisher/subscription below
    // are created, not merely before publish.
    ASSERT_EQ(0, setenv("RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS", "cyclonedds_test", 1));
    rmw_cyclonedds_cpp::test::reset_test_backend_call_counters();

    node_ = std::make_shared<rclcpp::Node>("buffer_descriptor_publish_test_311");

    rclcpp::QoS qos(10);
    publisher_ = node_->create_publisher<test_rosidl_buffer::msg::ByteArray>(
      "test_buffer_descriptor_311", qos);
    subscription_ = node_->create_subscription<test_rosidl_buffer::msg::ByteArray>(
      "test_buffer_descriptor_311", qos,
      [](const test_rosidl_buffer::msg::ByteArray &) {
        // The consuming side of the private reader does not feed back
        // into this ordinary callback -- this test only needs the
        // PUBLISHER side's negotiation and publish path, so an empty
        // callback is sufficient; its only job is to make this process's
        // own subscription a real, matchable buffer-backed peer.
      });

    ASSERT_TRUE(
      wait_for(
        node_, [this] {return publisher_->get_subscription_count() > 0;}, kMatchTimeout))
      << "publisher and subscription never matched at the ordinary DDS level";
  }

  void TearDown() override
  {
    subscription_.reset();
    publisher_.reset();
    node_.reset();
    unsetenv("RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS");
  }

  rclcpp::Node::SharedPtr node_;
  rclcpp::Publisher<test_rosidl_buffer::msg::ByteArray>::SharedPtr publisher_;
  rclcpp::Subscription<test_rosidl_buffer::msg::ByteArray>::SharedPtr subscription_;
};

}  // namespace

TEST_F(BufferDescriptorPublishTest, test_private_topic_carries_descriptor_type)
{
  using rmw_cyclonedds_cpp::test::get_descriptor_type_support_call_count;

  ASSERT_TRUE(
    wait_for(
      node_, [] {return get_descriptor_type_support_call_count() > 0;}, kMatchTimeout))
    << "finish_publication_match() never called this backend's own "
    << "get_descriptor_type_support() -- the private writer was not built "
    << "from the resolved backend's descriptor type (regression: reverted "
    << "to the original message type's sertype, or the private-topic "
    << "negotiation did not happen at all)";
}

TEST_F(BufferDescriptorPublishTest, test_publish_calls_create_descriptor_with_endpoint)
{
  using rmw_cyclonedds_cpp::test::create_descriptor_with_endpoint_call_count;
  using rmw_cyclonedds_cpp::test::last_created_descriptor_value;
  using rmw_cyclonedds_cpp::test::CyclonddsTestBufferImpl;

  // Wait for the match/negotiation to actually complete (same signal as the
  // other test) before publishing -- a publish before finish_publication_
  // match() has run has an empty private_writers_or_readers map, so
  // rmw_publish()'s buffer-backend branch has nothing to iterate.
  ASSERT_TRUE(
    wait_for(
      node_,
      [] {return rmw_cyclonedds_cpp::test::get_descriptor_type_support_call_count() > 0;},
      kMatchTimeout));

  constexpr std::size_t kElementCount = 37;  // arbitrary, distinctive

  // finish_publication_match()'s own private-topic creation is deferred
  // onto a background thread. get_descriptor_type_support_call_count()
  // (waited on above) increments EARLY in that function, but
  // buffer_discovery.private_writers_or_readers.emplace() -- the entry
  // rmw_publish() actually needs to find before it will call create_
  // descriptor_with_endpoint() -- happens LATER in the same function,
  // after a nested dds_create_writer() call. A single publish can land in
  // that window and legitimately fall back to the ordinary (non-buffer)
  // write path, the same documented behavior as publishing before any
  // match at all. Retry the publish until the real signal fires, rather
  // than assume one publish is enough once the earlier, weaker signal has.
  ASSERT_TRUE(
    wait_for(
      node_,
      [this] {
        if (create_descriptor_with_endpoint_call_count() > 0) {
          return true;
        }
        test_rosidl_buffer::msg::ByteArray msg;
        msg.seq = 1;
        msg.data = rosidl::Buffer<std::uint8_t>(
          std::make_unique<CyclonddsTestBufferImpl<std::uint8_t>>(
            std::vector<std::uint8_t>(kElementCount, 0xAB)));
        publisher_->publish(msg);
        return create_descriptor_with_endpoint_call_count() > 0;
      },
      kMatchTimeout))
    << "rmw_publish() never called create_descriptor_with_endpoint() on the "
    << "negotiated backend for a buffer-backed publish, across repeated "
    << "retries";
  EXPECT_EQ(static_cast<std::int32_t>(kElementCount), last_created_descriptor_value())
    << "the descriptor's content does not match the published buffer's own "
    << "element count -- rmw_publish() extracted the wrong impl, or this "
    << "backend's create_descriptor_with_endpoint() received the wrong one";
}

int main(int argc, char ** argv)
{
  testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int result = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return result;
}
