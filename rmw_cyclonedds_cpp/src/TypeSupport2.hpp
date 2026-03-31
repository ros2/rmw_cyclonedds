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
#ifndef TYPESUPPORT2_HPP_
#define TYPESUPPORT2_HPP_

#include <cassert>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <stdexcept>

#include "rosidl_runtime_c/string_functions.h"
#include "rosidl_runtime_c/u16string_functions.h"
#include "rosidl_typesupport_introspection_c/identifier.h"
#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_introspection_c/service_introspection.h"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"

namespace rmw_cyclonedds_cpp
{
/// Stub for code that should never be reachable by design.
/// If it is possible to reach the code due to bad data or other runtime conditions,
/// use a runtime_error instead
[[noreturn]] inline void unreachable()
{
#if defined(__has_builtin)
#if __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#endif
#elif (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5))
  __builtin_unreachable();
#endif
  throw std::logic_error("This code should be unreachable.");
}

struct AnyValueType;

/// contiguous storage objects
template<typename T>
class TypedSpan;

template<typename T>
class TypedSpan
{
  T * m_data;
  const size_t m_size;

public:
  TypedSpan(T * data, size_t size)
  : m_data(data), m_size(size)
  {
  }

  size_t size() const {return m_size;}
  size_t size_bytes() const {return size() * sizeof(T);}
  T * data() const {return m_data;}

  auto begin() {return m_data;}
  auto end() {return m_data + size();}
};

template<typename NativeType>
auto make_typed_span(const NativeType * m_data, size_t size)
{
  return TypedSpan<NativeType>{m_data, size};
}

enum class TypeGenerator
{
  ROSIDL_C,
  ROSIDL_Cpp,
};

template<TypeGenerator>
struct TypeGeneratorInfo;

template<>
struct TypeGeneratorInfo<TypeGenerator::ROSIDL_C>
{
  static constexpr auto enum_value = TypeGenerator::ROSIDL_C;
  static const auto & get_identifier() {return rosidl_typesupport_introspection_c__identifier;}
  using MetaMessage = rosidl_typesupport_introspection_c__MessageMembers;
  using MetaMember = rosidl_typesupport_introspection_c__MessageMember;
  using MetaService = rosidl_typesupport_introspection_c__ServiceMembers;
};

template<>
struct TypeGeneratorInfo<TypeGenerator::ROSIDL_Cpp>
{
  static constexpr auto enum_value = TypeGenerator::ROSIDL_Cpp;
  static const auto & get_identifier()
  {
    return rosidl_typesupport_introspection_cpp::typesupport_identifier;
  }
  using MetaMessage = rosidl_typesupport_introspection_cpp::MessageMembers;
  using MetaMember = rosidl_typesupport_introspection_cpp::MessageMember;
  using MetaService = rosidl_typesupport_introspection_cpp::ServiceMembers;
};

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


class StructValueType;
std::unique_ptr<StructValueType> make_message_value_type(const rosidl_message_type_support_t * mts);

std::pair<std::unique_ptr<StructValueType>, std::unique_ptr<StructValueType>>
make_request_response_value_types(const rosidl_service_type_support_t * svc);

enum class EValueType
{
  // the logical value type
  PrimitiveValueType,
  U8StringValueType,
  U16StringValueType,
  StructValueType,
  ArrayValueType,
  SpanSequenceValueType,
  BoolVectorValueType,
};

struct AnyValueType
{
  // represents not just the IDL value but also its physical representation
  virtual ~AnyValueType() = default;

  // how many bytes this value type takes up
  virtual size_t sizeof_type() const = 0;

  // how many bytes this value type takes up in CDR if primitive, 0 if not primitive
  virtual size_t cdrsizeof_type() const = 0;

  // alignment for this type in CDR if primitive, 0 if not primitive
  virtual size_t cdralignof_type() const = 0;

  // represents the logical value type and supports the 'apply' function
  virtual EValueType e_value_type() const = 0;

  virtual bool is_self_contained() const = 0;

  // faster alternative to dynamic cast
  template<typename UnaryFunction>
  auto apply(UnaryFunction f) const;

