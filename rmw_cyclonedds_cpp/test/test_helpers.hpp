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
#ifndef TEST_HELPERS_HPP_
#define TEST_HELPERS_HPP_

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "rosidl_typesupport_interface/macros.h"

#include "SizeTypeSupport.hpp"
#include "SerTypeSupport.hpp"
#include "DeserTypeSupport.hpp"
#include "Serialization.hpp"

#define GET_TS(pkg, iface, name) \
  ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME( \
    rosidl_typesupport_introspection_cpp, pkg, iface, name)()

#define GET_TS_C(pkg, iface, name) \
  ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME( \
    rosidl_typesupport_introspection_c, pkg, iface, name)()

using namespace rmw_cyclonedds_cpp;

// ---------------------------------------------------------------------------
// check_parity: CDRSizer min/max (Sample+Key) matches CDRWriter
// ---------------------------------------------------------------------------
inline void check_parity(
  const rosidl_message_type_support_t * ts,
  SampleOrRequest variant = SampleOrRequest::Sample)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, variant);
  auto writer = make_cdr_writer(members, variant);

  EXPECT_EQ(
    sizer.get_min_serialized_size(SampleOrKey::Sample),
    writer->get_min_serialized_size(SampleOrKey::Sample))
    << "min Sample mismatch";
  EXPECT_EQ(
    sizer.get_max_serialized_size(SampleOrKey::Sample),
    writer->get_max_serialized_size(SampleOrKey::Sample))
    << "max Sample mismatch";
  EXPECT_EQ(
    sizer.get_min_serialized_size(SampleOrKey::Key),
    writer->get_min_serialized_size(SampleOrKey::Key))
    << "min Key mismatch";
  EXPECT_EQ(
    sizer.get_max_serialized_size(SampleOrKey::Key),
    writer->get_max_serialized_size(SampleOrKey::Key))
    << "max Key mismatch";
}

// ---------------------------------------------------------------------------
// check_parity_with_data: CDRSizer serialized size / estimate matches CDRWriter
// ---------------------------------------------------------------------------
inline void check_parity_with_data(
  const rosidl_message_type_support_t * ts,
  const void * data,
  SampleOrRequest variant = SampleOrRequest::Sample)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, variant);
  auto writer = make_cdr_writer(members, variant);

  EXPECT_EQ(
    sizer.get_serialized_size(data, SampleOrKey::Sample),
    writer->get_serialized_size(data, SampleOrKey::Sample))
    << "get_serialized_size Sample mismatch";
  EXPECT_EQ(
    sizer.get_serialized_size(data, SampleOrKey::Key),
    writer->get_serialized_size(data, SampleOrKey::Key))
    << "get_serialized_size Key mismatch";

  EXPECT_GE(
    sizer.get_serialized_size_estimate(data, SampleOrKey::Sample),
    sizer.get_serialized_size(data, SampleOrKey::Sample))
    << "estimate < actual (Sample)";
  EXPECT_GE(
    sizer.get_serialized_size_estimate(data, SampleOrKey::Key),
    sizer.get_serialized_size(data, SampleOrKey::Key))
    << "estimate < actual (Key)";

  EXPECT_EQ(
    sizer.get_serialized_size_estimate(data, SampleOrKey::Sample),
    writer->get_serialized_size_estimate(data, SampleOrKey::Sample))
    << "get_serialized_size_estimate Sample mismatch";
}

// ---------------------------------------------------------------------------
// check_serializer_roundtrip: CDRSerializer bytes match CDRWriter bytes
// ---------------------------------------------------------------------------
inline void check_serializer_roundtrip(
  const rosidl_message_type_support_t * ts,
  const void * data,
  SampleOrRequest variant = SampleOrRequest::Sample)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSerializer serializer(members, variant);
  auto writer = make_cdr_writer(members, variant);

  size_t cdr_size = writer->get_serialized_size(data, SampleOrKey::Sample);
  ASSERT_GT(cdr_size, 0u);

  std::vector<unsigned char> buf_new(cdr_size, 0);
  std::vector<unsigned char> buf_old(cdr_size, 0);
  serializer.serialize(buf_new.data(), data, SampleOrKey::Sample);
  writer->serialize(buf_old.data(), data, SampleOrKey::Sample);

  EXPECT_EQ(buf_new, buf_old) << "CDRSerializer output differs from CDRWriter";
}

