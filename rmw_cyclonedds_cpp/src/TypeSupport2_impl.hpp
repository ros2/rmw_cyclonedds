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
#ifndef TYPESUPPORT2_IMPL_HPP_
#define TYPESUPPORT2_IMPL_HPP_
#include "TypeSupport2.hpp"

#include <limits>
#include <string>
#include <vector>

namespace rmw_cyclonedds_cpp
{
template<typename T>
struct typeval { using type = T; };

template<typename UnaryFunction, typename Result>
Result with_type(ValueType value_type, UnaryFunction f)
{
  switch (value_type) {
    case ValueType::FLOAT:
      return f(typeval<float>());
    case ValueType::DOUBLE:
      return f(typeval<double>());
    case ValueType::LONG_DOUBLE:
      return f(typeval<long double>());
    case ValueType::WCHAR:
      return f(typeval<char16_t>());
    case ValueType::CHAR:
      return f(typeval<char>());
    case ValueType::BOOLEAN:
      return f(typeval<bool>());
    case ValueType::OCTET:
      return f(typeval<unsigned char>());
    case ValueType::UINT8:
      return f(typeval<uint8_t>());
    case ValueType::INT8:
      return f(typeval<int8_t>());
    case ValueType::UINT16:
      return f(typeval<uint16_t>());
    case ValueType::INT16:
      return f(typeval<int16_t>());
    case ValueType::UINT32:
      return f(typeval<uint32_t>());
    case ValueType::INT32:
      return f(typeval<int32_t>());
    case ValueType::UINT64:
      return f(typeval<uint64_t>());
    case ValueType::INT64:
      return f(typeval<int64_t>());
    default:
      throw std::invalid_argument("not a primitive value");
  }
}

template<typename T>
struct NativeValueHelper
{
  using ptr_type = T *;
  using value_type = T;
  using reference_type = T &;
  static size_t sizeof_value() {return sizeof(T);}
  static reference_type cast_value(void * ptr) {return *static_cast<T *>(ptr);}
  static std::add_const_t<reference_type> cast_value(const void * ptr)
  {
    return *static_cast<const T *>(ptr);
  }
};

template<typename MetaMessage>
struct MessageValueHelper
{
  static_assert(std::is_same<MetaMessage, RTI_C::MetaMessage>::value ||
    std::is_same<MetaMessage, RTI_Cpp::MetaMessage>::value,
    "oops");
  const MetaMessage value_members;
  using reference_type = MessageRef<MetaMessage>;
  size_t sizeof_value() const {return value_members.size_of_;}
  reference_type cast_value(void * ptr) const
  {
    return make_message_ref(value_members, ptr);
  }
  std::add_const_t<reference_type> cast_value(const void * ptr) const
  {
    return make_message_ref(value_members, ptr);
  }
};

template<>
template<typename UnaryFunction, typename Result>
Result MemberRef<RTI_Cpp::MetaMember>::with_value_helper(UnaryFunction f)
{
  auto vt = ValueType(meta_member.type_id_);
  switch (vt) {
    case ValueType::MESSAGE:
      assert(meta_member.members_);
      return with_typesupport(*meta_member.members_, [&](auto submessage_members) {
                 return f(
                   MessageValueHelper<decltype(submessage_members)>{submessage_members});
               });
    case ValueType::STRING:
      return f(NativeValueHelper<std::string>());
    case ValueType::WSTRING:
      return f(NativeValueHelper<std::u16string>());
    default:
      return with_type(vt, [&](auto t) {
                 return f(NativeValueHelper<typename decltype(t)::type>());
               });
  }
}

template<>
template<typename UnaryFunction, typename Result>
Result MemberRef<RTI_C::MetaMember>::with_value_helper(UnaryFunction f)
{
  auto vt = ValueType(meta_member.type_id_);
  switch (vt) {
    case ValueType::MESSAGE:
      assert(meta_member.members_);
      return with_typesupport(*meta_member.members_, [&](auto submessage_members) {
                 return f(
                   MessageValueHelper<decltype(submessage_members)>{submessage_members});
               });
    case ValueType::STRING:
      return f(NativeValueHelper<RTI_C::String>());
    case ValueType::WSTRING:
      return f(NativeValueHelper<RTI_C::WString>());
    default:
      return with_type(vt, [&](auto t) {
                 return f(NativeValueHelper<typename decltype(t)::type>());
               });
  }
}
template<typename MetaMember, typename ValueHelper>
struct SingleValueMemberRef : MemberRef<MetaMember>
{
  SingleValueMemberRef(ValueHelper h, MemberRef<MetaMember> m)
  : MemberRef<MetaMember>{m}, value_helper{h} {}
  ValueHelper value_helper;
  auto get() {return value_helper.cast_value(this->data);}
  auto get() const {return value_helper.cast_value(this->data);}
};

template<typename MetaMember>
template<typename UnaryFunction, typename Result>
Result MemberRef<MetaMember>::with_single_value(UnaryFunction f)
{
  assert(get_container_type() == MemberContainerType::SingleValue);
  return with_value_helper([&](auto helper) {
             return f(SingleValueMemberRef<MetaMember, decltype(helper)>{helper, *this});
           });
}

template<typename MetaMember, typename ValueHelper>
struct ArrayMemberRef : MemberRef<MetaMember>
{
  ArrayMemberRef(ValueHelper h, MemberRef<MetaMember> m)
  : MemberRef<MetaMember>{m}, value_helper{h} {}
  ValueHelper value_helper;
  auto operator[](size_t index) const
  {
    return value_helper.cast_value(this->data + ByteOffset(value_helper.sizeof_value() * index));
  }
  size_t size() const {return this->meta_member.array_size_;}
};

template<typename MetaMember>
template<typename UnaryFunction, typename Result>
Result MemberRef<MetaMember>::with_array(UnaryFunction f)
{
  assert(get_container_type() == MemberContainerType::Array);
  return with_value_helper([&](auto helper) {
             return f(ArrayMemberRef<MetaMember, decltype(helper)>(helper, *this));
           });
}

template<typename MetaMember, typename ValueHelper>
struct ObjectSequenceMemberRef : MemberRef<MetaMember>
{
  ObjectSequenceMemberRef(const ValueHelper h, const MemberRef<MetaMember> m)
  : MemberRef<MetaMember>(m), value_helper(h)
  {
    assert(this->meta_member.get_function);
    assert(this->meta_member.get_const_function);
    assert(this->meta_member.size_function);
    assert(this->meta_member.resize_function);
    assert(size() <= std::numeric_limits<int32_t>::max());
    if (this->meta_member.is_upper_bound_) {
      assert(size() <= this->meta_member.array_size_);
    }
  }

