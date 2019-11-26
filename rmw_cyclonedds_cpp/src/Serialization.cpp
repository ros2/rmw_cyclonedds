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
#include "Serialization.hpp"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include "TypeSupport2.hpp"
#include "bytewise.hpp"
#include "rmw_cyclonedds_cpp/TypeSupport_impl.hpp"

namespace rmw_cyclonedds_cpp
{
std::pair<rosidl_message_type_support_t, rosidl_message_type_support_t>
get_svc_request_response_typesupports(const rosidl_service_type_support_t & svc)
{
  return with_typesupport(svc, [&](auto svc_ts) {
    return std::make_pair(
      rosidl_message_type_support_t{
        svc.typesupport_identifier,
        svc_ts.request_members_,
        get_message_typesupport_handle_function,
      },
      rosidl_message_type_support_t{
        svc.typesupport_identifier,
        svc_ts.response_members_,
        get_message_typesupport_handle_function,
      });
  });
}

enum class EncodingVersion {
  CDR_Legacy,
  CDR1,
};

template <TypeGenerator g>
class CDRWriter
{
protected:
  void * origin;
  void * cursor;

  const EncodingVersion eversion;
  const size_t max_align;

  void put_bytes(const void * bytes, size_t size)
  {
    std::memcpy(cursor, bytes, size);
    cursor = byte_offset(cursor, size);
  }

  void align(size_t n_bytes)
  {
    assert(n_bytes != 0);
    if (n_bytes == 1) {
      return;
    }
    assert(n_bytes == 1 || n_bytes == 2 || n_bytes == 4 || n_bytes == 8 || n_bytes == 16);
    size_t current_align =
      (eversion == EncodingVersion::CDR_Legacy) ? (position() - 4) : position();

    byte_offset(cursor ,((-current_align) % max_align % n_bytes));
  }

public:
  size_t position() { return static_cast<const char *>(cursor) - static_cast<const char *>(origin); }

  CDRWriter() = delete;
  explicit CDRWriter(void * dst)
  : origin(dst), cursor(dst), eversion{EncodingVersion::CDR_Legacy}, max_align{8}
  {
  }

  void serialize_top_level(const cdds_request_wrapper_t & request, const MetaMessage<g> & support)
  {
    put_rtps_header();
    serialize(request.header.guid);
    serialize(request.header.seq);
    serialize(request.data, support);
  }

  void serialize_top_level(const void * data, const MetaMessage<g> & support)
  {
    put_rtps_header();
    if (support.member_count_ == 0 && eversion == EncodingVersion::CDR_Legacy) {
      serialize('\0');
    } else {
      serialize(make_message_ref(support, data));
    }
  }

  size_t get_serialized_size_top_level(
    const cdds_request_wrapper_t & request, const MetaMessage<g> & support)
  {
    size_t offset = 0;
    offset += 4 + sizeof(request.header.guid) + sizeof(request.header.seq);
    size_t alignment = (eversion == EncodingVersion::CDR_Legacy ? offset - 4 : offset);
    offset += get_serialized_size(make_message_ref(support, request.data), alignment);
    return offset;
  }

  size_t get_serialized_size_top_level(const void * data, const MetaMessage<g> & support)
  {
    size_t offset = 0;
    offset += 4;
    size_t alignment = (eversion == EncodingVersion::CDR_Legacy ? offset - 4 : offset);
    offset += get_serialized_size(make_message_ref(support, data), alignment);
    return offset;
  }

protected:
  void put_rtps_header()
  {
    // beginning of message
    char eversion_byte;
    switch (eversion) {
      case EncodingVersion::CDR_Legacy:
        eversion_byte = '\0';
        break;
      case EncodingVersion::CDR1:
        eversion_byte = '\1';
        break;
    }
    std::array<char, 4> rtps_header{eversion_byte,
                                    // encoding format = PLAIN_CDR
                                    (native_endian() == endian::little) ? '\1' : '\0',
                                    // options
                                    '\0', '\0'};
    put_bytes(rtps_header.data(), rtps_header.size());
  }

  // normalize platform-dependent types before serializing
  template <typename T, std::enable_if_t<std::is_integral<T>::value, int> = 0>
  static auto format_value(T t)
  {
    return t;
  }
  template <>
  static auto format_value(bool t)
  {
    return uint8_t(t);
  }
  template <typename T, std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
  static auto format_value(T t)
  {
    static_assert(std::numeric_limits<T>::is_iec559, "nonstandard float");
    return t;
  }

  template <typename T>
  void serialize_noalign(T value)
  {
    auto v2 = format_value(value);
    put_bytes(&v2, sizeof(v2));
  }

  template <typename T, std::enable_if_t<std::is_arithmetic<T>::value, int> = 0>
  void serialize(T value)
  {
    auto v2 = format_value(value);
    align(sizeof(v2));
    put_bytes(&v2, sizeof(v2));
  }

  void serialize_u32(size_t value)
  {
    assert(value <= std::numeric_limits<uint32_t>::max());
    serialize(uint32_t(value));
  }

