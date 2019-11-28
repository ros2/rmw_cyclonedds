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

#include <unordered_map>
#include <utility>
namespace rmw_cyclonedds_cpp
{
static std::unordered_map<
  const rosidl_message_type_support_t *, std::unique_ptr<AnyStructValueType>>
s_struct_cache;

class ROSIDLC_StructValueType : public AnyStructValueType
{
  const rosidl_typesupport_introspection_c__MessageMembers impl;
  std::vector<std::unique_ptr<ROSIDLC_Member>> m_members;

public:
  static constexpr TypeGenerator gen = TypeGenerator::ROSIDL_C;
  using MemberType = ROSIDLC_Member;
  explicit ROSIDLC_StructValueType(decltype(impl) impl);
  size_t sizeof_struct() const override {return impl.size_of_;}
  size_t n_members() const override {return impl.member_count_;}
  const AnyMember * get_member(size_t index) const override {return m_members.at(index).get();}
};

class ROSIDLCPP_StructValueType : public AnyStructValueType
{
  const rosidl_typesupport_introspection_cpp::MessageMembers impl;
  std::vector<std::unique_ptr<ROSIDLCPP_Member>> m_members;

public:
  static constexpr TypeGenerator gen = TypeGenerator::ROSIDL_Cpp;
  using MemberType = ROSIDLCPP_Member;
  explicit ROSIDLCPP_StructValueType(decltype(impl) impl);
  size_t sizeof_struct() const override {return impl.size_of_;}
  size_t n_members() const override {return impl.member_count_;}
  const AnyMember * get_member(size_t index) const final {return m_members.at(index).get();}
};

const AnyStructValueType * from_rosidl(const rosidl_message_type_support_t * mts)
{
  auto iter = s_struct_cache.find(mts);
  if (iter == s_struct_cache.end()) {
    auto ts = identify_typesupport(mts->typesupport_identifier);
    switch (ts) {
      case TypeGenerator::ROSIDL_C: {
          auto c =
            static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(mts->data);
          iter =
            s_struct_cache.emplace(std::make_pair(mts, std::make_unique<ROSIDLC_StructValueType>(
                *c)))
            .first;
        }; break;
      case TypeGenerator::ROSIDL_Cpp: {
          auto c =
            static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(mts->data);
          iter = s_struct_cache
            .emplace(std::make_pair(mts, std::make_unique<ROSIDLCPP_StructValueType>(*c)))
            .first;
        }; break;
    }
  }
  return iter->second.get();
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

ROSIDLC_StructValueType::ROSIDLC_StructValueType(decltype(impl) impl)
: impl(impl)
{
  for (size_t index = 0; index < impl.member_count_; index++) {
    size_t next_member_offset;

    if (index + 1 == impl.member_count_) {
      next_member_offset = impl.size_of_;
    } else {
      next_member_offset = impl.members_[index + 1].offset_;
    }
    auto member_impl = impl.members_[index];
    std::unique_ptr<ROSIDLC_Member> a_member;
    if (!member_impl.is_array_) {
      a_member = std::make_unique<ROSIDLC_SingleValueMember>(member_impl, next_member_offset);
    } else if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
      a_member = std::make_unique<ROSIDLC_ArrayMember>(member_impl, next_member_offset);
    } else {
      a_member = std::make_unique<ROSIDLC_SequenceMember>(member_impl, next_member_offset);
    }
    m_members.push_back(std::move(a_member));
  }
}

ROSIDLCPP_StructValueType::ROSIDLCPP_StructValueType(decltype(impl) impl)
: impl(impl)
{
  for (size_t index = 0; index < impl.member_count_; index++) {
    size_t next_member_offset;

    if (index + 1 == impl.member_count_) {
      next_member_offset = impl.size_of_;
    } else {
      next_member_offset = impl.members_[index + 1].offset_;
    }
    auto member_impl = impl.members_[index];

    std::unique_ptr<ROSIDLCPP_Member> a_member;
    if (!member_impl.is_array_) {
      a_member = std::make_unique<ROSIDLCPP_SingleValueMember>(member_impl, next_member_offset);
    } else if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
      a_member = std::make_unique<ROSIDLCPP_ArrayMember>(member_impl, next_member_offset);
    } else if (ROSIDL_TypeKind(member_impl.type_id_) == ROSIDL_TypeKind::BOOLEAN) {
      a_member = std::make_unique<ROSIDLCPP_BoolSequenceMember>(member_impl, next_member_offset);
    } else {
      a_member = std::make_unique<ROSIDLCPP_SpanSequenceMember>(member_impl, next_member_offset);
    }
    m_members.push_back(std::move(a_member));
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

ROSIDLC_Member::ROSIDLC_Member(decltype(impl) impl, size_t next_member_offset)
: impl(impl), next_member_offset(next_member_offset)
{
  auto value_type = ROSIDL_TypeKind(impl.type_id_);
  switch (value_type) {
    case ROSIDL_TypeKind::STRING:
      m_value_type = std::make_unique<ROSIDLC_StringValueType>();
      break;
    case ROSIDL_TypeKind::WSTRING:
      m_value_type = std::make_unique<ROSIDLC_WStringValueType>();
      break;
    case ROSIDL_TypeKind::MESSAGE: {
        auto m = impl.members_;
        assert(m);
        assert(identify_typesupport(m->typesupport_identifier) == TypeGenerator::ROSIDL_C);
        auto mm =
          static_cast<const TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::MetaMessage *>(m->data);
        m_value_type = std::make_unique<ROSIDLC_StructValueType>(*mm);
      }
      break;
    default:
      m_value_type = std::make_unique<PrimitiveValueType>(value_type);
      break;
  }
}

ROSIDLCPP_Member::ROSIDLCPP_Member(decltype(impl) impl, size_t next_member_offset)
: impl(impl), next_member_offset(next_member_offset)
{
  auto type_kind = ROSIDL_TypeKind(impl.type_id_);
  switch (type_kind) {
    case ROSIDL_TypeKind::STRING:
      m_value_type = std::make_unique<ROSIDLCPP_StringValueType>();
      break;
    case ROSIDL_TypeKind::WSTRING:
      m_value_type = std::make_unique<ROSIDLCPP_U16StringValueType>();
      break;
    case ROSIDL_TypeKind::MESSAGE: {
        auto m = impl.members_;
        assert(m);
        assert(identify_typesupport(m->typesupport_identifier) == TypeGenerator::ROSIDL_Cpp);
        auto mm =
          static_cast<const TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::MetaMessage *>(m->data);
        m_value_type = std::make_unique<ROSIDLCPP_StructValueType>(*mm);
      }
      break;

    default:
      m_value_type = std::make_unique<PrimitiveValueType>(type_kind);
      break;

  }
}
}  // namespace rmw_cyclonedds_cpp
