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
#pragma once

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

namespace rmw_cyclonedds_cpp
{
enum class TypeGenerator
{
  ROSIDL_C,
  ROSIDL_Cpp,
};

TypeGenerator identify_typesupport(const char * identifier);

template<TypeGenerator>
struct TypeGeneratorInfo;

struct AnyValueType;

/// contiguous storage objects
template<typename T>
class TypedSpan;
class UntypedSpan;

template<typename T>
class TypedSpan
{
  const T * m_data;
  const size_t m_size;

public:
  TypedSpan(const T * data, size_t size)
  : m_data(data), m_size(size) {}

  size_t size() const {return m_size;}
  size_t size_bytes() const {return size() * sizeof(T);}
  const T * data() const {return m_data;}

  auto begin() {return m_data;}
  auto end() {return m_data + size();}
};

class UntypedSpan
{
protected:
  const void * m_data;
  size_t m_size_bytes;

public:
  UntypedSpan(const void * data, size_t size_bytes)
  : m_data(data), m_size_bytes(size_bytes) {}

  template<typename T>
  TypedSpan<T> cast() const
  {
    assert(m_size_bytes % sizeof(T) == 0);
    return {static_cast<T *>(m_data), m_size_bytes / sizeof(T)};
  }
  size_t size_bytes() const {return m_size_bytes;}
  const void * data() const {return m_data;}
};

class ChunkedIterator
{
protected:
  void * m_data;
  size_t m_size_bytes;

public:
  ChunkedIterator & operator++()
  {
    m_data = byte_offset(m_data, m_size_bytes);
    return *this;
  }
  UntypedSpan operator*() const {return {m_data, m_size_bytes};}
  bool operator==(const ChunkedIterator & other) const
  {
    assert(m_size_bytes == other.m_size_bytes);
    return m_data == other.m_data;
  }
  bool operator!=(const ChunkedIterator & other) const {return !(*this == other);}
};

template<typename NativeType>
auto make_typed_span(const NativeType * m_data, size_t size)
{
  return TypedSpan<NativeType>{m_data, size};
}

template<>
struct TypeGeneratorInfo<TypeGenerator::ROSIDL_C>
{
  static constexpr auto enum_value = TypeGenerator::ROSIDL_C;
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
};

template<>
struct TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>
{
  static constexpr auto enum_value = TypeGenerator::ROSIDL_Cpp;
  static constexpr auto & identifier = rosidl_typesupport_introspection_cpp::typesupport_identifier;
  using MetaMessage = rosidl_typesupport_introspection_cpp::MessageMembers;
  using MetaMember = rosidl_typesupport_introspection_cpp::MessageMember;
  using MetaService = rosidl_typesupport_introspection_cpp::ServiceMembers;
  using String = std::string;
  using WString = std::u16string;
};

constexpr const char * get_identifier(TypeGenerator g)
{
  switch (g) {
    case TypeGenerator::ROSIDL_C:
      return TypeGeneratorInfo<TypeGenerator::ROSIDL_C>::identifier;
    case TypeGenerator::ROSIDL_Cpp:
      return TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>::identifier;
  }
}

template<typename UnaryFunction>
void with_typesupport_info(const char * identifier, UnaryFunction f)
{
  {
    using tgi = TypeGeneratorInfo<TypeGenerator::ROSIDL_C>;
    if (identifier == tgi::identifier) {
      return f(tgi{});
    }
  }
  {
    using tgi = TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>;
    if (identifier == tgi::identifier) {
      return f(tgi{});
    }
  }
  {
    using tgi = TypeGeneratorInfo<TypeGenerator::ROSIDL_C>;
    if (std::strcmp(identifier, tgi::identifier) == 0) {
      return f(tgi{});
    }
  }
  {
    using tgi = TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>;
    if (std::strcmp(identifier, tgi::identifier) == 0) {
      return f(tgi{});
    }
  }
  throw std::runtime_error("typesupport not recognized");
}

template<TypeGenerator g>
using MetaMessage = typename TypeGeneratorInfo<g>::MetaMessage;
template<TypeGenerator g>
using MetaMember = typename TypeGeneratorInfo<g>::MetaMember;
template<TypeGenerator g>
using MetaService = typename TypeGeneratorInfo<g>::MetaService;

namespace tsi_enum = rosidl_typesupport_introspection_cpp;

// these are shared between c and cpp
enum class ROSIDL_TypeKind : uint8_t
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

class AnyStructValueType;
std::unique_ptr<AnyStructValueType> from_rosidl(const rosidl_message_type_support_t * mts);

class AnyMember;

struct AnyValueType
{
  // represents not just the IDL value but also its physical representation
  virtual ~AnyValueType() = default;
  virtual ROSIDL_TypeKind type_kind() const = 0;
  virtual size_t sizeof_type() const = 0;
};

class AnyStructValueType : public AnyValueType
{
public:
  ROSIDL_TypeKind type_kind() const final {return ROSIDL_TypeKind::MESSAGE;}
  size_t sizeof_type() const final {return sizeof_struct();}
  virtual size_t sizeof_struct() const = 0;
  virtual size_t n_members() const = 0;
  virtual std::unique_ptr<AnyMember> get_member(size_t) const = 0;
};

struct PrimitiveValueType : public AnyValueType
{
  const ROSIDL_TypeKind m_type_kind;

