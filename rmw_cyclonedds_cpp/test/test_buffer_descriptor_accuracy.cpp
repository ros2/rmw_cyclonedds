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
// No existing test verifies that data crossing the zero-copy
// buffer-descriptor path arrives byte-for-byte identical to what was
// sent. test_buffer_descriptor_publish.cpp's two named tests only prove
// the MECHANISM -- that negotiation happened and
// create_descriptor_with_endpoint() was called with the right element
// COUNT. Neither one ever reads back what the SUBSCRIBER received,
// because before the deadlock fix this file's own base branch includes, a
// same-process publisher+subscriber pair for a buffer-backed topic
// deadlocked before either side could observe anything.
//
// This test exercises the FULL path in one process: publish -> real
// negotiated descriptor on the wire -> rmw_take_int()'s install of the
// backend-reconstructed buffer into the received ROS message's own buffer
// field -- and asserts the received bytes match what was published, not
// just that a descriptor of the right size arrived. That assertion is
// only meaningful because cyclonedds_test_buffer_backend.cpp's own
// create_descriptor_with_endpoint()/from_descriptor_with_endpoint() carry
// real content instead of a same-sized zero-filled buffer -- see that
// file's header comment.
//
// Covers two cases: a single message with a known, verifiable pattern,
// and a sequence of several messages each with DIFFERENT content, to rule
// out a backend that could silently return a prior message's (stale or
// cached) data instead of the current one -- the per-peer
// pending-descriptor cache correlates by source_timestamp specifically to
// prevent that, and this is the test that would catch a regression in
// that correlation.
//
// WHY EACH PUBLISH, NOT JUST THE FIRST ONE, RETRIES ON CPU-FALLBACK: a
// publish can legitimately land before the background half of
// finish_publication_match() has populated private_writers_or_readers --
// test_buffer_descriptor_publish.cpp's own header comment already
// documents this for its one publish-and-retry test ("A single publish can
// land in that window and legitimately fall back... rather than assume one
// publish is enough once the earlier, weaker signal has"). Measured live:
// that window is not a one-time startup race. A publish made well after an
// EARLIER publish on the same peer had already negotiated successfully
// still arrived on backend "cpu", not "cyclonedds_test" -- so SetUp cannot
// confirm the path is clear once and move on; every publish has to
// confirm its OWN delivery.
//
// publish_via_buffer_backend() below does that confirmation by inspecting
// the backend type the delivery it just received actually carries, and
// discarding (not just ignoring) a CPU-fallback one before retrying with
// the same content -- the one fact that settles the question, rather than
// inferring it from when negotiation was first observed.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
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
// Same generosity as test_buffer_descriptor_publish.cpp's own
// kMatchTimeout -- the deferred background-retry mechanism can take
// several graph-cache-refresh cycles beyond the raw DDS match.
constexpr auto kMatchTimeout = std::chrono::seconds(15);
// Bounds the OUTER retry-on-fallback loop in publish_via_buffer_backend()
// below -- generous enough to absorb several consecutive fallback misses,
// short enough that a genuinely broken negotiation fails this test in well
// under a minute rather than only at the ctest-level TIMEOUT with no
// assertion message.
constexpr auto kFallbackRetryBudget = std::chrono::seconds(30);

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

class BufferDescriptorAccuracyTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    ASSERT_EQ(0, setenv("RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS", "cyclonedds_test", 1));
    rmw_cyclonedds_cpp::test::reset_test_backend_call_counters();

    node_ = std::make_shared<rclcpp::Node>("buffer_descriptor_accuracy_test_362");

    rclcpp::QoS qos(10);
    publisher_ = node_->create_publisher<test_rosidl_buffer::msg::ByteArray>(
      "test_buffer_descriptor_accuracy_362", qos);
    subscription_ = node_->create_subscription<test_rosidl_buffer::msg::ByteArray>(
      "test_buffer_descriptor_accuracy_362", qos,
      [this](const test_rosidl_buffer::msg::ByteArray & msg) {
        // Recording seq (rather than matching a reply to its own publish
        // purely by vector POSITION -- size_before/back()/pop_back())
        // lets lookups below key on the message's own identity instead of
        // arrival order, so a late or duplicate delivery can never be
        // mistaken for an earlier, already-discarded attempt.
        std::lock_guard<std::mutex> lock(received_mutex_);
        deliveries_.push_back({msg.seq, msg.data.get_backend_type(), msg.data.to_vector()});
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

  /// Keying every delivery by its own seq (set below, unique per logical
  /// message across both TEST_Fs) removes any ordering assumption: a
  /// naive match by vector POSITION (a size-before snapshot, then
  /// back()/pop_back()) risks a late delivery from an already-abandoned
  /// retry, or a genuine duplicate, landing after the real one and being
  /// mistaken for it. find_delivery_locked() looks up by identity, not
  /// position, so a duplicate or late arrival for a DIFFERENT seq simply
  /// sits unused in deliveries_ instead of being mismatched to this one.
  ///
  /// Publishes `pattern` under `seq` and does not return until a delivery
  /// for that seq genuinely arrives via the negotiated ("cyclonedds_test")
  /// backend -- retrying the identical publish whenever the only delivery
  /// seen for this seq so far is a "cpu" fallback, per this file's own
  /// header comment on why that can happen more than once.
  bool publish_via_buffer_backend(const std::vector<std::uint8_t> & pattern, std::uint32_t seq)
  {
    const auto deadline = std::chrono::steady_clock::now() + kFallbackRetryBudget;
    while (std::chrono::steady_clock::now() < deadline) {
      test_rosidl_buffer::msg::ByteArray msg;
      msg.seq = seq;
      msg.data = rosidl::Buffer<std::uint8_t>(
        std::make_unique<rmw_cyclonedds_cpp::test::CyclonddsTestBufferImpl<std::uint8_t>>(
          pattern));
      publisher_->publish(msg);

      const bool delivered = wait_for(
        node_,
        [this, seq] {
          std::lock_guard<std::mutex> lock(received_mutex_);
          const Delivery * d = find_delivery_locked(seq);
          return d != nullptr && d->backend_type == "cyclonedds_test";
        },
        kMatchTimeout);
      if (delivered) {
        return true;
      }
      // No cyclonedds_test delivery for this seq within kMatchTimeout --
      // whatever arrived for it (if anything) was a cpu fallback, which
      // find_delivery() below will simply be superseded by on the next
      // iteration's real delivery. Retry with the identical content.
    }
    return false;
  }

  struct Delivery
  {
    std::uint32_t seq;
    std::string backend_type;
    std::vector<std::uint8_t> bytes;
  };

  /// Returns the delivery recorded for `seq` that a caller should actually
  /// read: the real, negotiated-backend one if one has arrived, else the
  /// most recent cpu-fallback one so an assertion reports exactly what
  /// happened rather than a null/out-of-range read. Caller holds
  /// received_mutex_.
  const Delivery * find_delivery_locked(std::uint32_t seq) const
  {
    const Delivery * fallback = nullptr;
    for (const auto & d : deliveries_) {
      if (d.seq != seq) {continue;}
      if (d.backend_type == "cyclonedds_test") {return &d;}
      fallback = &d;
    }
    return fallback;
  }

  rclcpp::Node::SharedPtr node_;
  rclcpp::Publisher<test_rosidl_buffer::msg::ByteArray>::SharedPtr publisher_;
  rclcpp::Subscription<test_rosidl_buffer::msg::ByteArray>::SharedPtr subscription_;

  std::mutex received_mutex_;
  std::vector<Delivery> deliveries_;
};

}  // namespace

TEST_F(BufferDescriptorAccuracyTest, single_message_arrives_byte_for_byte)
{
  // A distinctive, non-repeating pattern -- not a fill value -- so a bug
  // that scrambles byte order, truncates, or pads would be caught by more
  // than a size check.
  std::vector<std::uint8_t> sent;
  for (int i = 0; i < 64; ++i) {
    sent.push_back(static_cast<std::uint8_t>((i * 37 + 11) & 0xFF));
  }

  ASSERT_TRUE(publish_via_buffer_backend(sent, 1))
    << "no delivery of this message ever arrived via the negotiated "
    << "backend, even after retrying the fallback window";

  std::lock_guard<std::mutex> lock(received_mutex_);
  const Delivery * d = find_delivery_locked(1);
  ASSERT_NE(nullptr, d) << "no delivery recorded for seq=1 at all";
  EXPECT_EQ("cyclonedds_test", d->backend_type);
  EXPECT_EQ(sent, d->bytes)
    << "received bytes do not match what was published -- the zero-copy "
    << "path delivered the wrong, corrupt, or truncated data";
}

TEST_F(BufferDescriptorAccuracyTest, sequential_messages_do_not_reuse_stale_data)
{
  // Five messages, each with content that depends on its own index so no
  // two are the same and none is a zero-fill the backend's descriptor
  // path could silently reconstruct by accident. If the backend (or the
  // pending-descriptor correlation) ever returns a PRIOR message's data
  // instead of the current one, this test ends up with at least one
  // mismatched pair.
  constexpr int kMessageCount = 5;
  std::vector<std::vector<std::uint8_t>> sent_each;

  for (int msg_index = 0; msg_index < kMessageCount; ++msg_index) {
    std::vector<std::uint8_t> pattern;
    for (int i = 0; i < 16; ++i) {
      pattern.push_back(static_cast<std::uint8_t>((msg_index * 53 + i * 7 + 3) & 0xFF));
    }
    sent_each.push_back(pattern);

    ASSERT_TRUE(publish_via_buffer_backend(pattern, msg_index + 1))
      << "message #" << msg_index << " never arrived via the negotiated backend";
  }

  std::lock_guard<std::mutex> lock(received_mutex_);
  for (int msg_index = 0; msg_index < kMessageCount; ++msg_index) {
    const Delivery * d = find_delivery_locked(static_cast<std::uint32_t>(msg_index + 1));
    ASSERT_NE(nullptr, d) << "no delivery recorded for message #" << msg_index << " at all";
    EXPECT_EQ("cyclonedds_test", d->backend_type);
    EXPECT_EQ(sent_each[msg_index], d->bytes)
      << "message #" << msg_index << " arrived with the wrong content -- "
      << "either corrupted or a stale/cached copy of a different message";
  }
}

int main(int argc, char ** argv)
{
  testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int result = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return result;
}
