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
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "TypeSupport2.hpp"

namespace rmw_cyclonedds_cpp
{
template<typename T>
struct typeval
{
  using type = T;
};

template<TypeGenerator g, typename UnaryFunction>
void with_type2(ValueType value_type, UnaryFunction f)
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
    case ValueType::STRING:
      return f(typeval<typename TypeGeneratorInfo<g>::String>());
    case ValueType::WSTRING:
      return f(typeval<typename TypeGeneratorInfo<g>::WString>());
    case ValueType::MESSAGE:
    default:
      return f(typeval<void>());
  }
}

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
  using const_ptr_type = const T *;
  using value_type = T;
  using reference_type = T &;

  static size_t sizeof_value() {return sizeof(T);}
  static reference_type cast_value(void * ptr) {return *static_cast<T *>(ptr);}
  static const T & cast_value(const void * ptr) {return *static_cast<const T *>(ptr);}
  static ptr_type cast_ptr(void * ptr) {return static_cast<ptr_type>(ptr);}
  static const_ptr_type cast_ptr(const void * ptr) {return static_cast<const_ptr_type>(ptr);}
};

template<TypeGenerator g>
struct MessageValueHelper
{
  MessageValueHelper(const MetaMessage<g> m)
  : value_members(m) {}

  const MetaMessage<g> value_members;

  using reference_type = MessageRef<g>;
  using value_type = void;

  struct ptr_type
    : public std::iterator<std::random_access_iterator_tag, void, ptrdiff_t, ptr_type,
      MessageRef<g>>
  {
    ptr_type(const MessageValueHelper & helper, void * ptr)
    : helper(helper), ptr(ptr) {}

    const MessageValueHelper & helper;
    void * ptr;

    explicit operator void *() const {return ptr;}

    ptr_type & operator++() {return operator+=(1);}

    auto operator*() const {return helper.cast_value(ptr);}

    ptr_type operator+(ptrdiff_t other) const
    {
      ptr_type new_ptr{helper, ptr};
      new_ptr += other;
      return new_ptr;
    }

    ptrdiff_t operator-(const ptr_type & other) const
    {
      ptrdiff_t n_bytes = (reinterpret_cast<byte *>(ptr) - reinterpret_cast<byte *>(other.ptr));
      assert(n_bytes % helper.sizeof_value() == 0);
      return n_bytes / helper.sizeof_value();
    }

    ptr_type & operator+=(ptrdiff_t other)
    {
      ptr = byte_offset(ptr, other * helper.sizeof_value());
      return *this;
    }

    bool operator==(const ptr_type & other) const {return ptr == other.ptr;}

    bool operator!=(const ptr_type & other) const {return !(*this == other);}
  };

  size_t sizeof_value() const {return value_members.size_of_;}

  reference_type cast_value(void * ptr) const {return reference_type(value_members, ptr);}
  std::add_const_t<reference_type> cast_value(const void * ptr) const
  {
    return make_message_ref(value_members, ptr);
  }

  ptr_type cast_ptr(void * ptr) const {return ptr_type(*this, ptr);}

  std::add_const_t<ptr_type> cast_ptr(const void * ptr) const {return ptr_type(*this, ptr);}
};

template<TypeGenerator g>
// cppcheck-suppress syntaxError
template<typename UnaryFunction, typename Result>
Result MemberRef<g>::with_value_helper(UnaryFunction f)
{
  using tgi = TypeGeneratorInfo<g>;
  auto vt = ValueType(meta_member.type_id_);
  switch (vt) {
    case ValueType::MESSAGE:
      assert(meta_member.members_);
      return f(
        MessageValueHelper<g>{*static_cast<const MetaMessage<g> *>(meta_member.members_->data)});
    case ValueType::STRING:
      return f(NativeValueHelper<typename tgi::String>());
    case ValueType::WSTRING:
      return f(NativeValueHelper<typename tgi::WString>());
    default:
      return with_type(
        vt, [&](auto t) {return f(NativeValueHelper<typename decltype(t)::type>());});
  }
}

template<TypeGenerator g>
template<typename UnaryFunction, typename Result>
Result MemberRef<g>::with_single_value(UnaryFunction f)
{
  assert(get_container_type() == MemberContainerType::SingleValue);
  return with_value_helper([&](auto helper) {return f(helper.cast_value(data));});
}

template<typename T>
class ArrayInterface
{
protected:
  const T * start;
  size_t size;

public:
  ArrayInterface(const T * start, size_t size)
  : start{start}, size{size} {}

  const T * data() {return start;}
  size_t count() {return size;}
};

