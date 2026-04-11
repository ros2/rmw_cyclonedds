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

#include "rcl_interfaces/msg/parameter_type.hpp"
#include "rcl_interfaces/msg/parameter_value.hpp"
#include "rcl_interfaces/msg/parameter.hpp"
#include "rcl_interfaces/msg/parameter_event.hpp"
#include "rcl_interfaces/msg/parameter_descriptor.hpp"
#include "rcl_interfaces/msg/set_parameters_result.hpp"
#include "rcl_interfaces/msg/detail/parameter_value__rosidl_typesupport_introspection_cpp.hpp"
#include "rcl_interfaces/msg/detail/parameter__rosidl_typesupport_introspection_cpp.hpp"
#include "rcl_interfaces/msg/detail/parameter_event__rosidl_typesupport_introspection_cpp.hpp"
#include "rcl_interfaces/msg/detail/parameter_descriptor__rosidl_typesupport_introspection_cpp.hpp"
#include "rcl_interfaces/msg/detail/set_parameters_result__rosidl_typesupport_introspection_cpp.hpp"

extern "C" {
#include "rcl_interfaces/msg/detail/parameter_value__rosidl_typesupport_introspection_c.h"
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — size bounds parity
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, RclParameterValue)
{
  check_parity(GET_TS(rcl_interfaces, msg, ParameterValue));
}

TEST(SizeTypeSupportTest, RclParameter)
{
  check_parity(GET_TS(rcl_interfaces, msg, Parameter));
}

TEST(SizeTypeSupportTest, RclParameterEvent)
{
  check_parity(GET_TS(rcl_interfaces, msg, ParameterEvent));
}

TEST(SizeTypeSupportTest, RclParameterDescriptor)
{
  check_parity(GET_TS(rcl_interfaces, msg, ParameterDescriptor));
}

TEST(SizeTypeSupportTest, RclSetParametersResult)
{
  check_parity(GET_TS(rcl_interfaces, msg, SetParametersResult));
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — concrete data size checks
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, RclParameterValueEmptyData)
{
  rcl_interfaces::msg::ParameterValue msg;
  check_parity_with_data(GET_TS(rcl_interfaces, msg, ParameterValue), &msg);
}

TEST(SizeTypeSupportTest, RclParameterValueWithBoolArray)
{
  rcl_interfaces::msg::ParameterValue msg;
  msg.type = rcl_interfaces::msg::ParameterType::PARAMETER_BOOL_ARRAY;
  msg.bool_array_value = {true, false, true, true, false};
  check_parity_with_data(GET_TS(rcl_interfaces, msg, ParameterValue), &msg);
}

TEST(SizeTypeSupportTest, RclParameterValueWithString)
{
  rcl_interfaces::msg::ParameterValue msg;
  msg.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING;
  msg.string_value = "hello_parameter";
  check_parity_with_data(GET_TS(rcl_interfaces, msg, ParameterValue), &msg);
}

TEST(SizeTypeSupportTest, RclParameterValueWithIntegerArray)
{
  rcl_interfaces::msg::ParameterValue msg;
  msg.type = rcl_interfaces::msg::ParameterType::PARAMETER_INTEGER_ARRAY;
  msg.integer_array_value = {1, 2, 3, 4, 5};
  check_parity_with_data(GET_TS(rcl_interfaces, msg, ParameterValue), &msg);
}

TEST(SizeTypeSupportTest, RclParameterValueWithStringArray)
{
  rcl_interfaces::msg::ParameterValue msg;
  msg.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING_ARRAY;
  msg.string_array_value = {"foo", "bar", "baz"};
  check_parity_with_data(GET_TS(rcl_interfaces, msg, ParameterValue), &msg);
}

TEST(SizeTypeSupportTest, RclParameterEventEmpty)
{
  rcl_interfaces::msg::ParameterEvent msg;
  check_parity_with_data(GET_TS(rcl_interfaces, msg, ParameterEvent), &msg);
}