  explicit PrimitiveValueType(ROSIDL_TypeKind type_kind)
  : m_type_kind(type_kind)
  {
    assert(type_kind != ROSIDL_TypeKind::STRING);
    assert(type_kind != ROSIDL_TypeKind::WSTRING);
    assert(type_kind != ROSIDL_TypeKind::MESSAGE);
  }
  ROSIDL_TypeKind type_kind() const final {return m_type_kind;}
  size_t sizeof_type() const final
  {
    switch (m_type_kind) {
      case ROSIDL_TypeKind::FLOAT:
        return sizeof(float);
      case ROSIDL_TypeKind::DOUBLE:
        return sizeof(double);
      case ROSIDL_TypeKind::LONG_DOUBLE:
        return sizeof(long double);
      case ROSIDL_TypeKind::CHAR:
        return sizeof(char);
      case ROSIDL_TypeKind::WCHAR:
        return sizeof(char16_t);
      case ROSIDL_TypeKind::BOOLEAN:
        return sizeof(bool);
      case ROSIDL_TypeKind::OCTET:
        return sizeof(unsigned char);
      case ROSIDL_TypeKind::UINT8:
        return sizeof(uint_least8_t);
      case ROSIDL_TypeKind::INT8:
        return sizeof(int_least8_t);
      case ROSIDL_TypeKind::UINT16:
        return sizeof(uint_least16_t);
      case ROSIDL_TypeKind::INT16:
        return sizeof(int_least16_t);
      case ROSIDL_TypeKind::UINT32:
        return sizeof(uint_least32_t);
      case ROSIDL_TypeKind::INT32:
        return sizeof(int_least32_t);
      case ROSIDL_TypeKind::UINT64:
        return sizeof(uint_least64_t);
      case ROSIDL_TypeKind::INT64:
        return sizeof(int_least64_t);
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
        throw std::runtime_error(
                "not a primitive value type: " +
                std::to_string(std::underlying_type_t<ROSIDL_TypeKind>(m_type_kind)));
    }
  }
};

class AnyMember
{
public:
  virtual ~AnyMember() = default;
  virtual const void * get_member_data(const void * struct_data) const = 0;
  virtual std::unique_ptr<AnyValueType> get_value_type() const = 0;
};

class ROSIDLC_Member : public virtual AnyMember
{
protected:
  const rosidl_typesupport_introspection_c__MessageMember impl;
  size_t next_member_offset;

public:
  ROSIDLC_Member(decltype(impl) impl, size_t next_member_offset)
  : impl(impl), next_member_offset(next_member_offset)
  {
  }

