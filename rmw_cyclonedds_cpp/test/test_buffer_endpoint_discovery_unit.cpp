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
// First-time gtest coverage for the plain, side-effect-free free functions
// BufferEndpointDiscovery.hpp exports. None of these need a DDS domain
// participant or a match -- dds_qos_t is a standalone object, and the
// registry/hex/name-building functions take plain values. This is Tier 1
// (no RMW involved), mirroring rosidl_buffer_backend_registry's own
// test/test_buffer_backend_registry.cpp.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "dds/dds.h"

#include "BufferEndpointDiscovery.hpp"

using rmw_cyclonedds_cpp::advertises_non_cpu_backend;
using rmw_cyclonedds_cpp::all_non_cpu_backend_types;
using rmw_cyclonedds_cpp::backend_locally_available;
using rmw_cyclonedds_cpp::guid_to_hex;
using rmw_cyclonedds_cpp::private_topic_name;

namespace
{

dds_guid_t make_guid(uint8_t fill)
{
  dds_guid_t guid;
  for (auto & byte : guid.v) {
    byte = fill;
  }
  return guid;
}

/// RAII wrapper so a failing EXPECT does not leak the dds_qos_t.
class ScopedQos
{
public:
  ScopedQos()
  : qos_(dds_create_qos()) {}
  ~ScopedQos() {dds_delete_qos(qos_);}
  dds_qos_t * get() {return qos_;}

private:
  dds_qos_t * qos_;
};

}  // namespace

TEST(GuidToHex, thirtytwo_lowercase_hex_chars)
{
  dds_guid_t guid = make_guid(0xAB);
  const std::string hex = guid_to_hex(guid);
  EXPECT_EQ(32u, hex.size());
  for (char c : hex) {
    EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) << "unexpected char: " << c;
  }
  std::string expected;
  for (int i = 0; i < 16; ++i) {
    expected += "ab";
  }
  EXPECT_EQ(expected, hex);
}

TEST(GuidToHex, distinguishes_different_guids)
{
  EXPECT_NE(guid_to_hex(make_guid(0x01)), guid_to_hex(make_guid(0x02)));
}

TEST(PrivateTopicName, appends_buf_and_hex_suffix)
{
  dds_guid_t guid = make_guid(0xff);
  const std::string name = private_topic_name("/chatter", guid);
  EXPECT_EQ("/chatter/_buf/" + guid_to_hex(guid), name);
}

TEST(PrivateTopicName, publisher_and_subscriber_sides_agree)
{
  // private_topic_name()'s own doc comment: this is a free function
  // specifically so the two sides can never independently drift on the
  // naming scheme. Calling it twice with the same inputs from what look
  // like two call sites must produce byte-identical names, or DDS's
  // name-based topic matching never actually connects the two sides.
  dds_guid_t guid = make_guid(0x42);
  const std::string publisher_side = rmw_cyclonedds_cpp::private_topic_name("/scan", guid);
  const std::string subscriber_side = rmw_cyclonedds_cpp::private_topic_name("/scan", guid);
  EXPECT_EQ(publisher_side, subscriber_side);
}

TEST(AdvertisesNonCpuBackend, absent_userdata_is_false)
{
  ScopedQos qos;
  EXPECT_FALSE(advertises_non_cpu_backend(qos.get()));
  EXPECT_TRUE(all_non_cpu_backend_types(qos.get()).empty());
}

TEST(AdvertisesNonCpuBackend, cpu_only_is_false)
{
  ScopedQos qos;
  const std::string user_data = "bufbackends=cpu;";
  dds_qset_userdata(qos.get(), user_data.data(), user_data.size());
  EXPECT_FALSE(advertises_non_cpu_backend(qos.get()));
  EXPECT_TRUE(all_non_cpu_backend_types(qos.get()).empty());
}

TEST(AdvertisesNonCpuBackend, one_non_cpu_backend_is_true)
{
  ScopedQos qos;
  const std::string user_data = "typehash=deadbeef;bufbackends=cpu,cyclonedds_test;";
  dds_qset_userdata(qos.get(), user_data.data(), user_data.size());
  EXPECT_TRUE(advertises_non_cpu_backend(qos.get()));
  const auto names = all_non_cpu_backend_types(qos.get());
  ASSERT_EQ(1u, names.size());
  EXPECT_EQ("cyclonedds_test", names[0]);
}

TEST(AdvertisesNonCpuBackend, several_non_cpu_backends_preserve_order)
{
  ScopedQos qos;
  const std::string user_data = "bufbackends=cpu,cuda,cyclonedds_test;";
  dds_qset_userdata(qos.get(), user_data.data(), user_data.size());
  const auto names = all_non_cpu_backend_types(qos.get());
  ASSERT_EQ(2u, names.size());
  EXPECT_EQ("cuda", names[0]);
  EXPECT_EQ("cyclonedds_test", names[1]);
}

// This only passes once the test plugin below is actually installed AND
// on AMENT_PREFIX_PATH -- see this package's own test/CMakeLists.txt
// comment on why this binary needs doInstallCheck, not doCheck.
TEST(BackendLocallyAvailable, finds_the_installed_test_plugin)
{
  EXPECT_TRUE(backend_locally_available("cyclonedds_test"));
}

TEST(BackendLocallyAvailable, does_not_find_an_unregistered_name)
{
  EXPECT_FALSE(backend_locally_available("no_such_backend_ever_12345"));
}

int main(int argc, char ** argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