  template<typename UnaryFunction>
  auto apply(UnaryFunction f);
};

struct Member
{
  const char * name;
  const AnyValueType * value_type;
  size_t member_offset;
  bool is_key;
};

class StructValueType : public AnyValueType
{
protected:
  bool m_has_keys;
  bool m_is_self_contained;

public:
  ROSIDL_TypeKind type_kind() const {return ROSIDL_TypeKind::MESSAGE;}
  size_t sizeof_type() const final {return sizeof_struct();}
  size_t cdrsizeof_type() const final {return cdrsizeof_struct();}
  size_t cdralignof_type() const final {return cdralignof_struct();}
  bool is_self_contained() const final {return m_is_self_contained;}
  virtual size_t sizeof_struct() const = 0;
  virtual size_t cdrsizeof_struct() const = 0;
  virtual size_t cdralignof_struct() const = 0;
  virtual size_t n_members() const = 0;
  virtual TypeGenerator type_generator() const = 0;
  virtual const Member * get_member(size_t) const = 0;
  bool has_keys() const {return m_has_keys;}
  EValueType e_value_type() const final {return EValueType::StructValueType;}
};

class ArrayValueType : public AnyValueType
{
protected:
  const AnyValueType * m_element_value_type;
  size_t m_size;
  bool m_is_self_contained;

public:
  ArrayValueType(const AnyValueType * element_value_type, size_t size)
  : m_element_value_type(element_value_type), m_size(size)
  {
  }
  const AnyValueType * element_value_type() const {return m_element_value_type;}
  size_t sizeof_type() const final {return m_size * m_element_value_type->sizeof_type();}
  size_t cdrsizeof_type() const final {return sizeof_type();}
  size_t cdralignof_type() const final {return m_element_value_type->cdralignof_type();}
  bool is_self_contained() const final {return m_is_self_contained;}
  size_t array_size() const {return m_size;}
  const void * get_data(const void * ptr_to_array) const {return ptr_to_array;}
  void * get_data(void * ptr_to_array) const {return ptr_to_array;}
  EValueType e_value_type() const final {return EValueType::ArrayValueType;}
};

class SpanSequenceValueType : public AnyValueType
{
protected:
  uint32_t m_bound;

  SpanSequenceValueType() = delete;
  explicit SpanSequenceValueType(uint32_t bound)
  : m_bound(bound) {}

public:
  using AnyValueType::sizeof_type;
  using AnyValueType::cdrsizeof_type;
  using AnyValueType::cdralignof_type;
  bool is_self_contained() const final {return false;}
  virtual const AnyValueType * element_value_type() const = 0;
  virtual size_t sequence_size(const void * ptr_to_sequence) const = 0;
  virtual const void * sequence_contents(const void * ptr_to_sequence) const = 0;
  virtual void * sequence_contents(void * ptr_to_sequence) const = 0;
  virtual void resize(void * ptr_to_sequence, size_t size) const = 0;
  uint32_t sequence_bound() const {return m_bound;}
  EValueType e_value_type() const final {return EValueType::SpanSequenceValueType;}
};

class CallbackSpanSequenceValueType : public SpanSequenceValueType
{
protected:
  const AnyValueType * m_element_value_type;
  std::function<size_t(const void *)> m_size_function;
  std::function<const void * (const void *, size_t index)> m_get_const_function;
  std::function<void * (void *, size_t index)> m_get_function;
  std::function<void(void *, size_t size)> m_resize_function;

public:
  CallbackSpanSequenceValueType(
    const AnyValueType * element_value_type, uint32_t bound,
    decltype(m_size_function) size_function,
    decltype(m_get_const_function) get_const_function, decltype(m_get_function) get_function,
    decltype(m_resize_function) resize_function)
  : SpanSequenceValueType(bound),
    m_element_value_type(element_value_type),
    m_size_function(size_function),
    m_get_const_function(get_const_function),
    m_get_function(get_function),
    m_resize_function(resize_function)
  {
    assert(m_element_value_type);
    assert(size_function);
    assert(get_const_function);
    assert(get_function);
    assert(resize_function);
  }

