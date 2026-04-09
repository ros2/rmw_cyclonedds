// Copyright 2019 ADLINK Technology
// Copyright 2025 ZettaScale Technology
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

#include <cstring>
#include <string>
#include <regex>
#include <sstream>

#include "dds/dds.h"
#include "dyntype.hpp"
#include "serdata.hpp"
#include "type_name.hpp"

#include "rcutils/macros.h"
#include "rmw/error_handling.h"

#if DDS_HAS_TYPELIB
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/md5.h"
#include "dyntype_helper.hpp"
#endif

template<typename MembersType>
ROSIDL_TYPESUPPORT_INTROSPECTION_CPP_LOCAL
std::string create_type_name(const void * untyped_members)
{
  auto members = static_cast<const MembersType *>(untyped_members);
  if (!members) {
    RMW_SET_ERROR_MSG("members handle is null");
    return "";
  }
  std::string message_namespace;
  if (members->message_namespace_ != nullptr) {
    message_namespace = std::string(members->message_namespace_);
  }
  return get_type_name_impl(
    message_namespace, std::string(members->message_name_),
    std::string(""));
}

#if DDS_HAS_TYPELIB

static bool using_introspection_c_typesupport(const char * typesupport_identifier)
{
  return strcmp(
    typesupport_identifier,
    rosidl_typesupport_introspection_c__identifier) == 0;
}

static bool using_introspection_cpp_typesupport(const char * typesupport_identifier)
{
  return strcmp(
    typesupport_identifier,
    rosidl_typesupport_introspection_cpp::typesupport_identifier) == 0;
}

template<typename MemberType>
static void dynamic_type_add_array_prim(
  dds_dynamic_type_t * dstruct, dds_entity_t dds_ppant, const MemberType * member,
  const dds_dynamic_type_kind_t type)
{
  dds_dynamic_type_t ddt;

  if (member->array_size_) {
    uint32_t array_size = static_cast<uint32_t>(member->array_size_);
    if (!member->is_upper_bound_) {
      ddt = dds_dynamic_type_create(
        dds_ppant,
        get_dynamic_type_descriptor_prim(DDS_DYNAMIC_ARRAY, member->name_, 1, &array_size, type));
    } else {
      ddt = dds_dynamic_type_create(
        dds_ppant,
        get_dynamic_type_descriptor_prim(
          DDS_DYNAMIC_SEQUENCE, member->name_, 1, &array_size,
          type));
    }
  } else {
    ddt = dds_dynamic_type_create(
      dds_ppant,
      get_dynamic_type_descriptor_prim(DDS_DYNAMIC_SEQUENCE, member->name_, 0, nullptr, type));
  }

  dds_return_t ret = dds_dynamic_type_add_member(
    dstruct,
    get_dynamic_member_descriptor(ddt, member->name_));
  assert(ret == DDS_RETCODE_OK);
  RCUTILS_UNUSED(ret);  // ret is unused in Release builds
}

template<typename MemberType>
static void dynamic_type_add_array(
  dds_dynamic_type * dstruct, dds_entity_t dds_ppant, const MemberType * member,
  const dds_dynamic_type_t ddt)
{
  dds_dynamic_type_t dseq;

  if (member->array_size_) {
    uint32_t array_size = static_cast<uint32_t>(member->array_size_);
    if (!member->is_upper_bound_) {
      dseq = dds_dynamic_type_create(
        dds_ppant,
        get_dynamic_type_descriptor(DDS_DYNAMIC_ARRAY, member->name_, 1, &array_size, ddt));
    } else {
      dseq = dds_dynamic_type_create(
        dds_ppant,
        get_dynamic_type_descriptor(DDS_DYNAMIC_SEQUENCE, member->name_, 1, &array_size, ddt));
    }
  } else {
    dseq = dds_dynamic_type_create(
      dds_ppant,
      get_dynamic_type_descriptor(DDS_DYNAMIC_SEQUENCE, member->name_, 0, nullptr, ddt));
  }

  dds_return_t ret = dds_dynamic_type_add_member(
    dstruct,
    get_dynamic_member_descriptor(dseq, member->name_));
  assert(ret == DDS_RETCODE_OK);
  RCUTILS_UNUSED(ret);  // ret is unused in Release builds
}

template<typename MemberType>
static void dynamic_type_add_member(
  dds_dynamic_type_t * dstruct, dds_entity_t dds_ppant, const MemberType * member,
  const dds_dynamic_type_kind_t type)
{
  dds_return_t ret;

  assert(member->type_id_ != rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING);
  assert(member->type_id_ != rosidl_typesupport_introspection_cpp::ROS_TYPE_WSTRING);
  assert(member->type_id_ != rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE);

  if (!member->is_array_) {
    ret = dds_dynamic_type_add_member(
      dstruct,
      get_dynamic_member_descriptor_prim(type, member->name_));
    assert(ret == DDS_RETCODE_OK);
    RCUTILS_UNUSED(ret);  // ret is unused in Release builds
  } else {
    dynamic_type_add_array_prim(dstruct, dds_ppant, member, type);
  }
}

