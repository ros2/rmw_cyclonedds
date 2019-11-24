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

#include <cassert>

#include <regex>
#include <string>

#include "bytewise.hpp"
#include "rosidl_generator_c/string_functions.h"
#include "rosidl_generator_c/u16string_functions.h"
#include "rosidl_typesupport_introspection_c/identifier.h"
#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_introspection_c/service_introspection.h"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"

#ifndef TYPESUPPORT2_HPP_
#define TYPESUPPORT2_HPP_
namespace rmw_cyclonedds_cpp
{

namespace  tsi_enum = rosidl_typesupport_introspection_cpp;

// these are shared between c and cpp
enum class ValueType : uint8_t
{
  FLOAT = tsi_enum::ROS_TYPE_FLOAT,
  DOUBLE = tsi_enum::ROS_TYPE_DOUBLE,
  LONG_DOUBLE = tsi_enum::ROS_TYPE_LONG_DOUBLE,
  CHAR = tsi_enum::ROS_TYPE_CHAR,
  WCHAR = tsi_enum::ROS_TYPE_WCHAR,
  BOOLEAN = tsi_enum::ROS_TYPE_BOOLEAN,
  OCTET = tsi_enum::ROS_TYPE_OCTET,
  UINT8 = tsi_enum::ROS_TYPE_UINT8,
  INT8 = tsi_enum::ROS_TYPE_INT8,
  UINT16 = tsi_enum::ROS_TYPE_UINT16,
  INT16 = tsi_enum::ROS_TYPE_INT16,
  UINT32 = tsi_enum::ROS_TYPE_UINT32,
  INT32 = tsi_enum::ROS_TYPE_INT32,
  UINT64 = tsi_enum::ROS_TYPE_UINT64,
  INT64 = tsi_enum::ROS_TYPE_INT64,
  STRING = tsi_enum::ROS_TYPE_STRING,
  WSTRING = tsi_enum::ROS_TYPE_WSTRING,

  MESSAGE = tsi_enum::ROS_TYPE_MESSAGE,
};

enum class MemberContainerType { Array, Sequence, SingleValue };

namespace RTI_C
{
static constexpr auto & identifier = rosidl_typesupport_introspection_c__identifier;

using MetaMessage = rosidl_typesupport_introspection_c__MessageMembers;
using MetaMember = rosidl_typesupport_introspection_c__MessageMember;
using MetaService = rosidl_typesupport_introspection_c__ServiceMembers;

// wrappers to make these more stringlike
struct String : protected rosidl_generator_c__String
{
  using traits_type = std::char_traits<char>;
  auto data() const {return rosidl_generator_c__String::data;}
  auto size() const {return rosidl_generator_c__String::size;}
};
static_assert(
  sizeof(String) == sizeof(rosidl_generator_c__String), "String should not add any new members");

struct WString : protected rosidl_generator_c__U16String
{
  using traits_type = std::char_traits<char16_t>;
  auto data() const {return rosidl_generator_c__U16String::data;}
  auto size() const {return rosidl_generator_c__U16String::size;}
};

static_assert(
  sizeof(WString) == sizeof(rosidl_generator_c__U16String),
  "WString should not add any new members");
}  // namespace RTI_C

namespace RTI_Cpp
{
static constexpr auto & identifier =
  rosidl_typesupport_introspection_cpp::typesupport_identifier;

using MetaMessage = rosidl_typesupport_introspection_cpp::MessageMembers;
using MetaMember = rosidl_typesupport_introspection_cpp::MessageMember;
using MetaService = rosidl_typesupport_introspection_cpp::ServiceMembers;
}  // namespace RTI_Cpp

template<typename UnaryFunction, typename Result = void>
Result with_type(ValueType value_type, UnaryFunction f);

template<typename UnaryFunction>
auto with_typesupport(
  const rosidl_message_type_support_t & untyped_typesupport, UnaryFunction f)
{
  const rosidl_message_type_support_t * ts;

  if ((ts = get_message_typesupport_handle(&untyped_typesupport, RTI_C::identifier))) {
    return f(*static_cast<const RTI_C::MetaMessage *>(ts->data));
  }
  if ((ts = get_message_typesupport_handle(&untyped_typesupport, RTI_Cpp::identifier))) {
    return f(*static_cast<const RTI_Cpp::MetaMessage *>(ts->data));
  }

  throw std::runtime_error("typesupport not recognized");
}

template<typename UnaryFunction>
auto with_typesupport(
  const rosidl_service_type_support_t & untyped_typesupport,
  UnaryFunction f)
{
  const rosidl_service_type_support_t * ts;

  if ((ts = get_service_typesupport_handle(&untyped_typesupport,
    RTI_C::identifier)))
  {
    return f(*static_cast<const RTI_C::MetaService *>(ts->data));
  }
  if ((ts = get_service_typesupport_handle(&untyped_typesupport,
    RTI_Cpp::identifier)))
  {
    return f(*static_cast<const RTI_Cpp::MetaService *>(ts->data));
  }

  throw std::runtime_error("typesupport not recognized");
}

//////////////////
template<typename MetaMessage>
struct MessageRef;

template<typename MetaMember>
struct MemberRef;

template<typename MetaMessage>
struct MessageRef
{
  const MetaMessage & meta_message;
  const void * data;

