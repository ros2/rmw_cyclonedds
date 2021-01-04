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
#include "TypeSupport2.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rcutils/error_handling.h"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"

namespace rmw_cyclonedds_cpp
{
class ROSIDLC_StructValueType : public StructValueType
{
  const rosidl_typesupport_introspection_c__MessageMembers * impl;
  std::vector<Member> m_members;
  std::vector<std::unique_ptr<const AnyValueType>> m_inner_value_types;

  template<typename ConstructedType, typename ... Args>
  ConstructedType * make_value_type(Args && ... args)
  {
    auto unique_ptr = std::make_unique<ConstructedType>(std::forward<Args>(args)...);
    auto ptr = unique_ptr.get();
    m_inner_value_types.push_back(std::move(unique_ptr));
    return ptr;
  }

public:
  static constexpr TypeGenerator gen = TypeGenerator::ROSIDL_C;
  explicit ROSIDLC_StructValueType(const rosidl_typesupport_introspection_c__MessageMembers * impl);
  size_t sizeof_struct() const override {return impl->size_of_;}
  size_t n_members() const override {return impl->member_count_;}
  const Member * get_member(size_t index) const override {return &m_members.at(index);}
};

class ROSIDLCPP_StructValueType : public StructValueType
{
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl;
  std::vector<Member> m_members;
  std::vector<std::unique_ptr<const AnyValueType>> m_inner_value_types;
  template<typename ConstructedType, typename ... Args>
  ConstructedType * make_value_type(Args && ... args)
  {
    auto unique_ptr = std::make_unique<ConstructedType>(std::forward<Args>(args)...);
    auto ptr = unique_ptr.get();
    m_inner_value_types.push_back(std::move(unique_ptr));
    return ptr;
  }

public:
  static constexpr TypeGenerator gen = TypeGenerator::ROSIDL_Cpp;
  explicit ROSIDLCPP_StructValueType(
    const rosidl_typesupport_introspection_cpp::MessageMembers * impl);
  size_t sizeof_struct() const override {return impl->size_of_;}
  size_t n_members() const override {return impl->member_count_;}
  const Member * get_member(size_t index) const final {return &m_members.at(index);}
};

std::unique_ptr<StructValueType> make_message_value_type(const rosidl_message_type_support_t * mts)
{
  if (auto ts_c =
    get_message_typesupport_handle(
      mts,
      TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::get_identifier()))
  {
    auto members = static_cast<const MetaMessage<TypeGenerator::ROSIDL_C> *>(ts_c->data);
    return std::make_unique<ROSIDLC_StructValueType>(members);
  } else {
    rcutils_error_string_t prev_error_string = rcutils_get_error_string();
    rcutils_reset_error();

    if (auto ts_cpp =
      get_message_typesupport_handle(
        mts,
        TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::get_identifier()))
    {
      auto members = static_cast<const MetaMessage<TypeGenerator::ROSIDL_Cpp> *>(ts_cpp->data);
      return std::make_unique<ROSIDLCPP_StructValueType>(members);
    } else {
      rcutils_error_string_t error_string = rcutils_get_error_string();
      rcutils_reset_error();

      throw std::runtime_error(
              std::string("Type support not from this implementation.  Got:\n") +
              "    " + prev_error_string.str + "\n" +
              "    " + error_string.str + "\n" +
              "while fetching it");
    }
  }
}

std::pair<std::unique_ptr<StructValueType>, std::unique_ptr<StructValueType>>
make_request_response_value_types(const rosidl_service_type_support_t * svc_ts)
{
  if (auto tsc =
    get_service_typesupport_handle(
      svc_ts,
      TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::get_identifier()))
  {
    auto typed =
      static_cast<const TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::MetaService *>(tsc->data);
    return {
      std::make_unique<ROSIDLC_StructValueType>(typed->request_members_),
      std::make_unique<ROSIDLC_StructValueType>(typed->response_members_)
    };
  } else {
    rcutils_error_string_t prev_error_string = rcutils_get_error_string();
    rcutils_reset_error();

    if (auto tscpp =
      get_service_typesupport_handle(
        svc_ts,
        TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::get_identifier()))
    {
      auto typed =
        static_cast<const TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::MetaService *>(tscpp->data);
      return {
        std::make_unique<ROSIDLCPP_StructValueType>(typed->request_members_),
        std::make_unique<ROSIDLCPP_StructValueType>(typed->response_members_)
      };
    } else {
      rcutils_error_string_t error_string = rcutils_get_error_string();
      rcutils_reset_error();

      throw std::runtime_error(
              std::string("Service type support not from this implementation.  Got:\n") +
              "    " + prev_error_string.str + "\n" +
              "    " + error_string.str + "\n" +
              "while fetching it");
    }
  }
}

ROSIDLC_StructValueType::ROSIDLC_StructValueType(
  const rosidl_typesupport_introspection_c__MessageMembers * impl)
: impl{impl}, m_members{}, m_inner_value_types{}
{
  for (size_t index = 0; index < impl->member_count_; index++) {
    auto member_impl = impl->members_[index];

    const AnyValueType * element_value_type;
    switch (ROSIDL_TypeKind(member_impl.type_id_)) {
      case ROSIDL_TypeKind::MESSAGE:
        m_inner_value_types.push_back(make_message_value_type(member_impl.members_));
        element_value_type = m_inner_value_types.back().get();
        break;
      case ROSIDL_TypeKind::STRING:
        element_value_type = make_value_type<ROSIDLC_StringValueType>();
        break;
      case ROSIDL_TypeKind::WSTRING:
        element_value_type = make_value_type<ROSIDLC_WStringValueType>();
        break;
      default:
        element_value_type =
          make_value_type<PrimitiveValueType>(ROSIDL_TypeKind(member_impl.type_id_));
        break;
    }

    const AnyValueType * member_value_type;
    if (!member_impl.is_array_) {
      member_value_type = element_value_type;
    } else if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
      member_value_type = make_value_type<ArrayValueType>(
        element_value_type, member_impl.array_size_);
    } else if (member_impl.size_function) {
      member_value_type = make_value_type<CallbackSpanSequenceValueType>(
        element_value_type, member_impl.size_function, member_impl.get_const_function);
    } else {
      member_value_type = make_value_type<ROSIDLC_SpanSequenceValueType>(element_value_type);
    }
    m_members.push_back(
      Member{
        member_impl.name_,
        member_value_type,
        member_impl.offset_,
      });
  }
}