TEST(SizeTypeSupportTest, RclParameterEventPopulated)
{
  rcl_interfaces::msg::ParameterEvent msg;
  msg.node = "/my_node";
  msg.stamp.sec = 100;
  msg.stamp.nanosec = 0u;
  rcl_interfaces::msg::Parameter p;
  p.name = "my_param";
  p.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING;
  p.value.string_value = "value";
  msg.new_parameters.push_back(p);
  rcl_interfaces::msg::Parameter p2;
  p2.name = "bool_array_param";
  p2.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_BOOL_ARRAY;
  p2.value.bool_array_value = {true, false, true};
  msg.changed_parameters.push_back(p2);
  check_parity_with_data(GET_TS(rcl_interfaces, msg, ParameterEvent), &msg);
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — C-type path
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, CType_ParameterValue)
{
  check_parity(GET_TS_C(rcl_interfaces, msg, ParameterValue));
}

// ---------------------------------------------------------------------------
// SerializerTest
// ---------------------------------------------------------------------------

TEST(SerializerTest, RclParameterValueEmpty)
{
  rcl_interfaces::msg::ParameterValue msg;
  check_serializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterValue), &msg);
}

TEST(SerializerTest, RclParameterValueWithBoolArray)
{
  rcl_interfaces::msg::ParameterValue msg;
  msg.type = rcl_interfaces::msg::ParameterType::PARAMETER_BOOL_ARRAY;
  msg.bool_array_value = {true, false, true, true, false};
  check_serializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterValue), &msg);
}

TEST(SerializerTest, RclParameterValueWithStringArray)
{
  rcl_interfaces::msg::ParameterValue msg;
  msg.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING_ARRAY;
  msg.string_array_value = {"alpha", "beta", "gamma"};
  check_serializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterValue), &msg);
}

TEST(SerializerTest, RclParameterEventEmpty)
{
  rcl_interfaces::msg::ParameterEvent msg;
  check_serializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterEvent), &msg);
}

TEST(SerializerTest, RclParameterEventPopulated)
{
  rcl_interfaces::msg::ParameterEvent msg;
  msg.node = "/some_node";
  rcl_interfaces::msg::Parameter p;
  p.name = "flag";
  p.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_BOOL_ARRAY;
  p.value.bool_array_value = {false, true};
  msg.new_parameters.push_back(p);
  check_serializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterEvent), &msg);
}

// ---------------------------------------------------------------------------
// DeserializerTest
// ---------------------------------------------------------------------------

TEST(DeserializerTest, RclParameterValueEmpty)
{
  rcl_interfaces::msg::ParameterValue msg;
  check_deserializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterValue), msg);
}

TEST(DeserializerTest, RclParameterValueWithBoolArray)
{
  rcl_interfaces::msg::ParameterValue msg;
  msg.type = rcl_interfaces::msg::ParameterType::PARAMETER_BOOL_ARRAY;
  msg.bool_array_value = {true, false, true, true, false};
  check_deserializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterValue), msg);
}

TEST(DeserializerTest, RclParameterValueWithStringArray)
{
  rcl_interfaces::msg::ParameterValue msg;
  msg.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING_ARRAY;
  msg.string_array_value = {"alpha", "beta"};
  check_deserializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterValue), msg);
}

TEST(DeserializerTest, RclParameterEventEmpty)
{
  rcl_interfaces::msg::ParameterEvent msg;
  check_deserializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterEvent), msg);
}

TEST(DeserializerTest, RclParameterEventPopulated)
{
  rcl_interfaces::msg::ParameterEvent msg;
  msg.node = "/some_node";
  rcl_interfaces::msg::Parameter p;
  p.name = "my_flag";
  p.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_BOOL_ARRAY;
  p.value.bool_array_value = {false, true, false};
  msg.new_parameters.push_back(p);
  rcl_interfaces::msg::Parameter p2;
  p2.name = "my_str";
  p2.value.type = rcl_interfaces::msg::ParameterType::PARAMETER_STRING;
  p2.value.string_value = "test";
  msg.changed_parameters.push_back(p2);
  check_deserializer_roundtrip(GET_TS(rcl_interfaces, msg, ParameterEvent), msg);
}
