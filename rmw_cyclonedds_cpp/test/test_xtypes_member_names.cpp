// Copyright 2026 Javier Blanco-Romero
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

#include "gtest/gtest.h"

#include "dds/dds.h"
#include "dds/ddsi/ddsi_typelib.h"
#include "dds/ddsi/ddsi_typewrap.h"
#include "dds/ddsrt/heap.h"
#include "rosidl_typesupport_introspection_c/identifier.h"
#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "std_msgs/msg/detail/u_int32__rosidl_typesupport_introspection_c.h"
#include "std_msgs/msg/detail/u_int32__rosidl_typesupport_introspection_cpp.hpp"

#include "UInt32_.h"
#include "Serialization.hpp"
#include "serdata.hpp"

namespace
{

void expect_idl_matches_dynamic_type(
  const rosidl_message_type_support_t * type_support,
  const dds_topic_descriptor_t & idl_descriptor)
{
  ASSERT_NE(type_support, nullptr);

  const dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr);
  ASSERT_GT(participant, 0);

  sertype_rmw type{};
  create_msg_dds_dynamic_type(
    type_support->typesupport_identifier, type_support->data, participant, &type);

  ASSERT_NE(type.type_information.data, nullptr);
  auto * dynamic_typeinfo =
    ddsi_typeinfo_deser(type.type_information.data, type.type_information.sz);
  auto * idl_typeinfo =
    ddsi_typeinfo_deser(
    idl_descriptor.type_information.data, idl_descriptor.type_information.sz);
  ASSERT_NE(dynamic_typeinfo, nullptr);
  ASSERT_NE(idl_typeinfo, nullptr);

  EXPECT_EQ(
    ddsi_typeid_compare(
      ddsi_typeinfo_minimal_typeid(dynamic_typeinfo),
      ddsi_typeinfo_minimal_typeid(idl_typeinfo)), 0);
  EXPECT_EQ(
    ddsi_typeid_compare(
      ddsi_typeinfo_complete_typeid(dynamic_typeinfo),
      ddsi_typeinfo_complete_typeid(idl_typeinfo)), 0);

  ddsi_typeinfo_free(dynamic_typeinfo);
  ddsi_typeinfo_free(idl_typeinfo);

  ddsrt_free(const_cast<unsigned char *>(type.type_information.data));
  ddsrt_free(const_cast<unsigned char *>(type.type_mapping.data));
  EXPECT_EQ(dds_delete(participant), DDS_RETCODE_OK);
}

}  // namespace

TEST(XTypesMemberNames, c_introspection_matches_idl)
{
  const auto * type_support =
    rosidl_typesupport_introspection_c__get_message_type_support_handle__std_msgs__msg__UInt32();
  ASSERT_NE(type_support, nullptr);
  ASSERT_STREQ(
    type_support->typesupport_identifier, rosidl_typesupport_introspection_c__identifier);
  expect_idl_matches_dynamic_type(type_support, std_msgs_msg_dds__UInt32__desc);
}

TEST(XTypesMemberNames, cpp_introspection_matches_idl)
{
  const auto * type_support =
    rosidl_typesupport_introspection_cpp__get_message_type_support_handle__std_msgs__msg__UInt32();
  ASSERT_NE(type_support, nullptr);
  ASSERT_STREQ(
    type_support->typesupport_identifier,
    rosidl_typesupport_introspection_cpp::typesupport_identifier);
  expect_idl_matches_dynamic_type(type_support, std_msgs_msg_dds__UInt32__desc);
}