ROSIDLCPP_StructValueType::ROSIDLCPP_StructValueType(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl)
: impl(impl)
{
  for (size_t index = 0; index < impl->member_count_; index++) {
    auto member_impl = impl->members_[index];

    const AnyValueType * element_value_type;
    switch (ROSIDL_TypeKind(member_impl.type_id_)) {
      case ROSIDL_TypeKind::MESSAGE:
        m_inner_value_types.push_back(make_message_value_type(member_impl.members_));
        element_value_type = m_inner_value_types.back().get();
        break;
      case ROSIDL_TypeKind::STRING:
        element_value_type = make_value_type<ROSIDLCPP_StringValueType>();
        break;
      case ROSIDL_TypeKind::WSTRING:
        element_value_type = make_value_type<ROSIDLCPP_U16StringValueType>();
        break;
      default:
        element_value_type =
          make_value_type<PrimitiveValueType>(ROSIDL_TypeKind(member_impl.type_id_));
        break;
    }

    const AnyValueType * member_value_type;
    if (!member_impl.is_array_) {
      member_value_type = element_value_type;
    } else if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
      member_value_type = make_value_type<ArrayValueType>(
        element_value_type, member_impl.array_size_);
    } else if (ROSIDL_TypeKind(member_impl.type_id_) == ROSIDL_TypeKind::BOOLEAN) {
      member_value_type = make_value_type<BoolVectorValueType>();
    } else {
      member_value_type = make_value_type<CallbackSpanSequenceValueType>(
        element_value_type, member_impl.size_function, member_impl.get_const_function);
    }
    m_members.push_back(
      Member {
        member_impl.name_,
        member_value_type,
        member_impl.offset_,
      });
  }
}
}  // namespace rmw_cyclonedds_cpp
