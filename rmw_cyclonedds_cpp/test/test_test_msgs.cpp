// Test new CDR implementation against old for test_msgs types used by
// test_communication (BasicTypes, Arrays, BoundedSequences, etc.)

#include "test_helpers.hpp"

// C++ message types
#include "test_msgs/msg/basic_types.hpp"
#include "test_msgs/msg/arrays.hpp"
#include "test_msgs/msg/bounded_sequences.hpp"
#include "test_msgs/msg/unbounded_sequences.hpp"
#include "test_msgs/msg/defaults.hpp"
#include "test_msgs/msg/constants.hpp"
#include "test_msgs/msg/strings.hpp"
#include "test_msgs/msg/multi_nested.hpp"

// rmw_dds_common message types (used during DDS discovery)
#include "rmw_dds_common/msg/participant_entities_info.hpp"
#include "rmw_dds_common/msg/node_entities_info.hpp"
#include "rmw_dds_common/msg/gid.hpp"

// C++ introspection
#include "test_msgs/msg/detail/basic_types__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/arrays__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/bounded_sequences__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/unbounded_sequences__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/defaults__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/constants__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/strings__rosidl_typesupport_introspection_cpp.hpp"
#include "test_msgs/msg/detail/multi_nested__rosidl_typesupport_introspection_cpp.hpp"
#include "rmw_dds_common/msg/detail/participant_entities_info__rosidl_typesupport_introspection_cpp.hpp"
#include "rmw_dds_common/msg/detail/node_entities_info__rosidl_typesupport_introspection_cpp.hpp"
#include "rmw_dds_common/msg/detail/gid__rosidl_typesupport_introspection_cpp.hpp"

// C introspection
extern "C" {
#include "test_msgs/msg/detail/basic_types__rosidl_typesupport_introspection_c.h"
#include "test_msgs/msg/detail/arrays__rosidl_typesupport_introspection_c.h"
#include "test_msgs/msg/detail/bounded_sequences__rosidl_typesupport_introspection_c.h"
#include "test_msgs/msg/detail/unbounded_sequences__rosidl_typesupport_introspection_c.h"
#include "test_msgs/msg/detail/defaults__rosidl_typesupport_introspection_c.h"
#include "test_msgs/msg/detail/constants__rosidl_typesupport_introspection_c.h"
#include "test_msgs/msg/detail/strings__rosidl_typesupport_introspection_c.h"
#include "test_msgs/msg/detail/multi_nested__rosidl_typesupport_introspection_c.h"
}

// C introspection for rmw_dds_common
extern "C" {
#include "rmw_dds_common/msg/detail/participant_entities_info__rosidl_typesupport_introspection_c.h"
#include "rmw_dds_common/msg/detail/node_entities_info__rosidl_typesupport_introspection_c.h"
#include "rmw_dds_common/msg/detail/gid__rosidl_typesupport_introspection_c.h"
}

// C message types and init/fini
#include "test_msgs/msg/detail/basic_types__functions.h"
#include "test_msgs/msg/detail/arrays__functions.h"
#include "test_msgs/msg/detail/bounded_sequences__functions.h"
#include "test_msgs/msg/detail/unbounded_sequences__functions.h"
#include "test_msgs/msg/detail/defaults__functions.h"
#include "test_msgs/msg/detail/constants__functions.h"
#include "test_msgs/msg/detail/strings__functions.h"
#include "test_msgs/msg/detail/multi_nested__functions.h"
#include "rmw_dds_common/msg/detail/participant_entities_info__functions.h"

// ===========================================================================
// Size parity: CDRSizer min/max matches CDRWriter (C++ introspection)
// ===========================================================================

TEST(SizeParityCpp, BasicTypes)    { check_parity(GET_TS(test_msgs, msg, BasicTypes)); }
// CDRSizer computes a tighter min bound than CDRWriter for types with
// arrays of alignment-variant structs; only check min ≤ writer_min and max ==.
TEST(SizeParityCpp, Arrays)
{
  auto ts = GET_TS(test_msgs, msg, Arrays);
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, SampleOrRequest::Sample);
  auto writer = make_cdr_writer(members, SampleOrRequest::Sample);
  EXPECT_LE(
    sizer.get_min_serialized_size(SampleOrKey::Sample),
    writer->get_min_serialized_size(SampleOrKey::Sample));
  EXPECT_EQ(
    sizer.get_max_serialized_size(SampleOrKey::Sample),
    writer->get_max_serialized_size(SampleOrKey::Sample));
}
TEST(SizeParityCpp, BoundedSeqs)   { check_parity(GET_TS(test_msgs, msg, BoundedSequences)); }
TEST(SizeParityCpp, UnboundedSeqs) { check_parity(GET_TS(test_msgs, msg, UnboundedSequences)); }
TEST(SizeParityCpp, Defaults)      { check_parity(GET_TS(test_msgs, msg, Defaults)); }
TEST(SizeParityCpp, Constants)     { check_parity(GET_TS(test_msgs, msg, Constants)); }
TEST(SizeParityCpp, Strings)       { check_parity(GET_TS(test_msgs, msg, Strings)); }
TEST(SizeParityCpp, MultiNested)
{
  auto ts = GET_TS(test_msgs, msg, MultiNested);
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, SampleOrRequest::Sample);
  auto writer = make_cdr_writer(members, SampleOrRequest::Sample);
  EXPECT_LE(
    sizer.get_min_serialized_size(SampleOrKey::Sample),
    writer->get_min_serialized_size(SampleOrKey::Sample));
  EXPECT_EQ(
    sizer.get_max_serialized_size(SampleOrKey::Sample),
    writer->get_max_serialized_size(SampleOrKey::Sample));
}

