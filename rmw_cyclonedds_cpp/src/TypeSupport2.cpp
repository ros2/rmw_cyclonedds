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
#include "rosidl_buffer/buffer.hpp"
#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/primitives_sequence.h"
#include "rosidl_runtime_c/primitives_sequence_functions.h"
#include "rosidl_runtime_c/service_type_support_struct.h"

namespace rmw_cyclonedds_cpp
{
const PrimitiveValueType primitive_value_type_boolean =
  PrimitiveValueType(ROSIDL_TypeKind::BOOLEAN);

namespace
{

using UInt8Buffer = rosidl::Buffer<uint8_t>;

bool is_cpu_buffer(const UInt8Buffer & buffer)
{
  return dynamic_cast<const rosidl::CpuBufferImpl<uint8_t> *>(buffer.get_impl()) != nullptr;
}

const void * buffer_serialization_contents(
  const UInt8Buffer & buffer, std::vector<uint8_t> & cpu_storage)
{
  if (is_cpu_buffer(buffer)) {
    return buffer.data();
  }
  cpu_storage = buffer.to_vector();
  return cpu_storage.data();
}

class ROSIDLCPP_BufferSequenceValueType : public SpanSequenceValueType
{
  const AnyValueType * m_element_value_type;

  static const UInt8Buffer * get_value(const void * ptr_to_sequence)
  {
    return static_cast<const UInt8Buffer *>(ptr_to_sequence);
  }

  static UInt8Buffer * get_value(void * ptr_to_sequence)
  {
    return static_cast<UInt8Buffer *>(ptr_to_sequence);
  }

public:
  explicit ROSIDLCPP_BufferSequenceValueType(
    const AnyValueType * element_value_type, uint32_t bound)
  : SpanSequenceValueType(bound), m_element_value_type(element_value_type)
  {
    assert(m_element_value_type);
  }

  size_t sizeof_type() const override {return sizeof(UInt8Buffer);}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {throw std::logic_error("not implemented");}
  const AnyValueType * element_value_type() const override {return m_element_value_type;}
  size_t sequence_size(const void * ptr_to_sequence) const override
  {
    return get_value(ptr_to_sequence)->size();
  }
  const void * sequence_contents(const void * ptr_to_sequence) const override
  {
    return get_value(ptr_to_sequence)->data();
  }
  const void * serialization_contents(
    const void * ptr_to_sequence, std::vector<uint8_t> & cpu_storage,
    bool data_required) const override
  {
    if (!data_required) {
      return nullptr;
    }
    return buffer_serialization_contents(*get_value(ptr_to_sequence), cpu_storage);
  }
  void * sequence_contents(void * ptr_to_sequence) const override
  {
    return get_value(ptr_to_sequence)->data();
  }
  void resize(void * ptr_to_sequence, size_t size) const override
  {
    auto & buffer = *get_value(ptr_to_sequence);
    if (is_cpu_buffer(buffer)) {
      buffer.resize(size);
    } else {
      buffer = UInt8Buffer(size);
    }
  }
};

class ROSIDLC_BufferSequenceValueType : public SpanSequenceValueType
{
  const AnyValueType * m_element_value_type;

  static const rosidl_runtime_c__uint8__Sequence * get_value(const void * ptr_to_sequence)
  {
    return static_cast<const rosidl_runtime_c__uint8__Sequence *>(ptr_to_sequence);
  }

  static rosidl_runtime_c__uint8__Sequence * get_value(void * ptr_to_sequence)
  {
    return static_cast<rosidl_runtime_c__uint8__Sequence *>(ptr_to_sequence);
  }

  static const UInt8Buffer * get_buffer(const rosidl_runtime_c__uint8__Sequence & sequence)
  {
    return reinterpret_cast<const UInt8Buffer *>(sequence.data);
  }

  static UInt8Buffer * get_buffer(rosidl_runtime_c__uint8__Sequence & sequence)
  {
    return reinterpret_cast<UInt8Buffer *>(sequence.data);
  }

public:
  explicit ROSIDLC_BufferSequenceValueType(
    const AnyValueType * element_value_type, uint32_t bound)
  : SpanSequenceValueType(bound), m_element_value_type(element_value_type)
  {
    assert(m_element_value_type);
  }