  using MetaMember = decltype(*meta_message.members_);

  MessageRef(const MetaMessage & meta_message, void * data)
  : meta_message(meta_message), data(data)
  {
    assert(data);
  }

  MessageRef() = delete;

  size_t size() const {return meta_message.member_count_;}

  auto at(size_t index) const;
};

template<typename MetaMember>
struct MemberRef
{
  const MetaMember & meta_member;
  void * data;

  MemberRef(const MetaMember & meta_member, void * data)
  : meta_member(meta_member), data(data)
  {
    assert(data);
  }

  MemberRef() = delete;

  MemberContainerType get_container_type() const
  {
    if (!meta_member.is_array_) {
      return MemberContainerType::SingleValue;
    }
    if (  // unbounded sequence
      meta_member.array_size_ == 0 ||
      // bounded sequence
      meta_member.is_upper_bound_)
    {
      return MemberContainerType::Sequence;
    }
    return MemberContainerType::Array;
  }

  template<typename UnaryFunction, typename Result = void>
  Result with_single_value(UnaryFunction f);

  template<typename UnaryFunction, typename Result = void>
  Result with_array(UnaryFunction f);

  template<typename UnaryFunction, typename Result = void>
  Result with_sequence(UnaryFunction f);

  bool is_submessage_type()
  {
    return ValueType(meta_member.type_id_) == ValueType::MESSAGE;
  }

  bool is_primitive_type()
  {
    switch (ValueType(meta_member.type_id_)) {
      case ValueType::MESSAGE:
      case ValueType::WSTRING:
      case ValueType::WCHAR:
        return false;
      default:
        return true;
    }
  }

  template<typename UnaryFunction>
  auto with_submessage_typesupport(UnaryFunction f)
  {
    assert(is_submessage_type());
    assert(meta_member.members_);
    with_typesupport(*meta_member.members_, f);
  }

protected:
  template<typename UnaryFunction, typename Result = void>
  Result with_value_helper(UnaryFunction f);
};

template<typename MetaMessage>
auto make_message_ref(const MetaMessage & meta, void * data)
{
  return MessageRef<MetaMessage>{meta, data};
}

template<typename MetaMessage>
auto make_message_ref(const MetaMessage & meta, const void * data)
{
  const MessageRef<MetaMessage> x(meta, const_cast<void *>(data));
  return x;
}

template<typename MetaMember>
auto make_member_ref(const MetaMember & meta, void * data)
{
  return MemberRef<MetaMember>(meta, data);
}

template<typename MetaMember>
auto make_member_ref(const MetaMember & meta, const void * data)
{
  const MemberRef<MetaMember> x(meta, const_cast<void *>(data));
  return x;
}

template<typename UnaryFunction>
auto with_message(
  const rosidl_message_type_support_t * type_support,
  const void * data, UnaryFunction f)
{
  return with_typesupport(
    type_support, [&](auto meta) {return f(make_message_ref(meta, data));});
}

template<typename MetaService>
auto make_service_request_ref(const MetaService & meta, const void * data)
{
  return make_message_ref(meta.request_members_, data);
}

template<typename MetaService>
auto make_service_response_ref(const MetaService & meta, const void * data)
{
  return make_message_ref(meta.response_members_, data);
}

template<typename MetaMessage>
auto MessageRef<MetaMessage>::at(size_t index) const
{
  if (index >= meta_message.member_count_) {
    throw std::out_of_range("index out of range");
  }
  auto & member = meta_message.members_[index];
  return make_member_ref(member, byte_offset(data, member.offset_));
}

}  // namespace rmw_cyclonedds_cpp
#include "TypeSupport2_impl.hpp"
#endif  // TYPESUPPORT2_HPP_