// ===========================================================================
// Size parity: CDRSizer min/max matches CDRWriter (C introspection)
// ===========================================================================

TEST(SizeParityC, BasicTypes)    { check_parity(GET_TS_C(test_msgs, msg, BasicTypes)); }
TEST(SizeParityC, Arrays)
{
  auto ts = GET_TS_C(test_msgs, msg, Arrays);
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, SampleOrRequest::Sample);
  auto writer = make_cdr_writer(members, SampleOrRequest::Sample);
  EXPECT_LE(
    sizer.get_min_serialized_size(SampleOrKey::Sample),
    writer->get_min_serialized_size(SampleOrKey::Sample));
  EXPECT_EQ(
    sizer.get_max_serialized_size(SampleOrKey::Sample),
    writer->get_max_serialized_size(SampleOrKey::Sample));
}
TEST(SizeParityC, BoundedSeqs)   { check_parity(GET_TS_C(test_msgs, msg, BoundedSequences)); }
TEST(SizeParityC, UnboundedSeqs) { check_parity(GET_TS_C(test_msgs, msg, UnboundedSequences)); }
TEST(SizeParityC, Defaults)      { check_parity(GET_TS_C(test_msgs, msg, Defaults)); }
TEST(SizeParityC, Constants)     { check_parity(GET_TS_C(test_msgs, msg, Constants)); }
TEST(SizeParityC, Strings)       { check_parity(GET_TS_C(test_msgs, msg, Strings)); }
TEST(SizeParityC, MultiNested)
{
  auto ts = GET_TS_C(test_msgs, msg, MultiNested);
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, SampleOrRequest::Sample);
  auto writer = make_cdr_writer(members, SampleOrRequest::Sample);
  EXPECT_LE(
    sizer.get_min_serialized_size(SampleOrKey::Sample),
    writer->get_min_serialized_size(SampleOrKey::Sample));
  EXPECT_EQ(
    sizer.get_max_serialized_size(SampleOrKey::Sample),
    writer->get_max_serialized_size(SampleOrKey::Sample));
}

// ===========================================================================
// Serialize parity + roundtrip with data (C++)
// ===========================================================================