  size_t sizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {throw std::logic_error("not implemented");}
  const AnyValueType * element_value_type() const override {return m_element_value_type;}
  size_t sequence_size(const void * ptr_to_sequence) const override
  {
    return m_size_function(ptr_to_sequence);
  }
  const void * sequence_contents(const void * ptr_to_sequence) const override
  {
    if (sequence_size(ptr_to_sequence) == 0) {
      return nullptr;
    }
    return m_get_const_function(ptr_to_sequence, 0);
  }
  void * sequence_contents(void * ptr_to_sequence) const override
  {
    if (sequence_size(ptr_to_sequence) == 0) {
      return nullptr;
    }
    return m_get_function(ptr_to_sequence, 0);
  }

  void resize(void * ptr_to_sequence, size_t size) const override
  {
    m_resize_function(ptr_to_sequence, size);
  }
};

class ROSIDLC_SpanSequenceValueType : public SpanSequenceValueType
{
protected:
  const AnyValueType * m_element_value_type;
  struct ROSIDLC_SequenceObject
  {
    void * data;
    size_t size;     /*!< The number of valid items in data */
    size_t capacity; /*!< The number of allocated items in data */
  };
  std::function<bool(void *, size_t size)> m_resize_function;

  const ROSIDLC_SequenceObject * get_value(const void * ptr_to_sequence) const
  {
    return static_cast<const ROSIDLC_SequenceObject *>(ptr_to_sequence);
  }

  ROSIDLC_SequenceObject * get_value(void * ptr_to_sequence) const
  {
    return static_cast<ROSIDLC_SequenceObject *>(ptr_to_sequence);
  }

public:
  explicit ROSIDLC_SpanSequenceValueType(
    const AnyValueType * element_value_type, uint32_t bound,
    decltype(m_resize_function) resize_function)
  : SpanSequenceValueType(bound),
    m_element_value_type(element_value_type),
    m_resize_function(resize_function)
  {
    assert(resize_function);
  }

  size_t sizeof_type() const override {return sizeof(ROSIDLC_SequenceObject);}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {throw std::logic_error("not implemented");}
  const AnyValueType * element_value_type() const override {return m_element_value_type;}
  size_t sequence_size(const void * ptr_to_sequence) const override
  {
    return get_value(ptr_to_sequence)->size;
  }
  const void * sequence_contents(const void * ptr_to_sequence) const final
  {
    return get_value(ptr_to_sequence)->data;
  }
  void * sequence_contents(void * ptr_to_sequence) const final
  {
    return get_value(ptr_to_sequence)->data;
  }
  void resize(void * ptr_to_sequence, size_t size) const final
  {
    if (!m_resize_function(ptr_to_sequence, size)) {
      throw;
    }
  }
};

struct PrimitiveValueType : public AnyValueType
{
  const ROSIDL_TypeKind m_type_kind;

  explicit constexpr PrimitiveValueType(ROSIDL_TypeKind type_kind)
  : m_type_kind(type_kind)
  {
    assert(type_kind != ROSIDL_TypeKind::STRING);
    assert(type_kind != ROSIDL_TypeKind::WSTRING);
    assert(type_kind != ROSIDL_TypeKind::MESSAGE);
  }
  ROSIDL_TypeKind type_kind() const {return m_type_kind;}
  size_t sizeof_type() const final
  {
    switch (m_type_kind) {
      case ROSIDL_TypeKind::FLOAT:
        return sizeof(float);
      case ROSIDL_TypeKind::DOUBLE:
        return sizeof(double);
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
        break;
    }
    unreachable();
  }
  size_t cdrsizeof_type() const final
  {
    switch (m_type_kind) {
      case ROSIDL_TypeKind::BOOLEAN:
      case ROSIDL_TypeKind::OCTET:
      case ROSIDL_TypeKind::UINT8:
      case ROSIDL_TypeKind::INT8:
      case ROSIDL_TypeKind::CHAR:
        return 1;
      case ROSIDL_TypeKind::UINT16:
      case ROSIDL_TypeKind::INT16:
      case ROSIDL_TypeKind::WCHAR:
        return 2;
      case ROSIDL_TypeKind::UINT32:
      case ROSIDL_TypeKind::INT32:
      case ROSIDL_TypeKind::FLOAT:
        return 4;
      case ROSIDL_TypeKind::UINT64:
      case ROSIDL_TypeKind::INT64:
      case ROSIDL_TypeKind::DOUBLE:
        return 8;
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
        return 0;
    }
    unreachable();
  }
  bool is_self_contained() const final {return true;}
  size_t cdralignof_type() const final {return cdrsizeof_type();}
  EValueType e_value_type() const override {return EValueType::PrimitiveValueType;}
};

extern const PrimitiveValueType primitive_value_type_boolean;

class BoolVectorValueType : public AnyValueType
{
protected:
  uint32_t m_bound;