  size_t num_alignment_bytes(size_t align_before, size_t sizeof_value)
  {
    if (sizeof_value == 1) return 0;

    size_t align_to = std::min(sizeof_value, max_align);
    assert(align_to == 1 || align_to == 2 || align_to == 4 || align_to == 8);

    if (align_before % align_to == 0) return 0;

    return ((-align_before) % align_to);
  };

  size_t get_primitive_type_size(ValueType vt)
  {
    /// return 0 if the value type is not primitive
    /// else returns the number of bytes it should serialize to
    switch (vt) {
      case ValueType::BOOLEAN:
      case ValueType::OCTET:
      case ValueType::UINT8:
      case ValueType::INT8:
      case ValueType::CHAR:
        return 1;
      case ValueType::UINT16:
      case ValueType::INT16:
      case ValueType::WCHAR:
        return 2;
      case ValueType::UINT32:
      case ValueType::INT32:
      case ValueType::FLOAT:
        return 4;
      case ValueType::UINT64:
      case ValueType::INT64:
      case ValueType::DOUBLE:
        return 8;
      case ValueType::LONG_DOUBLE:
        return 16;
      default:
        return 0;
    }
  }

  template <typename T, std::enable_if_t<std::is_fundamental<T>::value,int > = 0>
  size_t get_serialized_size(T, size_t align_before){
    return num_alignment_bytes(align_before,sizeof(T)) + sizeof(T);
  }

  size_t get_serialized_size(typename TypeGeneratorInfo<g>::String s, size_t align_before)
  {
    size_t cursor = align_before;
    cursor += num_alignment_bytes(align_before, 4);
    cursor += 4;
    cursor += s.size();
    return cursor - align_before;
  }

  size_t get_serialized_size(typename TypeGeneratorInfo<g>::WString s, size_t align_before)
  {
    size_t cursor = align_before;
    cursor += num_alignment_bytes(align_before, 4);
    cursor += 4;
    switch (eversion) {
      case EncodingVersion::CDR_Legacy:
        cursor += s.size() * sizeof(wchar_t);
        break;
      default:
        cursor += s.size() * 2;
    }
    return cursor - align_before;
  }

  /// returns the total serialized size, including needed padding
  /// this must inspect the values, so it can take a while
  size_t get_serialized_size_slow(MemberRef<g> member, size_t align_before)
  {
    size_t cursor = align_before;
    switch (member.get_container_type()) {
      case MemberContainerType::SingleValue:
        member.with_single_value([&](auto v) { cursor += get_serialized_size(v, cursor); });
      case MemberContainerType::Array:
        member.with_array([&](auto v) {
          for (auto x : v) {
            cursor += get_serialized_size(v, cursor);
          }
        });
      case MemberContainerType::Sequence:
        member.with_sequence([&](auto v) {
          cursor += num_alignment_bytes(align_before, 4);
          cursor += 4;
          for (auto x : v) {
            cursor += get_serialized_size(v, cursor);
          }
        });
    }
    return cursor - align_before;
  }

  size_t get_serialized_size(MessageRef<g> message, size_t align_before) {
    size_t cursor = align_before;
    for (auto i=0;i< message.size();i++){
      cursor += get_serialized_size( message.at(i), cursor);
    }
    return cursor - align_before;
  }

  /// return the total serialized size, including needed padding
  /// this tries to take a shortcut if it's a fixed size object
  size_t get_serialized_size(MemberRef<g> member, size_t align_before)
  {
    ValueType vt = ValueType(member.meta_member.type_id_);
    align_before %= max_align;

    size_t value_size = get_primitive_type_size(vt);

    if (value_size == 0) {
      return get_serialized_size_slow(member, align_before);
    }

    switch (member.get_container_type()) {
      case MemberContainerType::SingleValue:
        return num_alignment_bytes(align_before, value_size) + value_size;
      case MemberContainerType::Array:
        return num_alignment_bytes(align_before, value_size) +
               value_size * member.meta_member.array_size_;
      case MemberContainerType::Sequence:
        size_t total_size;
        member.with_sequence([&](auto seq) {
          size_t cursor = align_before;
          cursor += num_alignment_bytes(cursor, 4);
          cursor += 4;
          if (seq.size() > 0) {
            cursor += num_alignment_bytes(cursor, value_size);
            cursor += seq.size() * value_size;
          }
          total_size = cursor - align_before;
        });
        return total_size;
    }
  }

  // specialization for strings
  template <typename T, typename char_type = typename T::traits_type::char_type>
  void serialize(const T & value)
  {
    if (sizeof(char_type) == 1) {
      // count includes trailing null
      serialize_u32(value.size() + 1);
      auto d = value.data();
      for (size_t i = 0; i < value.size(); i++) {
        serialize_noalign(static_cast<char>(d[i]));
      }
      serialize_noalign('\0');
    } else if (eversion == EncodingVersion::CDR_Legacy) {
      // count of characters
      serialize_u32(value.size());
      auto d = value.data();
      for (size_t i = 0; i < value.size(); i++) {
        serialize_noalign(static_cast<wchar_t>(d[i]));
      }
      // no trailing null
    } else {
      // count of *bytes*
      serialize_u32(value.size() * sizeof(char_type));
      auto d = value.data();
      for (size_t i = 0; i < value.size(); i++) {
        serialize_noalign(static_cast<wchar_t>(d[i]));
      }
      // no trailing null
    }
  }