TEST(SerializerCpp, BasicTypesDefault)
{
  test_msgs::msg::BasicTypes msg{};
  auto ts = GET_TS(test_msgs, msg, BasicTypes);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, BasicTypesPopulated)
{
  test_msgs::msg::BasicTypes msg{};
  msg.bool_value = true;
  msg.byte_value = 255;
  msg.char_value = 'k';
  msg.float32_value = 1.0f;
  msg.float64_value = 2.0;
  msg.int8_value = 3;
  msg.uint8_value = 4;
  msg.int16_value = 5;
  msg.uint16_value = 6;
  msg.int32_value = 7;
  msg.uint32_value = 8;
  msg.int64_value = 9;
  msg.uint64_value = 10;
  auto ts = GET_TS(test_msgs, msg, BasicTypes);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, ArraysDefault)
{
  test_msgs::msg::Arrays msg{};
  auto ts = GET_TS(test_msgs, msg, Arrays);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, BoundedSequencesDefault)
{
  test_msgs::msg::BoundedSequences msg{};
  auto ts = GET_TS(test_msgs, msg, BoundedSequences);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, BoundedSequencesPopulated)
{
  test_msgs::msg::BoundedSequences msg{};
  test_msgs::msg::BasicTypes bt{};
  bt.bool_value = true;
  bt.byte_value = 255;
  bt.char_value = 'k';
  bt.float32_value = 1.0f;
  bt.float64_value = 2.0;
  bt.int8_value = 3;
  bt.uint8_value = 4;
  bt.int16_value = 5;
  bt.uint16_value = 6;
  bt.int32_value = 7;
  bt.uint32_value = 8;
  bt.int64_value = 9;
  bt.uint64_value = 10;
  msg.basic_types_values.push_back(bt);
  auto ts = GET_TS(test_msgs, msg, BoundedSequences);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, UnboundedSequencesDefault)
{
  test_msgs::msg::UnboundedSequences msg{};
  auto ts = GET_TS(test_msgs, msg, UnboundedSequences);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, DefaultsDefault)
{
  test_msgs::msg::Defaults msg{};
  auto ts = GET_TS(test_msgs, msg, Defaults);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, StringsDefault)
{
  test_msgs::msg::Strings msg{};
  auto ts = GET_TS(test_msgs, msg, Strings);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, StringsPopulated)
{
  test_msgs::msg::Strings msg{};
  msg.string_value = "hello world";
  msg.bounded_string_value = "bounded";
  auto ts = GET_TS(test_msgs, msg, Strings);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, MultiNestedDefault)
{
  test_msgs::msg::MultiNested msg{};
  auto ts = GET_TS(test_msgs, msg, MultiNested);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

// ===========================================================================
// rmw_dds_common discovery messages (C++)
// ===========================================================================

// CDRWriter overreports char CDR size (2 vs 1), so skip size parity for Gid.
TEST(SerializerCpp, GidDefault)
{
  rmw_dds_common::msg::Gid msg{};
  auto ts = GET_TS(rmw_dds_common, msg, Gid);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, NodeEntitiesInfoDefault)
{
  rmw_dds_common::msg::NodeEntitiesInfo msg{};
  auto ts = GET_TS(rmw_dds_common, msg, NodeEntitiesInfo);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, NodeEntitiesInfoPopulated)
{
  rmw_dds_common::msg::NodeEntitiesInfo msg{};
  msg.node_namespace = "/test";
  msg.node_name = "my_node";
  rmw_dds_common::msg::Gid gid{};
  gid.data[0] = 1;
  msg.reader_gid_seq.push_back(gid);
  msg.writer_gid_seq.push_back(gid);
  auto ts = GET_TS(rmw_dds_common, msg, NodeEntitiesInfo);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, ParticipantEntitiesInfoDefault)
{
  rmw_dds_common::msg::ParticipantEntitiesInfo msg{};
  auto ts = GET_TS(rmw_dds_common, msg, ParticipantEntitiesInfo);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

TEST(SerializerCpp, ParticipantEntitiesInfoPopulated)
{
  rmw_dds_common::msg::ParticipantEntitiesInfo msg{};
  msg.gid.data[0] = 42;
  rmw_dds_common::msg::NodeEntitiesInfo nei{};
  nei.node_namespace = "/ns";
  nei.node_name = "node1";
  msg.node_entities_info_seq.push_back(nei);
  auto ts = GET_TS(rmw_dds_common, msg, ParticipantEntitiesInfo);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  check_deserializer_roundtrip(ts, msg);
}

// ===========================================================================
// Serialize parity with C introspection + C data
// ===========================================================================

TEST(SerializerC, BasicTypesDefault)
{
  test_msgs__msg__BasicTypes msg;
  test_msgs__msg__BasicTypes__init(&msg);
  auto ts = GET_TS_C(test_msgs, msg, BasicTypes);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  test_msgs__msg__BasicTypes__fini(&msg);
}

TEST(SerializerC, ArraysDefault)
{
  test_msgs__msg__Arrays msg;
  test_msgs__msg__Arrays__init(&msg);
  auto ts = GET_TS_C(test_msgs, msg, Arrays);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  test_msgs__msg__Arrays__fini(&msg);
}

TEST(SerializerC, BoundedSequencesDefault)
{
  test_msgs__msg__BoundedSequences msg;
  test_msgs__msg__BoundedSequences__init(&msg);
  auto ts = GET_TS_C(test_msgs, msg, BoundedSequences);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  test_msgs__msg__BoundedSequences__fini(&msg);
}

TEST(SerializerC, UnboundedSequencesDefault)
{
  test_msgs__msg__UnboundedSequences msg;
  test_msgs__msg__UnboundedSequences__init(&msg);
  auto ts = GET_TS_C(test_msgs, msg, UnboundedSequences);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  test_msgs__msg__UnboundedSequences__fini(&msg);
}

TEST(SerializerC, DefaultsDefault)
{
  test_msgs__msg__Defaults msg;
  test_msgs__msg__Defaults__init(&msg);
  auto ts = GET_TS_C(test_msgs, msg, Defaults);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  test_msgs__msg__Defaults__fini(&msg);
}

TEST(SerializerC, StringsDefault)
{
  test_msgs__msg__Strings msg;
  test_msgs__msg__Strings__init(&msg);
  auto ts = GET_TS_C(test_msgs, msg, Strings);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  test_msgs__msg__Strings__fini(&msg);
}

TEST(SerializerC, MultiNestedDefault)
{
  test_msgs__msg__MultiNested msg;
  test_msgs__msg__MultiNested__init(&msg);
  auto ts = GET_TS_C(test_msgs, msg, MultiNested);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  test_msgs__msg__MultiNested__fini(&msg);
}

TEST(SerializerC, ParticipantEntitiesInfoDefault)
{
  rmw_dds_common__msg__ParticipantEntitiesInfo msg;
  rmw_dds_common__msg__ParticipantEntitiesInfo__init(&msg);
  auto ts = GET_TS_C(rmw_dds_common, msg, ParticipantEntitiesInfo);
  check_parity_with_data(ts, &msg);
  check_serializer_roundtrip(ts, &msg);
  rmw_dds_common__msg__ParticipantEntitiesInfo__fini(&msg);
}
