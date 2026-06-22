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

#include "example_interfaces/msg/w_string.hpp"
#include "example_interfaces/msg/detail/w_string__rosidl_typesupport_introspection_cpp.hpp"

TEST(SizeTypeSupportTest, WString)
{
  check_parity(GET_TS(example_interfaces, msg, WString));
}

TEST(SerializerTest, WStringEmpty)
{
  example_interfaces::msg::WString msg;
  msg.data = u"";
  check_serializer_roundtrip(GET_TS(example_interfaces, msg, WString), &msg);
}

TEST(SerializerTest, WStringPopulated)
{
  example_interfaces::msg::WString msg;
  msg.data = u"Hello \u4e16\u754c";  // "Hello 世界"
  check_serializer_roundtrip(GET_TS(example_interfaces, msg, WString), &msg);
}

TEST(DeserializerTest, WStringEmpty)
{
  example_interfaces::msg::WString msg;
  msg.data = u"";
  check_deserializer_roundtrip(GET_TS(example_interfaces, msg, WString), msg);
}

TEST(DeserializerTest, WStringPopulated)
{
  example_interfaces::msg::WString msg;
  msg.data = u"Hello \u4e16\u754c";  // "Hello 世界"
  check_deserializer_roundtrip(GET_TS(example_interfaces, msg, WString), msg);
}