  const ValueHelper value_helper;

  auto operator[](size_t index)
  {
    return value_helper.cast_value(
      this->meta_member.get_function(this->data, index));
  }
  auto operator[](size_t index) const
  {
    return value_helper.cast_value(
      this->meta_member.get_const_function(this->data, index));
  }
  size_t size() const {return this->meta_member.size_function(this->data);}
};

template<typename MetaMember, typename ValueHelper>
struct SequenceMemberRef;

template<typename T>
struct SequenceMemberRef<RTI_Cpp::MetaMember, NativeValueHelper<T>>
  : MemberRef<RTI_Cpp::MetaMember>
{
  const NativeValueHelper<T> value_helper;

  auto typed_data() const
  {
    auto ptr = static_cast<const std::vector<T> *>(this->data);
    assert(ptr);
    return ptr;
  }
  auto typed_data()
  {
    auto ptr = static_cast<std::vector<T> *>(this->data);
    assert(ptr);
    return ptr;
  }

  SequenceMemberRef(
    const NativeValueHelper<T> h,
    const MemberRef<RTI_Cpp::MetaMember> m)
  : MemberRef<RTI_Cpp::MetaMember>(m), value_helper(h)
  {
    assert(!meta_member.get_function);
    assert(!meta_member.get_const_function);
    assert(!meta_member.resize_function);
    assert(!meta_member.size_function);
    assert(size() <= std::numeric_limits<int32_t>::max());
    if (this->meta_member.is_upper_bound_) {
      assert(size() <= this->meta_member.array_size_);
    }
  }
  auto operator[](size_t index) const
  {
    assert(index < size());
    return typed_data()->at(index);
  }
  size_t size() const {return typed_data()->size();}
};

template<typename MetaMember, typename MetaMessage>
struct SequenceMemberRef<MetaMember, MessageValueHelper<MetaMessage>>
  : MemberRef<MetaMember>
{
protected:
  [[noreturn]] void throw_not_implemented() const
  {
    throw std::runtime_error(
            "Non-object sequences of message type not implemented");
  }

public:
  SequenceMemberRef(
    const MessageValueHelper<MetaMessage>,
    const MemberRef<MetaMember> m)
  : MemberRef<MetaMember>(m)
  {
    throw_not_implemented();
  }
  MessageRef<MetaMessage> operator[](size_t) const {throw_not_implemented();}
  size_t size() const {throw_not_implemented();}
};

template<typename T>
struct SequenceMemberRef<RTI_C::MetaMember, NativeValueHelper<T>>
  : MemberRef<RTI_C::MetaMember>
{
  ROSIDL_GENERATOR_C__PRIMITIVE_SEQUENCE(T, T)

  const NativeValueHelper<T> value_helper;

  auto typed_data() const
  {
    return static_cast<const rosidl_generator_c__T__Sequence *>(this->data);
  }
  auto typed_data()
  {
    return static_cast<rosidl_generator_c__T__Sequence *>(this->data);
  }
  SequenceMemberRef(
    const NativeValueHelper<T> h,
    const MemberRef<RTI_C::MetaMember> m)
  : MemberRef<RTI_C::MetaMember>{m}, value_helper{h}
  {
    assert(!meta_member.get_function);
    assert(!meta_member.get_const_function);
    assert(!meta_member.resize_function);
    assert(!meta_member.size_function);

    assert(size() == 0 || typed_data()->data);
    assert(size() <= std::numeric_limits<int32_t>::max());
    if (this->meta_member.is_upper_bound_) {
      assert(size() <= this->meta_member.array_size_);
    }
    assert(typed_data()->size <= typed_data()->capacity);
  }
  auto operator[](size_t index) const
  {
    assert(index < typed_data()->size);
    return typed_data()->data[index];
  }
  auto operator[](size_t index)
  {
    assert(index < typed_data()->size);
    return typed_data()->data[index];
  }
  size_t size() const {return typed_data()->size;}
};

template<typename MetaMember>
template<typename UnaryFunction, typename Result>
Result MemberRef<MetaMember>::with_sequence(UnaryFunction f)
{
  assert(get_container_type() == MemberContainerType::Sequence);
  return with_value_helper([&](auto helper) {
             if (meta_member.size_function) {
               return f(
                 ObjectSequenceMemberRef<MetaMember, decltype(helper)>(helper, *this));
             } else {
               return f(SequenceMemberRef<MetaMember, decltype(helper)>(helper, *this));
             }
           });
}

}  // namespace rmw_cyclonedds_cpp
#endif  // TYPESUPPORT2_IMPL_HPP_
