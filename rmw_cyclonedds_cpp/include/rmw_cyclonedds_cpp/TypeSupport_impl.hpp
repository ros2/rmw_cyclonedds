// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
// Copyright 2018 ADLINK Technology
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

#ifndef RMW_CYCLONEDDS_CPP__TYPESUPPORT_IMPL_HPP_
#define RMW_CYCLONEDDS_CPP__TYPESUPPORT_IMPL_HPP_

#include <cassert>
#include <functional>
#include <string>
#include <vector>

#include "rmw_cyclonedds_cpp/TypeSupport.hpp"
#include "rmw_cyclonedds_cpp/macros.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"

#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_introspection_c/service_introspection.h"

#include "rosidl_runtime_c/primitives_sequence_functions.h"
#include "rosidl_runtime_c/u16string_functions.h"

#include "serdes.hpp"
#include "u16string.hpp"

namespace rmw_cyclonedds_cpp
{

template<typename T>
struct GenericCSequence;

// multiple definitions of ambiguous primitive types
SPECIALIZE_GENERIC_C_SEQUENCE(bool, bool)
SPECIALIZE_GENERIC_C_SEQUENCE(byte, uint8_t)
SPECIALIZE_GENERIC_C_SEQUENCE(char, char)
SPECIALIZE_GENERIC_C_SEQUENCE(float32, float)
SPECIALIZE_GENERIC_C_SEQUENCE(float64, double)
SPECIALIZE_GENERIC_C_SEQUENCE(int8, int8_t)
SPECIALIZE_GENERIC_C_SEQUENCE(int16, int16_t)
SPECIALIZE_GENERIC_C_SEQUENCE(uint16, uint16_t)
SPECIALIZE_GENERIC_C_SEQUENCE(int32, int32_t)
SPECIALIZE_GENERIC_C_SEQUENCE(uint32, uint32_t)
SPECIALIZE_GENERIC_C_SEQUENCE(int64, int64_t)
SPECIALIZE_GENERIC_C_SEQUENCE(uint64, uint64_t)

typedef struct rosidl_runtime_c__void__Sequence
{
  void * data;
  /// The number of valid items in data
  size_t size;
  /// The number of allocated items in data
  size_t capacity;
} rosidl_runtime_c__void__Sequence;

inline
bool
rosidl_runtime_c__void__Sequence__init(
  rosidl_runtime_c__void__Sequence * sequence, size_t size, size_t member_size)
{
  if (!sequence) {
    return false;
  }
  void * data = nullptr;
  if (size) {
    data = static_cast<void *>(calloc(size, member_size));
    if (!data) {
      return false;
    }
  }
  sequence->data = data;
  sequence->size = size;
  sequence->capacity = size;
  return true;
}

inline
void
rosidl_runtime_c__void__Sequence__fini(rosidl_runtime_c__void__Sequence * sequence)
{
  if (!sequence) {
    return;
  }
  if (sequence->data) {
    // ensure that data and capacity values are consistent
    assert(sequence->capacity > 0);
    // finalize all sequence elements
    free(sequence->data);
    sequence->data = nullptr;
    sequence->size = 0;
    sequence->capacity = 0;
  } else {
    // ensure that data, size, and capacity values are consistent
    assert(0 == sequence->size);
    assert(0 == sequence->capacity);
  }
}

template<typename MembersType>
TypeSupport<MembersType>::TypeSupport()
{
  name = "";
}

template<typename MembersType>
void TypeSupport<MembersType>::setName(const std::string & name)
{
  this->name = std::string(name);
}

template<typename T>
static inline T
align_int_(size_t __align, T __int) noexcept
{
  return (__int - 1u + __align) & ~(__align - 1);
}

template<typename T>
static inline T *
align_ptr_(size_t __align, T * __ptr) noexcept
{
  const auto __intptr = reinterpret_cast<uintptr_t>(__ptr);
  const auto __aligned = align_int_(__align, __intptr);
  return reinterpret_cast<T *>(__aligned);
}

template<typename MembersType>
static size_t calculateMaxAlign(const MembersType * members)
{
  size_t max_align = 0;

  for (uint32_t i = 0; i < members->member_count_; ++i) {
    size_t alignment = 0;
    const auto & member = members->members_[i];

    if (member.is_array_ && (!member.array_size_ || member.is_upper_bound_)) {
      alignment = alignof(std::vector<unsigned char>);
    } else {
      switch (member.type_id_) {
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BOOL:
          alignment = alignof(bool);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BYTE:
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8:
          alignment = alignof(uint8_t);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_CHAR:
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT8:
          alignment = alignof(char);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT32:
          alignment = alignof(float);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT64:
          alignment = alignof(double);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT16:
          alignment = alignof(int16_t);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT16:
          alignment = alignof(uint16_t);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT32:
          alignment = alignof(int32_t);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT32:
          alignment = alignof(uint32_t);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT64:
          alignment = alignof(int64_t);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT64:
          alignment = alignof(uint64_t);
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING:
          // Note: specialization needed because calculateMaxAlign is called before
          // casting submembers as std::string, returned value is the same on i386
          if (std::is_same<MembersType,
            rosidl_typesupport_introspection_c__MessageMembers>::value)
          {
            alignment = alignof(rosidl_runtime_c__String);
          } else {
            alignment = alignof(std::string);
          }
          break;
        case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE:
          {
            auto sub_members = (const MembersType *)member.members_->data;
            alignment = calculateMaxAlign(sub_members);
          }
          break;
      }
    }

    if (alignment > max_align) {
      max_align = alignment;
    }
  }

  return max_align;
}

inline
size_t get_array_size_and_assign_field(
  const rosidl_typesupport_introspection_cpp::MessageMember * member,
  void * field,
  void * & subros_message,
  size_t sub_members_size,
  size_t max_align)
{
  auto vector = reinterpret_cast<std::vector<unsigned char> *>(field);
  size_t vsize = vector->size() / align_int_(max_align, sub_members_size);
  if (member->is_upper_bound_ && vsize > member->array_size_) {
    throw std::runtime_error("vector overcomes the maximum length");
  }
  subros_message = reinterpret_cast<void *>(vector->data());
  return vsize;
}

inline
size_t get_array_size_and_assign_field(
  const rosidl_typesupport_introspection_c__MessageMember * member,
  void * field,
  void * & subros_message,
  size_t, size_t)
{
  auto tmpsequence = static_cast<rosidl_runtime_c__void__Sequence *>(field);
  if (member->is_upper_bound_ && tmpsequence->size > member->array_size_) {
    throw std::runtime_error("vector overcomes the maximum length");
  }
  subros_message = reinterpret_cast<void *>(tmpsequence->data);
  return tmpsequence->size;
}

template<typename T>
void deserialize_field(
  const rosidl_typesupport_introspection_cpp::MessageMember * member,
  void * field,
  cycdeser & deser,
  bool call_new)
{
  if (!member->is_array_) {
    deser >> *static_cast<T *>(field);
  } else if (member->array_size_ && !member->is_upper_bound_) {
    deser.deserializeA(static_cast<T *>(field), member->array_size_);
  } else {
    auto & vector = *reinterpret_cast<std::vector<T> *>(field);
    if (call_new) {
      new(&vector) std::vector<T>;
    }
    deser >> vector;
  }
}

template<>
inline void deserialize_field<std::string>(
  const rosidl_typesupport_introspection_cpp::MessageMember * member,
  void * field,
  cycdeser & deser,
  bool call_new)
{
  if (!member->is_array_) {
    if (call_new) {
      // Because std::string is a complex datatype, we need to make sure that
      // the memory is initialized to something reasonable before eventually
      // passing it as a reference.
      new(field) std::string();
    }
    deser >> *static_cast<std::string *>(field);
  } else if (member->array_size_ && !member->is_upper_bound_) {
    std::string * array = static_cast<std::string *>(field);
    if (call_new) {
      for (size_t i = 0; i < member->array_size_; ++i) {
        new(&array[i]) std::string();
      }
    }
    deser.deserializeA(array, member->array_size_);
  } else {
    auto & vector = *reinterpret_cast<std::vector<std::string> *>(field);
    if (call_new) {
      new(&vector) std::vector<std::string>;
    }
    deser >> vector;
  }
}

template<>
inline void deserialize_field<std::wstring>(
  const rosidl_typesupport_introspection_cpp::MessageMember * member,
  void * field,
  cycdeser & deser,
  bool call_new)
{
  (void)call_new;
  std::wstring wstr;
  if (!member->is_array_) {
    deser >> wstr;
    wstring_to_u16string(
      wstr, *static_cast<std::u16string *>(field));
  } else {
    uint32_t size;
    if (member->array_size_ && !member->is_upper_bound_) {
      size = static_cast<uint32_t>(member->array_size_);
    } else {
      deser >> size;
      member->resize_function(field, size);
    }
    for (size_t i = 0; i < size; ++i) {
      void * element = member->get_function(field, i);
      auto u16str = static_cast<std::u16string *>(element);
      deser >> wstr;
      wstring_to_u16string(wstr, *u16str);
    }
  }
}

template<typename T>
void deserialize_field(
  const rosidl_typesupport_introspection_c__MessageMember * member,
  void * field,
  cycdeser & deser,
  bool call_new)
{
  (void)call_new;
  if (!member->is_array_) {
    deser >> *static_cast<T *>(field);
  } else if (member->array_size_ && !member->is_upper_bound_) {
    deser.deserializeA(static_cast<T *>(field), member->array_size_);
  } else {
    auto & data = *reinterpret_cast<typename GenericCSequence<T>::type *>(field);
    int32_t dsize = 0;
    deser >> dsize;
    GenericCSequence<T>::init(&data, dsize);
    deser.deserializeA(reinterpret_cast<T *>(data.data), dsize);
  }
}

template<>
inline void deserialize_field<std::string>(
  const rosidl_typesupport_introspection_c__MessageMember * member,
  void * field,
  cycdeser & deser,
  bool call_new)
{
  (void)call_new;
  if (!member->is_array_) {
    using CStringHelper = StringHelper<rosidl_typesupport_introspection_c__MessageMembers>;
    CStringHelper::assign(deser, field, call_new);
  } else {
    if (member->array_size_ && !member->is_upper_bound_) {
      auto deser_field = static_cast<rosidl_runtime_c__String *>(field);
      // tmpstring is defined here and not below to avoid
      // memory allocation in every iteration of the for loop
      std::string tmpstring;
      for (size_t i = 0; i < member->array_size_; ++i) {
        deser.deserialize(tmpstring);
        if (!rosidl_runtime_c__String__assign(&deser_field[i], tmpstring.c_str())) {
          throw std::runtime_error("unable to assign rosidl_runtime_c__String");
        }
      }
    } else {
      std::vector<std::string> cpp_string_vector;
      deser >> cpp_string_vector;

      auto & string_array_field = *reinterpret_cast<rosidl_runtime_c__String__Sequence *>(field);
      if (
        !rosidl_runtime_c__String__Sequence__init(
          &string_array_field, cpp_string_vector.size()))
      {
        throw std::runtime_error("unable to initialize rosidl_runtime_c__String array");
      }

      for (size_t i = 0; i < cpp_string_vector.size(); ++i) {
        if (
          !rosidl_runtime_c__String__assign(
            &string_array_field.data[i], cpp_string_vector[i].c_str()))
        {
          throw std::runtime_error("unable to assign rosidl_runtime_c__String");
        }
      }
    }
  }
}

template<>
inline void deserialize_field<std::wstring>(
  const rosidl_typesupport_introspection_c__MessageMember * member,
  void * field,
  cycdeser & deser,
  bool call_new)
{
  (void)call_new;
  std::wstring wstr;
  if (!member->is_array_) {
    deser >> wstr;
    wstring_to_u16string(
      wstr, *static_cast<rosidl_runtime_c__U16String *>(field));
  } else if (member->array_size_ && !member->is_upper_bound_) {
    auto array = static_cast<rosidl_runtime_c__U16String *>(field);
    for (size_t i = 0; i < member->array_size_; ++i) {
      deser >> wstr;
      wstring_to_u16string(wstr, array[i]);
    }
  } else {
    uint32_t size;
    deser >> size;
    auto sequence = static_cast<rosidl_runtime_c__U16String__Sequence *>(field);
    if (!rosidl_runtime_c__U16String__Sequence__init(sequence, size)) {
      throw std::runtime_error("unable to initialize rosidl_runtime_c__U16String sequence");
    }
    for (size_t i = 0; i < sequence->size; ++i) {
      deser >> wstr;
      wstring_to_u16string(wstr, sequence->data[i]);
    }
  }
}

inline size_t get_submessage_array_deserialize(
  const rosidl_typesupport_introspection_cpp::MessageMember * member,
  cycdeser & deser,
  void * field,
  void * & subros_message,
  bool call_new,
  size_t sub_members_size,
  size_t max_align)
{
  (void)member;
  uint32_t vsize = deser.deserialize_len(1);
  auto vector = reinterpret_cast<std::vector<unsigned char> *>(field);
  if (call_new) {
    new(vector) std::vector<unsigned char>;
  }
  vector->resize(vsize * align_int_(max_align, sub_members_size));
  subros_message = reinterpret_cast<void *>(vector->data());
  return vsize;
}

inline size_t get_submessage_array_deserialize(
  const rosidl_typesupport_introspection_c__MessageMember * member,
  cycdeser & deser,
  void * field,
  void * & subros_message,
  bool,
  size_t sub_members_size,
  size_t)
{
  (void)member;
  uint32_t vsize = deser.deserialize_len(1);
  auto tmparray = static_cast<rosidl_runtime_c__void__Sequence *>(field);
  rosidl_runtime_c__void__Sequence__init(tmparray, vsize, sub_members_size);
  subros_message = reinterpret_cast<void *>(tmparray->data);
  return vsize;
}

template<typename MembersType>
bool TypeSupport<MembersType>::deserializeROSmessage(
  cycdeser & deser, const MembersType * members, void * ros_message, bool call_new)
{
  assert(members);
  assert(ros_message);

  for (uint32_t i = 0; i < members->member_count_; ++i) {
    const auto * member = members->members_ + i;
    void * field = static_cast<char *>(ros_message) + member->offset_;
    switch (member->type_id_) {
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BOOL:
        deserialize_field<bool>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BYTE:
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8:
        deserialize_field<uint8_t>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_CHAR:
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT8:
        deserialize_field<char>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT32:
        deserialize_field<float>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT64:
        deserialize_field<double>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT16:
        deserialize_field<int16_t>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT16:
        deserialize_field<uint16_t>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT32:
        deserialize_field<int32_t>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT32:
        deserialize_field<uint32_t>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT64:
        deserialize_field<int64_t>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT64:
        deserialize_field<uint64_t>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING:
        deserialize_field<std::string>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_WSTRING:
        deserialize_field<std::wstring>(member, field, deser, call_new);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE:
        {
          auto sub_members = (const MembersType *)member->members_->data;
          if (!member->is_array_) {
            deserializeROSmessage(deser, sub_members, field, call_new);
          } else {
            void * subros_message = nullptr;
            size_t array_size = 0;
            size_t sub_members_size = sub_members->size_of_;
            size_t max_align = calculateMaxAlign(sub_members);
            bool recall_new = call_new;

            if (member->array_size_ && !member->is_upper_bound_) {
              subros_message = field;
              array_size = member->array_size_;
            } else {
              array_size = get_submessage_array_deserialize(
                member, deser, field, subros_message,
                call_new, sub_members_size, max_align);
              recall_new = true;
            }

            for (size_t index = 0; index < array_size; ++index) {
              deserializeROSmessage(deser, sub_members, subros_message, recall_new);
              subros_message = static_cast<char *>(subros_message) + sub_members_size;
              subros_message = align_ptr_(max_align, subros_message);
            }
          }
        }
        break;
      default:
        throw std::runtime_error("unknown type");
    }
  }

  return true;
}

template<typename M, typename T>
void print_field(const M * member, cycprint & deser, T & dummy)
{
  if (!member->is_array_) {
    deser >> dummy;
  } else {
    deser.print_constant("{");
    if (member->array_size_ && !member->is_upper_bound_) {
      deser.printA(&dummy, member->array_size_);
    } else {
      int32_t dsize = deser.get_len(1);
      deser.printA(&dummy, dsize);
    }
    deser.print_constant("}");
  }
}

template<typename MembersType>
bool TypeSupport<MembersType>::printROSmessage(
  cycprint & deser, const MembersType * members)
{
  assert(members);

  deser.print_constant("{");
  for (uint32_t i = 0; i < members->member_count_; ++i) {
    if (i != 0) {
      deser.print_constant(",");
    }
    const auto * member = members->members_ + i;
    switch (member->type_id_) {
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BOOL:
        {bool dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BYTE:
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8:
        {uint8_t dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_CHAR:
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT8:
        {char dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT32:
        {float dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT64:
        {double dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT16:
        {int16_t dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT16:
        {uint16_t dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT32:
        {int32_t dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT32:
        {uint32_t dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT64:
        {int64_t dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT64:
        {uint64_t dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING:
        {std::string dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_WSTRING:
        {std::wstring dummy; print_field(member, deser, dummy);}
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE:
        {
          auto sub_members = (const MembersType *)member->members_->data;
          if (!member->is_array_) {
            printROSmessage(deser, sub_members);
          } else {
            size_t array_size = 0;
            if (member->array_size_ && !member->is_upper_bound_) {
              array_size = member->array_size_;
            } else {
              array_size = deser.get_len(1);
            }
            deser.print_constant("{");
            for (size_t index = 0; index < array_size; ++index) {
              printROSmessage(deser, sub_members);
            }
            deser.print_constant("}");
          }
        }
        break;
      default:
        throw std::runtime_error("unknown type");
    }
  }
  deser.print_constant("}");

  return true;
}

template<typename MembersType>
bool TypeSupport<MembersType>::deserializeROSmessage(
  cycdeser & deser, void * ros_message,
  std::function<void(cycdeser &)> prefix)
{
  assert(ros_message);

  if (prefix) {
    prefix(deser);
  }

  if (members_->member_count_ != 0) {
    TypeSupport::deserializeROSmessage(deser, members_, ros_message, false);
  } else {
    uint8_t dump = 0;
    deser >> dump;
    (void)dump;
  }

  return true;
}

template<typename MembersType>
bool TypeSupport<MembersType>::printROSmessage(
  cycprint & prt,
  std::function<void(cycprint &)> prefix)
{
  if (prefix) {
    prt.print_constant("{");
    prefix(prt);
    prt.print_constant(",");
  }

  if (members_->member_count_ != 0) {
    TypeSupport::printROSmessage(prt, members_);
  } else {
    uint8_t dump = 0;
    prt >> dump;
    (void)dump;
  }

  if (prefix) {
    prt.print_constant("}");
  }

  return true;
}

template<typename MembersType>
std::string TypeSupport<MembersType>::getName()
{
  return name;
}

}  // namespace rmw_cyclonedds_cpp

#endif  // RMW_CYCLONEDDS_CPP__TYPESUPPORT_IMPL_HPP_