template<TypeGenerator g>
template<typename UnaryFunction, typename Result>
Result MemberRef<g>::with_array(UnaryFunction f)
{
  assert(get_container_type() == MemberContainerType::Array);
  return with_type2<g>(ValueType(meta_member.type_id_), [&](auto t) {
             using T = typename decltype(t)::type;
             f(ArrayInterface<T>(static_cast<T *>(data), meta_member.array_size_));
           });
}

template<TypeGenerator g, typename ValueHelper>
struct ObjectSequenceMemberRef : MemberRef<g>
{
  ObjectSequenceMemberRef(const ValueHelper h, const MemberRef<g> m)
  : MemberRef<g>(m), value_helper(h)
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

  template<typename Collection>
  struct iterator : public std::iterator<
      std::input_iterator_tag, typename ValueHelper::value_type, std::ptrdiff_t,
      typename ValueHelper::ptr_type, typename ValueHelper::reference_type>
  {
    iterator(const Collection & collection, size_t index)
    : collection(collection), index(index) {}

    Collection & collection;
    size_t index;

    iterator & operator++()
    {
      index++;
      return *this;
    }
    auto operator*() const {return collection[index];}
    bool operator==(const iterator & other) const
    {
      assert(&collection == &other.collection);
      return index == other.index;
    }
    bool operator!=(const iterator & other) const {return !(*this == other);}
  };

  const ValueHelper value_helper;

  auto operator[](size_t index)
  {
    return value_helper.cast_value(this->meta_member.get_function(this->data, index));
  }
  auto operator[](size_t index) const
  {
    return value_helper.cast_value(this->meta_member.get_const_function(this->data, index));
  }

  size_t size() const {return this->meta_member.size_function(this->data);}
  auto begin() const {return iterator<decltype(*this)>(*this, 0);}
  auto end() const {return iterator<decltype(*this)>(*this, size());}
};

template<typename ValueHelper>
struct CSequenceInterface
{
  ROSIDL_GENERATOR_C__PRIMITIVE_SEQUENCE(T, typename ValueHelper::value_type)

  const ValueHelper value_helper;
  size_t upper_bound;
  rosidl_generator_c__T__Sequence & obj;

  using iterator = typename decltype(value_helper)::ptr_type;

  CSequenceInterface(void * ptr, const ValueHelper h, size_t upper_bound)
  : value_helper{h},
    upper_bound(upper_bound),
    obj(*static_cast<rosidl_generator_c__T__Sequence *>(ptr))
  {
    assert(size() == 0 || obj.data);
    assert(size() <= std::numeric_limits<int32_t>::max());
    assert(size() <= upper_bound);
    assert(size() <= obj.capacity);
  }
  auto operator[](size_t index) const
  {
    assert(index < obj->size);
    return obj->data[index];
  }
  auto operator[](size_t index)
  {
    assert(index < obj.size);
    return obj.data[index];
  }
  iterator begin() {return value_helper.cast_ptr(obj.data);}
  iterator end() {return begin() + size();}
  size_t size() const {return obj.size;}
};

template<typename T>
std::vector<T> & cast_vector(void * data, NativeValueHelper<T>)
{
  return *static_cast<std::vector<T> *>(data);
}

template<TypeGenerator g>
[[noreturn]] std::vector<int> & cast_vector(void *, MessageValueHelper<g>)
{
  throw std::runtime_error("Can't make a vector of objects of runtime size.");
}

template<>
template<typename UnaryFunction, typename Result>
Result MemberRef<TypeGenerator::ROSIDL_Cpp>::with_sequence(UnaryFunction f)
{
  assert(get_container_type() == MemberContainerType::Sequence);
  return with_value_helper([&](auto helper) {
             if (this->is_submessage_type() || meta_member.size_function) {
               return f(ObjectSequenceMemberRef<TypeGenerator::ROSIDL_Cpp, decltype(helper)>(helper,
               *this));
             } else {
               return f(cast_vector(data, helper));
             }
           });
}

template<>
template<typename UnaryFunction, typename Result>
Result MemberRef<TypeGenerator::ROSIDL_C>::with_sequence(UnaryFunction f)
{
  assert(get_container_type() == MemberContainerType::Sequence);
  return with_value_helper([&](auto helper) {
             if (this->is_submessage_type() || meta_member.size_function) {
               return f(ObjectSequenceMemberRef<TypeGenerator::ROSIDL_C, decltype(helper)>(helper,
               *this));
             } else {
               size_t upper_bound = meta_member.is_upper_bound_ ? meta_member.array_size_ :
               std::numeric_limits<uint32_t>::max();
               return f(CSequenceInterface<decltype(helper)>(data, helper, upper_bound));
             }
           });
}

}  // namespace rmw_cyclonedds_cpp
#endif  // TYPESUPPORT2_IMPL_HPP_