static void dynamic_type_register(
  struct sertype_rmw * st, dds_dynamic_type_t dt,
  dds_entity_t dds_ppant)
{
  dds_typeinfo_t * type_info;
  auto rc = dds_dynamic_type_register(&dt, &type_info);
  if (rc != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("dds_dynamic_type_register failed to register type");
    goto fail_typeinfo;
  }

  dds_topic_descriptor_t * desc;
  rc = dds_create_topic_descriptor(
    DDS_FIND_SCOPE_GLOBAL, dds_ppant, type_info, 0, &desc);
  if (rc != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("dds_create_topic_descriptor failed to create descriptor");
    goto fail_descriptor;
  }

  st->type_information.data =
    static_cast<const unsigned char *>(ddsrt_memdup(
      desc->type_information.data,
      desc->type_information.sz));
  st->type_information.sz = desc->type_information.sz;
  st->type_mapping.data = static_cast<const unsigned char *>(ddsrt_memdup(
      desc->type_mapping.data,
      desc->type_mapping.sz));
  st->type_mapping.sz = desc->type_mapping.sz;

  dds_delete_topic_descriptor(desc);
fail_descriptor:
  dds_free_typeinfo(type_info);
fail_typeinfo:
  dds_dynamic_type_unref(&dt);
}

template<typename MembersType>
static bool construct_dds_dynamic_type(
  dds_dynamic_type_t * dstruct, dds_entity_t dds_ppant, const MembersType * members)
{
  assert(members);
  assert(dds_ppant);

  dds_return_t ret;

  for (uint32_t i = 0; i < members->member_count_; ++i) {
    const auto * member = members->members_ + i;
    switch (member->type_id_) {
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BOOL:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_BOOLEAN);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BYTE:
        // ROS2 appears to have mapped CHAR to UINT8
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT8);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT8);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_CHAR:
        // ROS2 appears to have mapped CHAR to UINT8
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT8);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_WCHAR:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_CHAR16);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT8:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_INT8);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT32:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_FLOAT32);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT64:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_FLOAT64);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT16:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_INT16);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT16:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT16);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT32:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_INT32);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT32:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT32);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT64:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_INT64);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT64:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT64);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING:
        {
          dds_dynamic_type_t ddt;
          if (member->string_upper_bound_) {
            uint32_t string_size = static_cast<uint32_t>(member->string_upper_bound_);
            ddt = dds_dynamic_type_create(
              dds_ppant,
              get_dynamic_type_descriptor(DDS_DYNAMIC_STRING8, nullptr, 1, &string_size, {}));
          } else {
            ddt = dds_dynamic_type_create(
              dds_ppant,
              get_dynamic_type_descriptor(DDS_DYNAMIC_STRING8, nullptr, 0, nullptr, {}));
          }
          if (!member->is_array_) {
            ret = dds_dynamic_type_add_member(
              dstruct,
              get_dynamic_member_descriptor(ddt, member->name_));
            assert(ret == DDS_RETCODE_OK);
            RCUTILS_UNUSED(ret);  // ret is unused in Release builds
          } else {
            dynamic_type_add_array(dstruct, dds_ppant, member, ddt);
          }

          break;
        }
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_WSTRING:
        {
          dds_dynamic_type_t ddt;
          if (member->string_upper_bound_) {
            uint32_t string_size = static_cast<uint32_t>(member->string_upper_bound_);
            ddt = dds_dynamic_type_create(
              dds_ppant,
              get_dynamic_type_descriptor(DDS_DYNAMIC_STRING16, nullptr, 1, &string_size, {}));
          } else {
            ddt = dds_dynamic_type_create(
              dds_ppant,
              get_dynamic_type_descriptor(DDS_DYNAMIC_STRING16, nullptr, 0, nullptr, {}));
          }
          if (!member->is_array_) {
            ret = dds_dynamic_type_add_member(
              dstruct,
              get_dynamic_member_descriptor(ddt, member->name_));
            assert(ret == DDS_RETCODE_OK);
          } else {
            dynamic_type_add_array(dstruct, dds_ppant, member, ddt);
          }

          break;
        }
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE:
        {
          if (!member->members_) {
            RMW_SET_ERROR_MSG("no members value");
            return false;
          }

          auto sub_members = static_cast<const MembersType *>(member->members_->data);
          dds_dynamic_type_t ddt;
          ddt = dds_dynamic_type_create(
            dds_ppant,
            get_dynamic_type_descriptor(
              DDS_DYNAMIC_STRUCTURE,
              create_type_name<MembersType>(member->members_->data).c_str(), 0, nullptr, {}));

          dds_dynamic_type_set_extensibility(&ddt, DDS_DYNAMIC_TYPE_EXT_FINAL);

          if (!construct_dds_dynamic_type(&ddt, dds_ppant, sub_members)) {
            RMW_SET_ERROR_MSG("construct_dds_dynamic_type error");
            return false;
          }

          if (!member->is_array_) {
            ret = dds_dynamic_type_add_member(
              dstruct,
              get_dynamic_member_descriptor(ddt, member->name_));
            assert(ret == DDS_RETCODE_OK);
          } else {
            dynamic_type_add_array(dstruct, dds_ppant, member, ddt);
          }

          break;
        }
      default:
        RMW_SET_ERROR_MSG(
          (std::string("unknown type id ") +
          std::to_string(member->type_id_)).c_str());
        return false;
    }

    if (member->is_key_) {
      dds_dynamic_member_set_key(dstruct, i, true);
    }
  }

  return true;
}
#endif