  const std::vector<bool> * get_value(const void * ptr_to_sequence) const
  {
    return static_cast<const std::vector<bool> *>(ptr_to_sequence);
  }

  std::vector<bool> * get_value(void * ptr_to_sequence) const
  {
    return static_cast<std::vector<bool> *>(ptr_to_sequence);
  }

  std::unique_ptr<PrimitiveValueType> s_element_value_type;

public:
  BoolVectorValueType() = delete;
  explicit BoolVectorValueType(uint32_t bound)
  : m_bound(bound) {}

  size_t sizeof_type() const override {return sizeof(std::vector<bool>);}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {throw std::logic_error("not implemented");}
  bool is_self_contained() const final {return false;}

  const AnyValueType * element_value_type() const
  {
    return &primitive_value_type_boolean;
  }

  std::vector<bool>::const_iterator begin(const void * ptr_to_sequence) const
  {
    return get_value(ptr_to_sequence)->begin();
  }
  std::vector<bool>::const_iterator end(const void * ptr_to_sequence) const
  {
    return get_value(ptr_to_sequence)->end();
  }
  virtual void assign(void * ptr_to_sequence, const uint8_t * src, size_t n) const
  {
    std::vector<bool> * seq = get_value(ptr_to_sequence);
    seq->resize(n);
    for (size_t i = 0; i < n; i++) {
      (*seq)[i] = (src[i] != 0);
    }
  }

  size_t size(const void * ptr_to_sequence) const {return get_value(ptr_to_sequence)->size();}
  uint32_t sequence_bound() const {return m_bound;}
  EValueType e_value_type() const final {return EValueType::BoolVectorValueType;}
};

class ROSIDLC_StructValueType;

class U8StringValueType : public AnyValueType
{
protected:
  uint32_t m_bound;

  U8StringValueType() = delete;
  explicit U8StringValueType(uint32_t bound)
  : m_bound(bound)
  {
    if (bound > UINT32_MAX - 1) {
      throw std::logic_error("oversize string bound (> UINT32_MAX-1)");
    }
  }

public:
  using char_traits = std::char_traits<char>;
  virtual TypedSpan<char_traits::char_type> data(void *) const = 0;
  virtual TypedSpan<const char_traits::char_type> data(const void *) const = 0;
  virtual void assign(void *, const TypedSpan<const char_traits::char_type> &) const = 0;
  uint32_t string_bound() const {return m_bound;}
  bool is_self_contained() const {return false;}
  EValueType e_value_type() const final {return EValueType::U8StringValueType;}
};

class U16StringValueType : public AnyValueType
{
protected:
  uint32_t m_bound;

  U16StringValueType() = delete;
  explicit U16StringValueType(uint32_t bound)
  : m_bound(bound)
  {
    if (bound > UINT32_MAX / 2) {
      throw std::logic_error("oversize string bound (> UINT32_MAX/2)");
    }
  }

public:
  using char_traits = std::char_traits<char16_t>;
  virtual TypedSpan<char_traits::char_type> data(void *) const = 0;
  virtual TypedSpan<const char_traits::char_type> data(const void *) const = 0;
  virtual void assign(void *, const TypedSpan<const char_traits::char_type> &) const = 0;
  uint32_t string_bound() const {return m_bound;}
  bool is_self_contained() const final {return false;}
  EValueType e_value_type() const final {return EValueType::U16StringValueType;}
};

struct ROSIDLC_StringValueType : public U8StringValueType
{
public:
  using type = rosidl_runtime_c__String;

  ROSIDLC_StringValueType() = delete;
  explicit ROSIDLC_StringValueType(uint32_t bound)
  : U8StringValueType(bound) {}