  template <
    typename Iter, typename value_type = typename std::iterator_traits<Iter>::value_type,
    typename format_type = decltype(format_value(std::declval<value_type>()))>
  void serialize(Iter begin, Iter end)
  {
    // Note we do *NOT* align an empty sequence
    if (begin == end) {
      return;
    }
    /// for sequences of primitive values, we can align once and the rest will be aligned
    align(sizeof(format_type));
    for (auto it = begin; it != end; ++it) {
      // note the conversion to value_type so that e.g. std::vector<bool> values
      // get turned into real bools
      serialize_noalign(value_type(*it));
    }
  }

  template <
    typename Iter, typename value_type = typename std::iterator_traits<Iter>::value_type,
    std::enable_if_t<!std::is_arithmetic<value_type>::value, int> = 0>
  void serialize(Iter begin, Iter end)
  {
    /// for sequences of non-primitive values, we need to align for every value
    for (auto it = begin; it != end; ++it) {
      serialize(*it);
    }
  }

  template <typename T, std::enable_if_t<!std::is_void<T>::value, int> = 0>
  void serialize(ArrayInterface<T> value, MetaMember<g>)
  {
    assert(value.count() > 0);
    for (size_t i = 0; i < value.size; i++) {
      serialize(*(value.start + i));
    }
  }

  void serialize(ArrayInterface<void> value, MetaMember<g> m)
  {
    assert(value.count() > 0);
    for (size_t i = 0; i < value.size; i++) {
      auto submessage_ts = cast_typesupport<g>(m.members_);
      serialize(
        make_message_ref(submessage_ts, byte_offset(value.start, i * submessage_ts.size_of_)));
    }
  }

  void serialize(void * data, const MetaMessage<g> & typesupport)
  {
    for (size_t i = 0; i < typesupport.member_count_; i++) {
      auto member_data = byte_offset(data, typesupport.members_[i].offset_);
      //todo...
    }
  }

  void serialize(const MessageRef<g> & message)
  {
    for (size_t i = 0; i < message.size(); i++) {
      auto member = message.at(i);
      switch (member.get_container_type()) {
        case MemberContainerType::SingleValue:
          member.with_single_value([&](auto m) { serialize(m); });
          break;
        case MemberContainerType::Array:
          member.with_array([&](auto m) { serialize(m, member.meta_member); });
          break;
        case MemberContainerType::Sequence:
          member.with_sequence([&](auto m) {
            serialize_u32(m.size());
            serialize(std::begin(m), std::end(m));
          });
      }
    }
  }
};  // namespace rmw_cyclonedds_cpp

void serialize(
  void * dest, size_t dest_size, const void * data, const rosidl_message_type_support_t & ts)
{
  return with_typesupport_info(ts.typesupport_identifier, [&](auto info) {
    auto & mts = *static_cast<const typename decltype(info)::MetaMessage *>(ts.data);
    CDRWriter<info.enum_value> writer{dest};
    writer.serialize_top_level(data, mts);
    assert(writer.position() == dest_size);
  });
}

size_t get_serialized_size(const void * data, const rosidl_message_type_support_t & ts)
{
  size_t n;
  with_typesupport_info(ts.typesupport_identifier, [&](auto info) {
    auto & mts = *static_cast<const typename decltype(info)::MetaMessage *>(ts.data);
    void * dummy = nullptr;
    CDRWriter<info.enum_value> writer(dummy);
    n = writer.get_serialized_size_top_level(data, mts);
  });
  return n;
}

size_t get_serialized_size(
  const cdds_request_wrapper_t & request, const rosidl_message_type_support_t & ts)
{
  size_t n;
  with_typesupport_info(ts.typesupport_identifier, [&](auto info) {
    auto & mts = *static_cast<const typename decltype(info)::MetaMessage *>(ts.data);
    void * dummy = nullptr;
    CDRWriter<info.enum_value> writer(dummy);
    n = writer.get_serialized_size_top_level(request, mts);
  });
  return n;
}

void serialize(
  void * dest, size_t dest_size, const cdds_request_wrapper_t & request,
  const rosidl_message_type_support_t & ts)
{
  return with_typesupport_info(ts.typesupport_identifier, [&](auto info) {
    auto & mts = *static_cast<const typename decltype(info)::MetaMessage *>(ts.data);
    CDRWriter<info.enum_value> writer{dest};
    writer.serialize_top_level(request, mts);
    assert(writer.position() == dest_size);
  });
}
}  // namespace rmw_cyclonedds_cpp
