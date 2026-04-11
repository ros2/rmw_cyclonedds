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

// std_msgs
#include "std_msgs/msg/bool.hpp"
#include "std_msgs/msg/int32.hpp"
#include "std_msgs/msg/string.hpp"
#include "std_msgs/msg/header.hpp"
#include "std_msgs/msg/float64_multi_array.hpp"
#include "std_msgs/msg/detail/bool__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/int8__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/int32__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/int64__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/float64__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/string__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/header__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/int32_multi_array__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/float64_multi_array__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/color_rgba__rosidl_typesupport_introspection_cpp.hpp"

// visualization_msgs
#include "visualization_msgs/msg/marker.hpp"
#include "visualization_msgs/msg/marker_array.hpp"
#include "visualization_msgs/msg/detail/marker__rosidl_typesupport_introspection_cpp.hpp"
#include "visualization_msgs/msg/detail/marker_array__rosidl_typesupport_introspection_cpp.hpp"

// C introspection type supports
extern "C" {
#include "std_msgs/msg/detail/bool__rosidl_typesupport_introspection_c.h"
#include "std_msgs/msg/detail/int32__rosidl_typesupport_introspection_c.h"
#include "std_msgs/msg/detail/string__rosidl_typesupport_introspection_c.h"
#include "std_msgs/msg/detail/float64_multi_array__rosidl_typesupport_introspection_c.h"
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — fixed-size scalars
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, Bool)
{
  check_parity(GET_TS(std_msgs, msg, Bool));
}

TEST(SizeTypeSupportTest, Int8)
{
  check_parity(GET_TS(std_msgs, msg, Int8));
}

TEST(SizeTypeSupportTest, Int32)
{
  check_parity(GET_TS(std_msgs, msg, Int32));
}

TEST(SizeTypeSupportTest, Int64)
{
  check_parity(GET_TS(std_msgs, msg, Int64));
}

TEST(SizeTypeSupportTest, Float64)
{
  check_parity(GET_TS(std_msgs, msg, Float64));
}

TEST(SizeTypeSupportTest, ColorRgba)
{
  check_parity(GET_TS(std_msgs, msg, ColorRGBA));
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — variable-size / nested
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, String)
{
  check_parity(GET_TS(std_msgs, msg, String));
}

TEST(SizeTypeSupportTest, Header)
{
  check_parity(GET_TS(std_msgs, msg, Header));
}

TEST(SizeTypeSupportTest, Int32MultiArray)
{
  check_parity(GET_TS(std_msgs, msg, Int32MultiArray));
}

TEST(SizeTypeSupportTest, Float64MultiArray)
{
  check_parity(GET_TS(std_msgs, msg, Float64MultiArray));
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — Request variant
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, Int32AsRequest)
{
  check_parity(GET_TS(std_msgs, msg, Int32), SampleOrRequest::Request);
}

TEST(SizeTypeSupportTest, StringAsRequest)
{
  check_parity(GET_TS(std_msgs, msg, String), SampleOrRequest::Request);
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — visualization_msgs
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, Marker)
{
  check_parity(GET_TS(visualization_msgs, msg, Marker));
}

TEST(SizeTypeSupportTest, MarkerArray)
{
  check_parity(GET_TS(visualization_msgs, msg, MarkerArray));
}

TEST(SizeTypeSupportTest, MarkerArrayMinSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Sample), 8u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), SIZE_MAX);
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — absolute size checks
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, BoolAbsoluteSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(std_msgs, msg, Bool));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Sample), 5u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), 5u);
}

TEST(SizeTypeSupportTest, Int32AbsoluteSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(std_msgs, msg, Int32));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Sample), 8u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), 8u);
}

TEST(SizeTypeSupportTest, Float64AbsoluteSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(std_msgs, msg, Float64));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Sample), 12u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), 12u);
}

TEST(SizeTypeSupportTest, StringMinSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(std_msgs, msg, String));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Sample), 9u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), SIZE_MAX);
}

TEST(SizeTypeSupportTest, Int32RequestAbsoluteSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(std_msgs, msg, Int32));
  CDRSizer sizer(members, SampleOrRequest::Request);
  auto writer = make_cdr_writer(members, SampleOrRequest::Request);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Sample), 24u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), 24u);
  EXPECT_EQ(
    sizer.get_min_serialized_size(SampleOrKey::Sample),
    writer->get_min_serialized_size(SampleOrKey::Sample));
  EXPECT_EQ(
    sizer.get_max_serialized_size(SampleOrKey::Sample),
    writer->get_max_serialized_size(SampleOrKey::Sample));
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — concrete serialized-size checks
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, SerializedSizeBoolDefault)
{
  std_msgs::msg::Bool msg;
  msg.data = false;
  MessageMembersVariant members = make_message_members_variant(GET_TS(std_msgs, msg, Bool));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_serialized_size(&msg, SampleOrKey::Sample), 5u);
}