  TypedSpan<const char_traits::char_type> data(const void * ptr) const override
  {
    auto str = static_cast<const type *>(ptr);
    assert(str->capacity == str->size + 1);
    assert(str->data[str->size] == '\0');
    return {str->data, str->size};
  }
  TypedSpan<char_traits::char_type> data(void * ptr) const override
  {
    auto str = static_cast<type *>(ptr);
    assert(str->capacity == str->size + 1);
    assert(str->data[str->size + 1] == 0);
    return {str->data, str->size};
  }
  void assign(void * ptr, const TypedSpan<const char_traits::char_type> & src) const override
  {
    auto str = static_cast<type *>(ptr);
    rosidl_runtime_c__String__assignn(str, src.data(), src.size());
  }
  size_t sizeof_type() const override {return sizeof(type);}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {return 4;}
};

class ROSIDLC_WStringValueType : public U16StringValueType
{
public:
  using type = rosidl_runtime_c__U16String;

  ROSIDLC_WStringValueType() = delete;
  explicit ROSIDLC_WStringValueType(uint32_t bound)
  : U16StringValueType(bound) {}

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
  void assign(void * ptr, const TypedSpan<const char_traits::char_type> & src) const override
  {
    auto str = static_cast<type *>(ptr);
    rosidl_runtime_c__U16String__assignn(
      str, reinterpret_cast<const uint16_t *>(src.data()),
      src.size());
  }
  size_t sizeof_type() const override {return sizeof(type);}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {return 4;}
};

class ROSIDLCPP_StringValueType : public U8StringValueType
{
public:
  using type = std::string;

  ROSIDLCPP_StringValueType() = delete;
  explicit ROSIDLCPP_StringValueType(uint32_t bound)
  : U8StringValueType(bound) {}

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
  void assign(void * ptr, const TypedSpan<const char_traits::char_type> & src) const override
  {
    auto str = static_cast<type *>(ptr);
    *str = std::string(src.data(), src.size());
  }
  size_t sizeof_type() const override {return sizeof(type);}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {return 4;}
};

class ROSIDLCPP_U16StringValueType : public U16StringValueType
{
public:
  using type = std::u16string;

  ROSIDLCPP_U16StringValueType() = delete;
  explicit ROSIDLCPP_U16StringValueType(uint32_t bound)
  : U16StringValueType(bound) {}

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
  void assign(void * ptr, const TypedSpan<const char_traits::char_type> & src) const override
  {
    auto str = static_cast<type *>(ptr);
    *str = std::u16string(src.data(), src.size());
  }
  size_t sizeof_type() const override {return sizeof(type);}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {return 4;}
};

template<typename UnaryFunction>
auto AnyValueType::apply(UnaryFunction f) const
{
  switch (e_value_type()) {
    case EValueType::PrimitiveValueType:
      return f(*static_cast<const PrimitiveValueType *>(this));
    case EValueType::U8StringValueType:
      return f(*static_cast<const U8StringValueType *>(this));
    case EValueType::U16StringValueType:
      return f(*static_cast<const U16StringValueType *>(this));
    case EValueType::StructValueType:
      return f(*static_cast<const StructValueType *>(this));
    case EValueType::ArrayValueType:
      return f(*static_cast<const ArrayValueType *>(this));
    case EValueType::SpanSequenceValueType:
      return f(*static_cast<const SpanSequenceValueType *>(this));
    case EValueType::BoolVectorValueType:
      return f(*static_cast<const BoolVectorValueType *>(this));
    default:
      unreachable();
  }
}

template<typename UnaryFunction>
auto AnyValueType::apply(UnaryFunction f)
{
  switch (e_value_type()) {
    case EValueType::PrimitiveValueType:
      return f(*static_cast<PrimitiveValueType *>(this));
    case EValueType::U8StringValueType:
      return f(*static_cast<U8StringValueType *>(this));
    case EValueType::U16StringValueType:
      return f(*static_cast<U16StringValueType *>(this));
    case EValueType::StructValueType:
      return f(*static_cast<StructValueType *>(this));
    case EValueType::ArrayValueType:
      return f(*static_cast<ArrayValueType *>(this));
    case EValueType::SpanSequenceValueType:
      return f(*static_cast<SpanSequenceValueType *>(this));
    case EValueType::BoolVectorValueType:
      return f(*static_cast<BoolVectorValueType *>(this));
    default:
      unreachable();
  }
}

}  // namespace rmw_cyclonedds_cpp
#endif  // TYPESUPPORT2_HPP_