void create_msg_dds_dynamic_type(
  const char * type_support_identifier, const void * untyped_members,
  dds_entity_t dds_ppant, struct sertype_rmw * st)
{
#if DDS_HAS_TYPELIB
  if (using_introspection_c_typesupport(type_support_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__MessageMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(
      dds_ppant,
      get_dynamic_type_descriptor(
        DDS_DYNAMIC_STRUCTURE,
        create_type_name<rosidl_typesupport_introspection_c__MessageMembers_s>(
          untyped_members).c_str(), 0, nullptr, {}));

    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members)) {
      dynamic_type_register(st, dstruct, dds_ppant);
    } else {
#if THROW_ON_DYNAMIC_TYPE_ERROR
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
    }
  } else if (using_introspection_cpp_typesupport(type_support_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(
      dds_ppant,
      get_dynamic_type_descriptor(
        DDS_DYNAMIC_STRUCTURE,
        create_type_name<rosidl_typesupport_introspection_cpp::MessageMembers_s>(
          untyped_members).c_str(), 0, nullptr, {}));

    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members)) {
      dynamic_type_register(st, dstruct, dds_ppant);
    } else {
#if THROW_ON_DYNAMIC_TYPE_ERROR
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
    }
  } else {
    throw std::runtime_error("create_dds_dynamic_type, unsupported typesupport");
  }
#else
  static_cast<void>(type_support_identifier);
  static_cast<void>(untyped_members);
  static_cast<void>(dds_ppant);
  static_cast<void>(st);
#endif
}

void create_req_dds_dynamic_type(
  const char * type_support_identifier, const void * untyped_members,
  dds_entity_t dds_ppant, struct sertype_rmw * st)
{
#if DDS_HAS_TYPELIB
  if (using_introspection_c_typesupport(type_support_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__ServiceMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(
      dds_ppant,
      get_dynamic_type_descriptor(
        DDS_DYNAMIC_STRUCTURE,
        create_type_name<rosidl_typesupport_introspection_c__MessageMembers_s>(
          untyped_members).c_str(), 0, nullptr, {}));

    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members->request_members_)) {
      dynamic_type_register(st, dstruct, dds_ppant);
    } else {
#if THROW_ON_DYNAMIC_TYPE_ERROR
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
    }
  } else if (using_introspection_cpp_typesupport(type_support_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(
      dds_ppant,
      get_dynamic_type_descriptor(
        DDS_DYNAMIC_STRUCTURE,
        create_type_name<rosidl_typesupport_introspection_cpp::MessageMembers_s>(
          untyped_members).c_str(), 0, nullptr, {}));

    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members->request_members_)) {
      dynamic_type_register(st, dstruct, dds_ppant);
    } else {
#if THROW_ON_DYNAMIC_TYPE_ERROR
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
    }
  } else {
    throw std::runtime_error("create_dds_dynamic_type, unsupported typesupport");
  }
#else
  static_cast<void>(type_support_identifier);
  static_cast<void>(untyped_members);
  static_cast<void>(dds_ppant);
  static_cast<void>(st);
#endif
}

void create_res_dds_dynamic_type(
  const char * type_support_identifier, const void * untyped_members,
  dds_entity_t dds_ppant, struct sertype_rmw * st)
{
#if DDS_HAS_TYPELIB
  if (using_introspection_c_typesupport(type_support_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__ServiceMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(
      dds_ppant,
      get_dynamic_type_descriptor(
        DDS_DYNAMIC_STRUCTURE,
        create_type_name<rosidl_typesupport_introspection_c__MessageMembers_s>(
          untyped_members).c_str(), 0, nullptr, {}));

    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members->response_members_)) {
      dynamic_type_register(st, dstruct, dds_ppant);
    } else {
#if THROW_ON_DYNAMIC_TYPE_ERROR
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
    }
  } else if (using_introspection_cpp_typesupport(type_support_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(
      dds_ppant,
      get_dynamic_type_descriptor(
        DDS_DYNAMIC_STRUCTURE,
        create_type_name<rosidl_typesupport_introspection_cpp::MessageMembers_s>(
          untyped_members).c_str(), 0, nullptr, {}));
    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members->response_members_)) {
      dynamic_type_register(st, dstruct, dds_ppant);
    } else {
#if THROW_ON_DYNAMIC_TYPE_ERROR
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
    }
  } else {
    throw std::runtime_error("create_dds_dynamic_type, unsupported typesupport");
  }

#else
  static_cast<void>(type_support_identifier);
  static_cast<void>(untyped_members);
  static_cast<void>(dds_ppant);
  static_cast<void>(st);
#endif
}