  size_t sizeof_type() const override {return sizeof(rosidl_runtime_c__uint8__Sequence);}
  size_t cdrsizeof_type() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_type() const override {throw std::logic_error("not implemented");}
  const AnyValueType * element_value_type() const override {return m_element_value_type;}
  size_t sequence_size(const void * ptr_to_sequence) const override
  {
    const auto & sequence = *get_value(ptr_to_sequence);
    return sequence.is_rosidl_buffer ? get_buffer(sequence)->size() : sequence.size;
  }
  const void * sequence_contents(const void * ptr_to_sequence) const override
  {
    const auto & sequence = *get_value(ptr_to_sequence);
    return sequence.is_rosidl_buffer ? get_buffer(sequence)->data() : sequence.data;
  }
  const void * serialization_contents(
    const void * ptr_to_sequence, std::vector<uint8_t> & cpu_storage,
    bool data_required) const override
  {
    if (!data_required) {
      return nullptr;
    }
    const auto & sequence = *get_value(ptr_to_sequence);
    if (sequence.is_rosidl_buffer) {
      return buffer_serialization_contents(*get_buffer(sequence), cpu_storage);
    }
    return sequence.data;
  }
  void * sequence_contents(void * ptr_to_sequence) const override
  {
    auto & sequence = *get_value(ptr_to_sequence);
    return sequence.is_rosidl_buffer ? get_buffer(sequence)->data() : sequence.data;
  }
  void resize(void * ptr_to_sequence, size_t size) const override
  {
    auto * sequence = get_value(ptr_to_sequence);
    rosidl_runtime_c__uint8__Sequence__fini(sequence);
    if (!rosidl_runtime_c__uint8__Sequence__init(sequence, size)) {
      throw std::bad_alloc();
    }
  }
};

void validate_buffer_member(ROSIDL_TypeKind type_kind, bool is_array)
{
  if (!is_array || type_kind != ROSIDL_TypeKind::UINT8) {
    throw std::logic_error("rosidl buffer fields must be uint8 sequences");
  }
}

}  // namespace

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
  size_t cdrsizeof_struct() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_struct() const override {throw std::logic_error("not implemented");}
  TypeGenerator type_generator() const override {return gen;}
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
  size_t cdrsizeof_struct() const override {throw std::logic_error("not implemented");}
  size_t cdralignof_struct() const override {throw std::logic_error("not implemented");}
  TypeGenerator type_generator() const override {return gen;}
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
  bool has_keys = false;
  bool is_self_contained = true;
  for (size_t index = 0; index < impl->member_count_; index++) {
    auto member_impl = impl->members_[index];

    const AnyValueType * element_value_type;
    switch (ROSIDL_TypeKind(member_impl.type_id_)) {
      case ROSIDL_TypeKind::MESSAGE:
        m_inner_value_types.push_back(make_message_value_type(member_impl.members_));
        element_value_type = m_inner_value_types.back().get();
        break;
      case ROSIDL_TypeKind::STRING:
        element_value_type = make_value_type<ROSIDLC_StringValueType>(UINT32_MAX - 1);
        break;
      case ROSIDL_TypeKind::WSTRING:
        element_value_type = make_value_type<ROSIDLC_WStringValueType>(UINT32_MAX / 2);
        break;
      default:
        element_value_type =
          make_value_type<PrimitiveValueType>(ROSIDL_TypeKind(member_impl.type_id_));
        break;
    }

    const AnyValueType * member_value_type;
    if (member_impl.is_array_ && member_impl.array_size_ > UINT32_MAX) {
      throw std::length_error("arrays > UINT32_MAX not supported");
    }
    uint32_t bound = UINT32_MAX;
    if (member_impl.is_array_ && member_impl.array_size_ != 0 && member_impl.is_upper_bound_) {
      bound = static_cast<uint32_t>(member_impl.array_size_);
    }
    if (member_impl.is_rosidl_buffer_) {
      validate_buffer_member(ROSIDL_TypeKind(member_impl.type_id_), member_impl.is_array_);
      member_value_type = make_value_type<ROSIDLC_BufferSequenceValueType>(
        element_value_type, bound);
    } else if (!member_impl.is_array_) {
      member_value_type = element_value_type;
    } else if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
      member_value_type = make_value_type<ArrayValueType>(
        element_value_type, member_impl.array_size_);
    } else if (member_impl.size_function) {
      member_value_type = make_value_type<CallbackSpanSequenceValueType>(
        element_value_type,
        bound,
        member_impl.size_function,
        member_impl.get_const_function,
        member_impl.get_function,
        [member_impl](void * p, size_t s){
          if (!member_impl.resize_function(p, s)) {throw std::bad_alloc();}
        });
    } else {
      member_value_type = make_value_type<ROSIDLC_SpanSequenceValueType>(
        element_value_type,
        bound,
        member_impl.resize_function);
    }
    if (member_impl.is_key_) {
      has_keys = true;
    }
    if (!member_value_type->is_self_contained()) {
      is_self_contained = false;
    }
    m_members.push_back(
      Member{
        member_impl.name_,
        member_value_type,
        member_impl.offset_,
        member_impl.is_key_
      });
  }
  m_has_keys = has_keys;
  m_is_self_contained = is_self_contained;
}