TEST(SizeTypeSupportTest, SerializedSizeInt32Values)
{
  std_msgs::msg::Int32 msg;
  msg.data = 12345;
  MessageMembersVariant members = make_message_members_variant(GET_TS(std_msgs, msg, Int32));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_serialized_size(&msg, SampleOrKey::Sample), 8u);
}

TEST(SizeTypeSupportTest, SerializedSizeStringEmpty)
{
  std_msgs::msg::String msg;
  msg.data = "";
  MessageMembersVariant members = make_message_members_variant(GET_TS(std_msgs, msg, String));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_serialized_size(&msg, SampleOrKey::Sample), 9u);
}

TEST(SizeTypeSupportTest, SerializedSizeStringHello)
{
  std_msgs::msg::String msg;
  msg.data = "Hello";
  MessageMembersVariant members = make_message_members_variant(GET_TS(std_msgs, msg, String));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_serialized_size(&msg, SampleOrKey::Sample), 14u);
}

TEST(SizeTypeSupportTest, SerializedSizeHeaderWithFrameId)
{
  std_msgs::msg::Header msg;
  msg.frame_id = "map";
  check_parity_with_data(GET_TS(std_msgs, msg, Header), &msg);
}

TEST(SizeTypeSupportTest, SerializedSizeFloat64MultiArrayPopulated)
{
  std_msgs::msg::Float64MultiArray msg;
  msg.data.resize(5, 1.0);
  msg.layout.dim.resize(1);
  msg.layout.dim[0].label = "x";
  msg.layout.dim[0].size = 5;
  msg.layout.dim[0].stride = 5;
  check_parity_with_data(GET_TS(std_msgs, msg, Float64MultiArray), &msg);
}

TEST(SizeTypeSupportTest, SerializedSizeMarkerDefault)
{
  visualization_msgs::msg::Marker msg;
  check_parity_with_data(GET_TS(visualization_msgs, msg, Marker), &msg);
}

TEST(SizeTypeSupportTest, SerializedSizeMarkerWithPoints)
{
  visualization_msgs::msg::Marker msg;
  msg.points.resize(50);
  msg.colors.resize(50);
  msg.header.frame_id = "world";
  msg.text = "label";
  check_parity_with_data(GET_TS(visualization_msgs, msg, Marker), &msg);
}

TEST(SizeTypeSupportTest, SerializedSizeMarkerArrayPopulated)
{
  visualization_msgs::msg::MarkerArray msg;
  msg.markers.resize(5);
  for (auto & m : msg.markers) {
    m.points.resize(10);
    m.header.frame_id = "map";
  }
  check_parity_with_data(GET_TS(visualization_msgs, msg, MarkerArray), &msg);
}

TEST(SizeTypeSupportTest, MarkerMemberCacheKinds)
{
  // Build the Marker type tree and verify that the SzMemberCache fast-path
  // tags are actually populated — i.e. the optimization is doing something.
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, Marker));
  auto [root, storage] = make_sz_struct(members);

  // Count how many members fall into each kind.
  size_t n_fixed = 0, n_string = 0, n_struct = 0;
  size_t n_cseq = 0, n_cppvec = 0, n_slow = 0, n_other = 0;
  for (const auto & m : storage.all_members) {
    switch (m.cache.kind) {
      case SzMemberKind::Fixed:      ++n_fixed; break;
      case SzMemberKind::String:     ++n_string; break;
      case SzMemberKind::Struct:     ++n_struct; break;
      case SzMemberKind::CSeqFixed:  ++n_cseq; break;
      case SzMemberKind::CppVecFixed: ++n_cppvec; break;
      case SzMemberKind::Slow:       ++n_slow; break;
      default:                       ++n_other; break;
    }
  }

  // Marker has primitives (int32, float64, …) → Fixed
  EXPECT_GT(n_fixed, 0u) << "expected Fixed members (primitives, stamp, etc.)";
  // Marker has std::string fields (ns, frame_id, text, mesh_resource) → String
  EXPECT_GT(n_string, 0u) << "expected String members";
  // Marker has std::vector<Point>, std::vector<ColorRGBA> → CppVecFixed
  EXPECT_GT(n_cppvec, 0u) << "expected CppVecFixed members (points, colors)";
  // Marker has nested Header, Pose, etc. that are NOT fixed → Struct
  EXPECT_GT(n_struct, 0u) << "expected Struct members (Header with frame_id)";
  // The vast majority of members should use a fast path, not Slow.
  size_t total = n_fixed + n_string + n_struct + n_cseq + n_cppvec + n_slow + n_other;
  EXPECT_LT(n_slow, total / 2) << "most members should use a fast path, not Slow";

  // Verify the actual size computation still matches the old impl.
  visualization_msgs::msg::Marker msg;
  msg.header.frame_id = "test";
  msg.ns = "ns";
  msg.text = "hello";
  msg.points.resize(10);
  msg.colors.resize(10);
  check_parity_with_data(GET_TS(visualization_msgs, msg, Marker), &msg);
}