  std::unique_ptr<AnyValueType> get_value_type() const final;

  const void * get_member_data(const void * struct_data) const final
  {
    return byte_offset(struct_data, impl.offset_);
  }

  size_t sizeof_member_plus_padding() const {return next_member_offset - impl.offset_;}
};

class ROSIDLCPP_Member : public virtual AnyMember
{
protected:
  const rosidl_typesupport_introspection_cpp::MessageMember impl;
  size_t next_member_offset;
  std::unique_ptr<AnyValueType> get_value_type() const final;

public:
  ROSIDLCPP_Member(decltype(impl) impl, size_t next_member_offset)
  : impl(impl), next_member_offset(next_member_offset)
  {
  }

  const void * get_member_data(const void * struct_data) const final
  {
    return byte_offset(struct_data, impl.offset_);
  }
  size_t sizeof_member_plus_padding() const {return next_member_offset - impl.offset_;}
};

class SingleValueMember : public virtual AnyMember
{
public:
};

class ArrayValueMember : public virtual AnyMember
{
public:
  virtual size_t array_size() const = 0;
  virtual UntypedSpan array_contents(const void * struct_data) const = 0;
};

class SpanSequenceValueMember : public virtual AnyMember
{
public:
  virtual size_t sequence_size(const void * struct_data) const = 0;
  virtual UntypedSpan sequence_contents(const void * struct_data) const = 0;
};

class ROSIDLC_SingleValueMember : public virtual SingleValueMember, public ROSIDLC_Member
{
public:
  using ROSIDLC_Member::get_member_data;
  using ROSIDLC_Member::ROSIDLC_Member;
};

class ROSIDLC_ArrayMember : public ArrayValueMember, public ROSIDLC_Member
{
public:
  using ROSIDLC_Member::get_member_data;
  using ROSIDLC_Member::ROSIDLC_Member;

  size_t array_size() const final {return impl.array_size_;}
  UntypedSpan array_contents(const void * struct_data) const final
  {
    return {get_member_data(struct_data), array_size() * get_value_type()->sizeof_type()};
  }
};

class ROSIDLC_SequenceMember : public SpanSequenceValueMember, public ROSIDLC_Member
{
protected:
  using ROSIDLC_Member::get_member_data;

public:
  using ROSIDLC_Member::get_value_type;
  using ROSIDLC_Member::ROSIDLC_Member;

  size_t sequence_size(const void * struct_data) const final
  {
    return impl.size_function(get_member_data(struct_data));
  }
  UntypedSpan sequence_contents(const void * struct_data) const final
  {
    auto data = impl.get_const_function(get_member_data(struct_data), 0);
    return {data, sequence_size(struct_data) * get_value_type()->sizeof_type()};
  }
};

class ROSIDLCPP_SingleValueMember : public virtual SingleValueMember, public ROSIDLCPP_Member
{
public:
  using ROSIDLCPP_Member::get_member_data;
  using ROSIDLCPP_Member::ROSIDLCPP_Member;
};

class ROSIDLCPP_ArrayMember : public ArrayValueMember, public ROSIDLCPP_Member
{
public:
  using ROSIDLCPP_Member::get_member_data;
  using ROSIDLCPP_Member::ROSIDLCPP_Member;

  size_t array_size() const final {return impl.array_size_;}
  UntypedSpan array_contents(const void * struct_data) const final
  {
    return {get_member_data(struct_data), array_size() * get_value_type()->sizeof_type()};
  }
};

class ROSIDLCPP_SpanSequenceMember : public SpanSequenceValueMember, public ROSIDLCPP_Member
{
public:
  using ROSIDLCPP_Member::get_member_data;
  using ROSIDLCPP_Member::get_value_type;
  using ROSIDLCPP_Member::ROSIDLCPP_Member;

