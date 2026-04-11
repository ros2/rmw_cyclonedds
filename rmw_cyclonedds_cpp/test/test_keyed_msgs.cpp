// Copyright 2019 Rover Robotics via Dan Rose
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

#include "test_helpers.hpp"

#include "test_msgs/msg/keyed_long.hpp"
#include "test_msgs/msg/keyed_string.hpp"
#include "test_msgs/msg/complex_nested_key.hpp"
#include "test_msgs/msg/detail/keyed_long__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/keyed_string__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/complex_nested_key__rosidl_typesupport_introspection_cpp.hpp"

// ---------------------------------------------------------------------------
// KeyedLong: @key int32, non-key int32
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, KeyedLong_KeySizeParity)
{
  check_key_size_parity(GET_TS(test_msgs, msg, KeyedLong));
}

TEST(SizeTypeSupportTest, KeyedLong_KeySizeAbsolute)
{
  // key = int32: 4-byte CDR header + 4-byte int32 = 8
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(test_msgs, msg, KeyedLong));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Key), 8u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Key), 8u);
}

TEST(SerializerTest, KeyedLong_KeySerialize)
{
  test_msgs::msg::KeyedLong msg;
  msg.key = 42;
  msg.value = 99;
  check_key_serialize(GET_TS(test_msgs, msg, KeyedLong), msg);
}

TEST(DeserializerTest, KeyedLong_ExtractKey)
{
  test_msgs::msg::KeyedLong msg;
  msg.key = 42;
  msg.value = 99;
  check_extractkey(GET_TS(test_msgs, msg, KeyedLong), msg);
}

TEST(DeserializerTest, KeyedLong_ExtractKeyBE)
{
  test_msgs::msg::KeyedLong msg;
  msg.key = 42;
  msg.value = 99;
  check_extractkey_be(GET_TS(test_msgs, msg, KeyedLong), msg);
}

TEST(DeserializerTest, KeyedLong_KeyOnlyDeserialize)
{
  test_msgs::msg::KeyedLong msg;
  msg.key = 1234;
  msg.value = 5678;

  MessageMembersVariant members =
    make_message_members_variant(GET_TS(test_msgs, msg, KeyedLong));
  CDRSerializer serializer(members, SampleOrRequest::Sample);
  CDRDeserializer deserializer(members, SampleOrRequest::Sample);

  size_t key_sz = serializer.get_serialized_size(&msg, SampleOrKey::Key);
  std::vector<unsigned char> key_buf(key_sz, 0);
  serializer.serialize(key_buf.data(), &msg, SampleOrKey::Key);

  test_msgs::msg::KeyedLong restored{};
  deserializer.deserialize(&restored, key_buf.data(), key_sz, SampleOrKey::Key);
  EXPECT_EQ(restored.key, msg.key);
}

// ---------------------------------------------------------------------------
// KeyedString: @key string, non-key string
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, KeyedString_KeySizeParity)
{
  check_key_size_parity(GET_TS(test_msgs, msg, KeyedString));
}

TEST(SizeTypeSupportTest, KeyedString_KeyMinSizeUnbounded)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(test_msgs, msg, KeyedString));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Key), SIZE_MAX);
}

TEST(SerializerTest, KeyedString_KeySerialize)
{
  test_msgs::msg::KeyedString msg;
  msg.key = "robot_1";
  msg.value = "ignored_in_key";
  check_key_serialize(GET_TS(test_msgs, msg, KeyedString), msg);
}

TEST(DeserializerTest, KeyedString_ExtractKey)
{
  test_msgs::msg::KeyedString msg;
  msg.key = "robot_1";
  msg.value = "ignored_in_key";
  check_extractkey(GET_TS(test_msgs, msg, KeyedString), msg);
}

TEST(DeserializerTest, KeyedString_ExtractKeyBE)
{
  test_msgs::msg::KeyedString msg;
  msg.key = "robot_1";
  msg.value = "ignored_in_key";
  check_extractkey_be(GET_TS(test_msgs, msg, KeyedString), msg);
}

// ---------------------------------------------------------------------------
// ComplexNestedKey: @key uint32, @key nested struct, non-key double
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, ComplexNestedKey_KeySizeParity)
{
  check_key_size_parity(GET_TS(test_msgs, msg, ComplexNestedKey));
}

TEST(SerializerTest, ComplexNestedKey_KeySerialize)
{
  test_msgs::msg::ComplexNestedKey msg;
  msg.uint32_key = 7;
  msg.float64_value = 3.14;
  check_key_serialize(GET_TS(test_msgs, msg, ComplexNestedKey), msg);
}

TEST(DeserializerTest, ComplexNestedKey_ExtractKey)
{
  test_msgs::msg::ComplexNestedKey msg;
  msg.uint32_key = 7;
  msg.float64_value = 3.14;
  check_extractkey(GET_TS(test_msgs, msg, ComplexNestedKey), msg);
}

TEST(DeserializerTest, ComplexNestedKey_ExtractKeyBE)
{
  test_msgs::msg::ComplexNestedKey msg;
  msg.uint32_key = 7;
  msg.float64_value = 3.14;
  check_extractkey_be(GET_TS(test_msgs, msg, ComplexNestedKey), msg);
}