// ---------------------------------------------------------------------------
// SizeTypeSupportTest — C-type path
// ---------------------------------------------------------------------------

TEST(SizeTypeSupportTest, CType_Bool)
{
  check_parity(GET_TS_C(std_msgs, msg, Bool));
}

TEST(SizeTypeSupportTest, CType_Int32)
{
  check_parity(GET_TS_C(std_msgs, msg, Int32));
}

TEST(SizeTypeSupportTest, CType_String)
{
  check_parity(GET_TS_C(std_msgs, msg, String));
}

TEST(SizeTypeSupportTest, CType_Float64MultiArray)
{
  check_parity(GET_TS_C(std_msgs, msg, Float64MultiArray));
}

TEST(SizeTypeSupportTest, CType_BoolAbsoluteSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS_C(std_msgs, msg, Bool));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Sample), 5u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), 5u);
}

TEST(SizeTypeSupportTest, CType_Int32AbsoluteSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS_C(std_msgs, msg, Int32));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_EQ(sizer.get_min_serialized_size(SampleOrKey::Sample), 8u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), 8u);
}

TEST(SizeTypeSupportTest, CType_StringMinSize)
{
  MessageMembersVariant members =
    make_message_members_variant(GET_TS_C(std_msgs, msg, String));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  EXPECT_GT(sizer.get_min_serialized_size(SampleOrKey::Sample), 0u);
  EXPECT_EQ(sizer.get_max_serialized_size(SampleOrKey::Sample), SIZE_MAX);
}

// ---------------------------------------------------------------------------
// SerializerTest — std_msgs + visualization_msgs
// ---------------------------------------------------------------------------

TEST(SerializerTest, Bool)
{
  std_msgs::msg::Bool msg;
  msg.data = true;
  check_serializer_roundtrip(GET_TS(std_msgs, msg, Bool), &msg);
}

TEST(SerializerTest, Int32)
{
  std_msgs::msg::Int32 msg;
  msg.data = -42;
  check_serializer_roundtrip(GET_TS(std_msgs, msg, Int32), &msg);
}

TEST(SerializerTest, String)
{
  std_msgs::msg::String msg;
  msg.data = "hello world";
  check_serializer_roundtrip(GET_TS(std_msgs, msg, String), &msg);
}

TEST(SerializerTest, Header)
{
  std_msgs::msg::Header msg;
  msg.frame_id = "base_link";
  msg.stamp.sec = 123;
  msg.stamp.nanosec = 456789u;
  check_serializer_roundtrip(GET_TS(std_msgs, msg, Header), &msg);
}

TEST(SerializerTest, Float64MultiArrayPopulated)
{
  std_msgs::msg::Float64MultiArray msg;
  msg.data.resize(10);
  for (size_t i = 0; i < msg.data.size(); ++i) {
    msg.data[i] = static_cast<double>(i) * 1.5;
  }
  msg.layout.dim.resize(1);
  msg.layout.dim[0].label = "x";
  msg.layout.dim[0].size = 10;
  msg.layout.dim[0].stride = 10;
  check_serializer_roundtrip(GET_TS(std_msgs, msg, Float64MultiArray), &msg);
}

TEST(SerializerTest, MarkerDefault)
{
  visualization_msgs::msg::Marker msg;
  check_serializer_roundtrip(GET_TS(visualization_msgs, msg, Marker), &msg);
}