  size_t sequence_size(const void * struct_data) const final
  {
    return impl.size_function(get_member_data(struct_data));
  }
  UntypedSpan sequence_contents(const void * struct_data) const final
  {
    auto member_data = get_member_data(struct_data);
    auto size = impl.size_function(member_data);
    auto data = impl.get_const_function(member_data, 0);
    return {data, size * get_value_type()->sizeof_type()};
  }
};

class ROSIDLCPP_BoolSequenceMember : public ROSIDLCPP_Member
{
public:
  using ROSIDLCPP_Member::ROSIDLCPP_Member;

  std::unique_ptr<AnyValueType> value_type;
  size_t get_size(void * data) const {return static_cast<std::vector<bool> *>(data)->size();}
  const std::vector<bool> & get_bool_vector(const void * struct_data) const
  {
    return *static_cast<const std::vector<bool> *>(get_member_data(struct_data));
  }
};

class ROSIDLC_Member;
class ROSIDLC_StructValueType;

enum class MemberContainerType { Array, Sequence, SingleValue };

class AnyU8StringValueType : public AnyValueType
{
public:
  using char_traits = std::char_traits<char>;
  ROSIDL_TypeKind type_kind() const final {return ROSIDL_TypeKind::STRING;}
  virtual TypedSpan<char_traits::char_type> data(void *) const = 0;
  virtual TypedSpan<const char_traits::char_type> data(const void *) const = 0;
};

class AnyU16StringValueType : public AnyValueType
{
public:
  using char_traits = std::char_traits<char16_t>;
  ROSIDL_TypeKind type_kind() const final {return ROSIDL_TypeKind::WSTRING;}
  virtual TypedSpan<char_traits::char_type> data(void *) const = 0;
  virtual TypedSpan<const char_traits::char_type> data(const void *) const = 0;
};

struct ROSIDLC_StringValueType : public AnyU8StringValueType
{
public:
  using type = rosidl_generator_c__String;

  TypedSpan<const char_traits::char_type> data(const void * ptr) const override
  {
    auto str = static_cast<const type *>(ptr);
    return {str->data, str->size};
  }
  TypedSpan<char_traits::char_type> data(void * ptr) const override
  {
    auto str = static_cast<type *>(ptr);
    return {str->data, str->size};
  }
  size_t sizeof_type() const override {return sizeof(type);}
};

class ROSIDLC_WStringValueType : public AnyU16StringValueType
{
public:
  using type = rosidl_generator_c__U16String;
  TypedSpan<const char_traits::char_type> data(const void * ptr) const override
  {
    auto str = static_cast<const type *>(ptr);
    return {reinterpret_cast<const char_traits::char_type *>(str->data), str->size};
  }
  TypedSpan<char_traits::char_type> data(void * ptr) const override
  {
    auto str = static_cast<type *>(ptr);
    return {reinterpret_cast<char_traits::char_type *>(str->data), str->size};
  }
  size_t sizeof_type() const override {return sizeof(type);}
};
class ROSIDLCPP_StringValueType : public AnyU8StringValueType
{
public:
  using type = std::string;

  TypedSpan<const char_traits::char_type> data(const void * ptr) const override
  {
    auto str = static_cast<const type *>(ptr);
    return {str->data(), str->size()};
  }
  TypedSpan<char_traits::char_type> data(void * ptr) const override
  {
    auto str = static_cast<type *>(ptr);
    return {str->data(), str->size()};
  }
  size_t sizeof_type() const override {return sizeof(type);}
};

class ROSIDLCPP_U16StringValueType : public AnyU16StringValueType
{
public:
  using type = std::u16string;

  TypedSpan<const char_traits::char_type> data(const void * ptr) const override
  {
    auto str = static_cast<const type *>(ptr);
    return {str->data(), str->size()};
  }
  TypedSpan<char_traits::char_type> data(void * ptr) const override
  {
    auto str = static_cast<type *>(ptr);
    return {str->data(), str->size()};
  }
  size_t sizeof_type() const override {return sizeof(type);}
};
}  // namespace rmw_cyclonedds_cpp