// ---------------------------------------------------------------------------
// check_deserializer_roundtrip: serialize → deserialize → re-serialize = same
// ---------------------------------------------------------------------------
template<typename MsgT>
inline void check_deserializer_roundtrip(
  const rosidl_message_type_support_t * ts,
  const MsgT & original,
  SampleOrRequest variant = SampleOrRequest::Sample)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSerializer serializer(members, variant);
  CDRDeserializer deserializer(members, variant);

  size_t cdr_size = serializer.get_serialized_size(&original, SampleOrKey::Sample);
  ASSERT_GT(cdr_size, 0u);

  std::vector<unsigned char> buf_orig(cdr_size, 0);
  serializer.serialize(buf_orig.data(), &original, SampleOrKey::Sample);

  MsgT restored{};
  deserializer.deserialize(&restored, buf_orig.data(), cdr_size, SampleOrKey::Sample);

  size_t cdr_size2 = serializer.get_serialized_size(&restored, SampleOrKey::Sample);
  std::vector<unsigned char> buf_restored(cdr_size2, 0);
  serializer.serialize(buf_restored.data(), &restored, SampleOrKey::Sample);

  EXPECT_EQ(buf_orig, buf_restored) << "CDRDeserializer roundtrip: re-serialized bytes differ";
}

// ---------------------------------------------------------------------------
// Key-specific helpers
// ---------------------------------------------------------------------------

inline void check_key_size_parity(const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, SampleOrRequest::Sample);
  auto writer = make_cdr_writer(members, SampleOrRequest::Sample);

  EXPECT_EQ(
    sizer.get_min_serialized_size(SampleOrKey::Key),
    writer->get_min_serialized_size(SampleOrKey::Key))
    << "Key min mismatch";
  EXPECT_EQ(
    sizer.get_max_serialized_size(SampleOrKey::Key),
    writer->get_max_serialized_size(SampleOrKey::Key))
    << "Key max mismatch";
}

template<typename MsgT>
inline void check_key_serialize(
  const rosidl_message_type_support_t * ts, const MsgT & msg)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSerializer serializer(members, SampleOrRequest::Sample);
  auto writer = make_cdr_writer(members, SampleOrRequest::Sample);

  size_t sz = writer->get_serialized_size(&msg, SampleOrKey::Key);
  ASSERT_GT(sz, 0u);

  std::vector<unsigned char> buf_new(sz, 0);
  std::vector<unsigned char> buf_old(sz, 0);
  serializer.serialize(buf_new.data(), &msg, SampleOrKey::Key);
  writer->serialize(buf_old.data(), &msg, SampleOrKey::Key);

  EXPECT_EQ(buf_new, buf_old) << "CDRSerializer Key output differs from CDRWriter";
}

template<typename MsgT>
inline void check_extractkey(
  const rosidl_message_type_support_t * ts, const MsgT & msg)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSerializer serializer(members, SampleOrRequest::Sample);
  CDRDeserializer deserializer(members, SampleOrRequest::Sample);

  size_t sample_sz = serializer.get_serialized_size(&msg, SampleOrKey::Sample);
  ASSERT_GT(sample_sz, 0u);
  std::vector<unsigned char> sample_buf(sample_sz, 0);
  serializer.serialize(sample_buf.data(), &msg, SampleOrKey::Sample);

  std::vector<std::byte> extracted;
  deserializer.extractkey(extracted, sample_buf.data(), sample_sz, SampleOrKey::Sample);
  ASSERT_FALSE(extracted.empty()) << "extractkey returned empty output";

  size_t key_sz = serializer.get_serialized_size(&msg, SampleOrKey::Key);
  std::vector<unsigned char> key_buf(key_sz, 0);
  serializer.serialize(key_buf.data(), &msg, SampleOrKey::Key);

  ASSERT_EQ(extracted.size(), key_buf.size()) << "extractkey size mismatch";
  EXPECT_EQ(0, std::memcmp(extracted.data(), key_buf.data(), key_sz))
    << "extractkey bytes differ from direct Key serialization";
}

template<typename MsgT>
inline void check_extractkey_be(
  const rosidl_message_type_support_t * ts, const MsgT & msg)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSerializer serializer(members, SampleOrRequest::Sample);
  CDRDeserializer deserializer(members, SampleOrRequest::Sample);

  size_t sample_sz = serializer.get_serialized_size(&msg, SampleOrKey::Sample);
  std::vector<unsigned char> sample_buf(sample_sz, 0);
  serializer.serialize(sample_buf.data(), &msg, SampleOrKey::Sample);

  std::vector<std::byte> extracted_be;
  deserializer.extractkey_be(
    extracted_be, sample_buf.data(), sample_sz, SampleOrKey::Sample);
  ASSERT_GE(extracted_be.size(), 4u);
  EXPECT_EQ(static_cast<uint8_t>(extracted_be[1]), 0u)
    << "extractkey_be header byte should indicate big-endian (0)";
}

#endif  // TEST_HELPERS_HPP_
