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

struct ByteCountingIterator : public std::iterator<std::output_iterator_tag, void, void, void, void>
{
  size_t n;

  ByteCountingIterator()
  : n(0) {}

  ByteCountingIterator & operator++()
  {
    n++;
    return *this;
  }

  ByteCountingIterator & operator+=(const size_t & s)
  {
    n += s;
    return *this;
  }
  bool operator==(const ByteCountingIterator & other) const {return n == other.n;}

  bool operator!=(const ByteCountingIterator & other) const {return n != other.n;}

  ptrdiff_t operator-(const ByteCountingIterator & other) const {return n - other.n;}

  struct DummyByteRef
  {
    template<typename T>
    DummyByteRef & operator=(T &&)
    {
      static_assert(sizeof(T) == 1, "assignment value is too big");
      return *this;
    }
  };

  DummyByteRef operator*() {return {};}
};

enum class EncodingVersion
{
  CDR_Legacy,
  CDR1,
};

template<typename OutputIterator>
class CDRWriter
{
protected:
  OutputIterator origin;
  OutputIterator cursor;

  const EncodingVersion eversion;
  const size_t max_align;

  void put_bytes(const void * bytes, size_t size)
  {
    cursor = std::copy(
      reinterpret_cast<const byte *>(bytes), reinterpret_cast<const byte *>(bytes) + size, cursor);
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

    cursor += ((-current_align) % max_align % n_bytes);
  }

public:
  size_t position() {return cursor - origin;}

  CDRWriter() = delete;
  explicit CDRWriter(OutputIterator dst)
  : origin(dst), cursor(dst), eversion{EncodingVersion::CDR_Legacy}, max_align{8}
  {
  }

  void serialize_top_level(
    const cdds_request_wrapper_t & request, const rosidl_message_type_support_t & support)
  {
    put_rtps_header();
    serialize(request.header.guid);
    serialize(request.header.seq);
    with_typesupport(support, [&](auto t) {serialize(make_message_ref(t, request.data));});
  }

  void serialize_top_level(const void * data, const rosidl_message_type_support_t & support)
  {
    put_rtps_header();
    with_typesupport(support, [&](auto t) {
        if (t.member_count_ == 0 && eversion == EncodingVersion::CDR_Legacy) {
          serialize('\0');
        } else {
          serialize(make_message_ref(t, data));
        }
      });
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
  template<typename T,
    std::enable_if_t<std::is_integral<T>::value, int> = 0>
  static auto format_value(T t)
  {
    return t;
  }
  template<>
  static auto format_value(bool t)
  {
    return uint8_t(t);
  }
  template<typename T, std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
  static auto format_value(T t)
  {
    static_assert(std::numeric_limits<T>::is_iec559, "nonstandard float");
    return t;
  }

  template<typename T>
  void serialize_noalign(T value)
  {
    auto v2 = format_value(value);
    put_bytes(&v2, sizeof(v2));
  }

  template<typename T,
    std::enable_if_t<std::is_arithmetic<T>::value, int> = 0>
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

  // specialization for strings
  template<typename T, typename char_type = typename T::traits_type::char_type>
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

  template<typename Iter,
    typename value_type = typename std::iterator_traits<Iter>::value_type,
    typename format_type = decltype(format_value(std::declval<value_type>()))
  >
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

  template<typename Iter,
    typename value_type = typename std::iterator_traits<Iter>::value_type,
    std::enable_if_t<!std::is_arithmetic<value_type>::value, int> = 0
  >
  void serialize(Iter begin, Iter end)
  {
    /// for sequences of non-primitive values, we need to align for every value
    for (auto it = begin; it != end; ++it) {
      serialize(*it);
    }
  }

  template<TypeGenerator g>
  void serialize(const MessageRef<g> & message)
  {
    for (size_t i = 0; i < message.size(); i++) {
      auto member = message.at(i);
      switch (member.get_container_type()) {
        case MemberContainerType::SingleValue:
          member.with_single_value([&](auto m) {serialize(m);});
          break;
        case MemberContainerType::Array:
          member.with_array([&](auto m) {
              serialize(std::begin(m), std::end(m));
            });
          break;
        case MemberContainerType::Sequence:
          member.with_sequence([&](auto m) {
              serialize_u32(m.size());
              serialize(std::begin(m), std::end(m));
            });
      }
    }
  }
};

void serialize(
  void * dest, size_t dest_size, const void * data, const rosidl_message_type_support_t & ts)
{
  CDRWriter<byte *> writer{static_cast<byte *>(dest)};
  writer.serialize_top_level(data, ts);
  assert(writer.position() == dest_size);
}

size_t get_serialized_size(const void * data, const rosidl_message_type_support_t & ts)
{
  ByteCountingIterator it;
  CDRWriter<ByteCountingIterator> writer(it);
  writer.serialize_top_level(data, ts);
  return writer.position();
}

size_t get_serialized_size(
  const cdds_request_wrapper_t & request, const rosidl_message_type_support_t & ts)
{
  ByteCountingIterator it;
  CDRWriter<ByteCountingIterator> writer(it);
  writer.serialize_top_level(request, ts);
  return writer.position();
}

void serialize(
  void * dest, size_t dest_size, const cdds_request_wrapper_t & request,
  const rosidl_message_type_support_t & ts)
{
  CDRWriter<byte *> writer{static_cast<byte *>(dest)};
  writer.serialize_top_level(request, ts);
  assert(writer.position() == dest_size);
}
}  // namespace rmw_cyclonedds_cpp
