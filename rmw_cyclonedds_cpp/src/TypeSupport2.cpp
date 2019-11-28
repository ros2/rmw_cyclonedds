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

#include <utility>
namespace rmw_cyclonedds_cpp
{
class ROSIDLC_StructValueType : public AnyStructValueType
{
public:
  const rosidl_typesupport_introspection_c__MessageMembers impl;
  static constexpr TypeGenerator gen = TypeGenerator::ROSIDL_C;
  using MemberType = ROSIDLC_Member;
  explicit ROSIDLC_StructValueType(decltype(impl) impl)
  : impl(impl) {}
  size_t sizeof_struct() const override {return impl.size_of_;}
  size_t n_members() const override {return impl.member_count_;}
  std::unique_ptr<AnyMember> get_member(size_t index) const override;
};

class ROSIDLCPP_StructValueType : public AnyStructValueType
{
public:
  const rosidl_typesupport_introspection_cpp::MessageMembers impl;
  static constexpr TypeGenerator gen = TypeGenerator::ROSIDL_Cpp;
  using MemberType = ROSIDLCPP_Member;
  explicit ROSIDLCPP_StructValueType(decltype(impl) impl)
  : impl(impl) {}
  size_t sizeof_struct() const override {return impl.size_of_;}
  size_t n_members() const override {return impl.member_count_;}
  std::unique_ptr<AnyMember> get_member(size_t index) const override;
};

std::unique_ptr<AnyStructValueType> from_rosidl(const rosidl_message_type_support_t * mts)
{
  auto ts = identify_typesupport(mts->typesupport_identifier);
  switch (ts) {
    case TypeGenerator::ROSIDL_C: {
        auto c = static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(mts->data);
        return std::make_unique<ROSIDLC_StructValueType>(*c);
      }
    case TypeGenerator::ROSIDL_Cpp: {
        auto c =
          static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(mts->data);
        return std::make_unique<ROSIDLCPP_StructValueType>(*c);
      }
  }
}

std::pair<rosidl_message_type_support_t, rosidl_message_type_support_t>
get_svc_request_response_typesupports(const rosidl_service_type_support_t & svc)
{
  auto ts = identify_typesupport(svc.typesupport_identifier);

  rosidl_message_type_support_t request;
  rosidl_message_type_support_t response;

  request.typesupport_identifier = response.typesupport_identifier = svc.typesupport_identifier;
  request.func = response.func = get_message_typesupport_handle_function;

  switch (ts) {
    case TypeGenerator::ROSIDL_C: {
        auto s = static_cast<const MetaService<TypeGenerator::ROSIDL_C> *>(svc.data);
        request.data = s->request_members_;
        response.data = s->response_members_;
      } break;
    case TypeGenerator::ROSIDL_Cpp: {
        auto s = static_cast<const MetaService<TypeGenerator::ROSIDL_Cpp> *>(svc.data);
        request.data = s->request_members_;
        response.data = s->response_members_;
      } break;
  }
  return {request, response};
}

std::unique_ptr<AnyMember> ROSIDLC_StructValueType::get_member(size_t index) const
{
  assert(index < impl.member_count_);
  size_t next_member_offset;
  if (index + 1 == impl.member_count_) {
    next_member_offset = impl.size_of_;
  } else {
    next_member_offset = impl.members_[index + 1].offset_;
  }
  auto member_impl = impl.members_[index];
  if (!member_impl.is_array_) {
    return std::make_unique<ROSIDLC_SingleValueMember>(member_impl, next_member_offset);
  }
  if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
    return std::make_unique<ROSIDLC_ArrayMember>(member_impl, next_member_offset);
  }
  return std::make_unique<ROSIDLC_SequenceMember>(member_impl, next_member_offset);
}

std::unique_ptr<AnyMember> ROSIDLCPP_StructValueType::get_member(size_t index) const
{
  assert(index < impl.member_count_);
  size_t next_member_offset;
  if (index + 1 == impl.member_count_) {
    next_member_offset = impl.size_of_;
  } else {
    next_member_offset = impl.members_[index + 1].offset_;
  }
  auto member_impl = impl.members_[index];
  if (!member_impl.is_array_) {
    return std::make_unique<ROSIDLCPP_SingleValueMember>(member_impl, next_member_offset);
  }
  if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
    return std::make_unique<ROSIDLCPP_ArrayMember>(member_impl, next_member_offset);
  }
  if (ROSIDL_TypeKind(member_impl.type_id_) == ROSIDL_TypeKind::BOOLEAN) {
    return std::make_unique<ROSIDLCPP_BoolSequenceMember>(member_impl, next_member_offset);
  } else {
    return std::make_unique<ROSIDLCPP_SpanSequenceMember>(member_impl, next_member_offset);
  }
}

TypeGenerator identify_typesupport(const char * identifier)
{
  if (identifier == TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::identifier) {
    return TypeGenerator::ROSIDL_C;
  }
  if (identifier == TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::identifier) {
    return TypeGenerator::ROSIDL_Cpp;
  }
  if (std::strcmp(identifier, TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::identifier) == 0) {
    return TypeGenerator::ROSIDL_C;
  }
  if (std::strcmp(identifier, TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::identifier) == 0) {
    return TypeGenerator::ROSIDL_Cpp;
  }
  throw std::runtime_error(std::string("unrecognized typesupport") + identifier);
}

std::unique_ptr<AnyValueType> ROSIDLC_Member::get_value_type() const
{
  auto value_type = ROSIDL_TypeKind(impl.type_id_);
  switch (value_type) {
    case ROSIDL_TypeKind::STRING:
      return std::make_unique<ROSIDLC_StringValueType>();
    case ROSIDL_TypeKind::WSTRING:
      return std::make_unique<ROSIDLC_WStringValueType>();
    case ROSIDL_TypeKind::MESSAGE: {
        auto m = impl.members_;
        assert(m);
        assert(identify_typesupport(m->typesupport_identifier) == TypeGenerator::ROSIDL_C);
        auto mm =
          static_cast<const TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::MetaMessage *>(m->data);
        return std::make_unique<ROSIDLC_StructValueType>(*mm);
      }
    default:
      return std::make_unique<PrimitiveValueType>(value_type);
  }
}

std::unique_ptr<AnyValueType> ROSIDLCPP_Member::get_value_type() const
{
  auto value_type = ROSIDL_TypeKind(impl.type_id_);
  switch (value_type) {
    case ROSIDL_TypeKind::STRING:
      return std::make_unique<ROSIDLCPP_StringValueType>();
    case ROSIDL_TypeKind::WSTRING:
      return std::make_unique<ROSIDLCPP_U16StringValueType>();
    case ROSIDL_TypeKind::MESSAGE: {
        auto m = impl.members_;
        assert(m);
        assert(identify_typesupport(m->typesupport_identifier) == TypeGenerator::ROSIDL_Cpp);
        auto mm =
          static_cast<const TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::MetaMessage *>(m->data);
        return std::make_unique<ROSIDLCPP_StructValueType>(*mm);
      }
    default:
      return std::make_unique<PrimitiveValueType>(value_type);
  }
}
}  // namespace rmw_cyclonedds_cpp