ROSIDLCPP_StructValueType::ROSIDLCPP_StructValueType(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl)
: impl(impl)
{
  bool has_keys = false;
  bool is_self_contained = true;
  for (size_t index = 0; index < impl->member_count_; index++) {
    auto member_impl = impl->members_[index];

    const AnyValueType * element_value_type;
    switch (ROSIDL_TypeKind(member_impl.type_id_)) {
      case ROSIDL_TypeKind::MESSAGE:
        m_inner_value_types.push_back(make_message_value_type(member_impl.members_));
        element_value_type = m_inner_value_types.back().get();
        break;
      case ROSIDL_TypeKind::STRING:
        element_value_type = make_value_type<ROSIDLCPP_StringValueType>(UINT32_MAX - 1);
        break;
      case ROSIDL_TypeKind::WSTRING:
        element_value_type = make_value_type<ROSIDLCPP_U16StringValueType>(UINT32_MAX / 2);
        break;
      default:
        element_value_type =
          make_value_type<PrimitiveValueType>(ROSIDL_TypeKind(member_impl.type_id_));
        break;
    }

    const AnyValueType * member_value_type;
    if (member_impl.is_array_ && member_impl.array_size_ > UINT32_MAX) {
      throw std::length_error("arrays > UINT32_MAX not supported");
    }
    uint32_t bound = UINT32_MAX;
    if (member_impl.is_array_ && member_impl.array_size_ != 0 && member_impl.is_upper_bound_) {
      bound = static_cast<uint32_t>(member_impl.array_size_);
    }
    if (member_impl.is_rosidl_buffer_) {
      validate_buffer_member(ROSIDL_TypeKind(member_impl.type_id_), member_impl.is_array_);
      member_value_type = make_value_type<ROSIDLCPP_BufferSequenceValueType>(
        element_value_type, bound);
    } else if (!member_impl.is_array_) {
      member_value_type = element_value_type;
    } else if (member_impl.array_size_ != 0 && !member_impl.is_upper_bound_) {
      member_value_type = make_value_type<ArrayValueType>(
        element_value_type, member_impl.array_size_);
    } else if (ROSIDL_TypeKind(member_impl.type_id_) == ROSIDL_TypeKind::BOOLEAN) {
      member_value_type =
        make_value_type<BoolVectorValueType>(
        bound);
    } else {
      member_value_type = make_value_type<CallbackSpanSequenceValueType>(
        element_value_type, bound,
        member_impl.size_function, member_impl.get_const_function, member_impl.get_function,
        member_impl.resize_function);
    }
    if (member_impl.is_key_) {
      has_keys = true;
    }
    if (!member_value_type->is_self_contained()) {
      is_self_contained = false;
    }
    m_members.push_back(
      Member {
        member_impl.name_,
        member_value_type,
        member_impl.offset_,
        member_impl.is_key_
      });
  }
  m_has_keys = has_keys;
  m_is_self_contained = is_self_contained;
}
}  // namespace rmw_cyclonedds_cpp