TEST(SerializerTest, MarkerPopulated)
{
  visualization_msgs::msg::Marker msg;
  msg.header.frame_id = "map";
  msg.ns = "test_ns";
  msg.id = 7;
  msg.type = visualization_msgs::msg::Marker::POINTS;
  msg.action = visualization_msgs::msg::Marker::ADD;
  msg.text = "some label";
  msg.mesh_resource = "package://pkg/mesh.dae";
  msg.points.resize(100);
  for (auto & p : msg.points) {
    p.x = 1.0; p.y = 2.0; p.z = 3.0;
  }
  msg.colors.resize(100);
  for (auto & c : msg.colors) {
    c.r = 1.0f; c.g = 0.5f; c.b = 0.0f; c.a = 1.0f;
  }
  check_serializer_roundtrip(GET_TS(visualization_msgs, msg, Marker), &msg);
}

TEST(SerializerTest, MarkerArrayPopulated)
{
  visualization_msgs::msg::MarkerArray msg;
  msg.markers.resize(3);
  for (size_t i = 0; i < msg.markers.size(); ++i) {
    msg.markers[i].header.frame_id = "map";
    msg.markers[i].ns = "ns";
    msg.markers[i].id = static_cast<int32_t>(i);
    msg.markers[i].text = "label";
    msg.markers[i].points.resize(50);
    msg.markers[i].colors.resize(50);
  }
  check_serializer_roundtrip(GET_TS(visualization_msgs, msg, MarkerArray), &msg);
}

// ---------------------------------------------------------------------------
// DeserializerTest — std_msgs + visualization_msgs
// ---------------------------------------------------------------------------

TEST(DeserializerTest, Bool)
{
  std_msgs::msg::Bool msg;
  msg.data = true;
  check_deserializer_roundtrip(GET_TS(std_msgs, msg, Bool), msg);
}

TEST(DeserializerTest, Int32)
{
  std_msgs::msg::Int32 msg;
  msg.data = -42;
  check_deserializer_roundtrip(GET_TS(std_msgs, msg, Int32), msg);
}

TEST(DeserializerTest, String)
{
  std_msgs::msg::String msg;
  msg.data = "hello world";
  check_deserializer_roundtrip(GET_TS(std_msgs, msg, String), msg);
}

TEST(DeserializerTest, Header)
{
  std_msgs::msg::Header msg;
  msg.frame_id = "base_link";
  msg.stamp.sec = 123;
  msg.stamp.nanosec = 456789u;
  check_deserializer_roundtrip(GET_TS(std_msgs, msg, Header), msg);
}

TEST(DeserializerTest, Float64MultiArrayPopulated)
{
  std_msgs::msg::Float64MultiArray msg;
  msg.data.resize(10);
  for (size_t i = 0; i < msg.data.size(); ++i) {
    msg.data[i] = static_cast<double>(i) * 1.5;
  }
  msg.layout.dim.resize(1);
  msg.layout.dim[0].label = "x";
  msg.layout.dim[0].size = 10;
  msg.layout.dim[0].stride = 10;
  check_deserializer_roundtrip(GET_TS(std_msgs, msg, Float64MultiArray), msg);
}

TEST(DeserializerTest, MarkerDefault)
{
  visualization_msgs::msg::Marker msg;
  check_deserializer_roundtrip(GET_TS(visualization_msgs, msg, Marker), msg);
}

TEST(DeserializerTest, MarkerPopulated)
{
  visualization_msgs::msg::Marker msg;
  msg.header.frame_id = "map";
  msg.ns = "test_ns";
  msg.id = 7;
  msg.type = visualization_msgs::msg::Marker::POINTS;
  msg.action = visualization_msgs::msg::Marker::ADD;
  msg.text = "some label";
  msg.mesh_resource = "package://pkg/mesh.dae";
  msg.points.resize(100);
  for (auto & p : msg.points) {
    p.x = 1.0; p.y = 2.0; p.z = 3.0;
  }
  msg.colors.resize(100);
  for (auto & c : msg.colors) {
    c.r = 1.0f; c.g = 0.5f; c.b = 0.0f; c.a = 1.0f;
  }
  check_deserializer_roundtrip(GET_TS(visualization_msgs, msg, Marker), msg);
}

TEST(DeserializerTest, MarkerArrayPopulated)
{
  visualization_msgs::msg::MarkerArray msg;
  msg.markers.resize(3);
  for (size_t i = 0; i < msg.markers.size(); ++i) {
    msg.markers[i].header.frame_id = "map";
    msg.markers[i].ns = "ns";
    msg.markers[i].id = static_cast<int32_t>(i);
    msg.markers[i].text = "label";
    msg.markers[i].points.resize(50);
    msg.markers[i].colors.resize(50);
  }
  check_deserializer_roundtrip(GET_TS(visualization_msgs, msg, MarkerArray), msg);
}
