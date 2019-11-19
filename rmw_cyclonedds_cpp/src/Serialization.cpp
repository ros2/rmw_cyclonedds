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

struct SizeAccumulator
{
  size_t _size;
  size_t _capacity;

  explicit SizeAccumulator(size_t capacity = std::numeric_limits<size_t>::max(), size_t size = 0)
  : _size(size), _capacity(capacity)
  {
  }

  auto size() const {return _size;}

  void put_bytes(const void * s, size_t n)
  {
    (void)s;

    if (_size + n > _capacity) {
      throw std::length_error("Data exceeds buffer size");
    }
    _size += n;
    assert(_size <= _capacity);
  }
};

struct DataAccumulator
{
  void * _data;
  size_t _size;
  size_t _capacity;

  DataAccumulator() = delete;
  DataAccumulator(void * data, size_t capacity, size_t size = 0)
  : _data(data), _size(size), _capacity(capacity)
  {
    assert(data);
    assert(size <= capacity);
  }

  auto size() const {return _size;}

  void put_bytes(const void * s, size_t n)
  {
    if (_size + n > _capacity) {
      throw std::length_error("Data exceeds buffer size");
    }

    memcpy(_data + ByteOffset(_size), s, n);
    _size += n;
  }
};

enum class EncodingVersion
{
  Legacy,
  CDR1,
};

template<typename Accumulator>
class CDRWriter
{
protected:
  Accumulator & dst;
  const EncodingVersion eversion = EncodingVersion::Legacy;

  void put_bytes(const void * bytes, size_t size) {dst.put_bytes(bytes, size);}

  void put_bytes(std::initializer_list<char> bs)
  {
    for (char b : bs) {
      dst.put_bytes(&b, 1);
    }
  }

  void align(size_t n_bytes)
  {
    size_t max_align, origin;
    switch (eversion) {
      case (EncodingVersion::Legacy):
        max_align = 8;
        origin = 4;
        break;
      case EncodingVersion::CDR1:
        max_align = 8;
        origin = 0;
        break;
    }
    auto align_to = std::min(max_align, n_bytes);
    size_t current_align = (dst.size() + max_align - origin) % align_to;

    auto bytes_needed = (align_to - current_align) % align_to;
    for (size_t i = 0; i < bytes_needed; i++) {
      auto fill = '\0';
      dst.put_bytes(&fill, 1);
    }
  }

public:
  CDRWriter() = delete;
  explicit CDRWriter(Accumulator & dst)
  : dst(dst) {}

  void serialize_top_level(
    const cdds_request_wrapper_t & request,
    const rosidl_message_type_support_t & support)
  {
    put_rtps_header();
    serialize(request.header.guid);
    serialize(request.header.seq);
    with_typesupport(support, [&](auto t) {
        serialize(make_message_ref(t, request.data));
      });
  }

  void serialize_top_level(const void * data, const rosidl_message_type_support_t & support)
  {
    put_rtps_header();
    with_typesupport(support, [&](auto t) {
        if (t.member_count_ == 0 && eversion == EncodingVersion::Legacy) {
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
      case EncodingVersion::Legacy:
        eversion_byte = 0x00;
        break;
      case EncodingVersion::CDR1:
        eversion_byte = 0x01;
        break;
    }

    put_bytes({
        // encoding version
        eversion_byte,
        // encoding format = PLAIN_CDR
        (native_endian() == endian::little) ? '\1' : '\0',
        // options
        '\0', '\0'});
  }

  template<
    typename T,
    std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value, int> = 0>
  void serialize(T data)
  {
    align(sizeof(data));
    put_bytes(static_cast<const void *>(&data), sizeof(data));
  }

  template<typename T, std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
  void serialize(T data)
  {
    static_assert(std::numeric_limits<T>::is_iec559, "Non-standard float");

    align(sizeof(data));
    put_bytes(static_cast<const void *>(&data), sizeof(data));
  }

  template<
    typename T,
    std::enable_if_t<
      std::is_same<T, bool>::value || std::is_same<T, std::vector<bool>::reference>::value ||
      std::is_same<T, std::vector<bool>::const_reference>::value,
      int> = 0>
  void serialize(T value)
  {
    serialize(value ? '\1' : '\0');
  }

  template<typename T, typename char_type = typename T::traits_type::char_type>
  void serialize(const T & value)
  {
    if (sizeof(char_type) == 1) {
      serialize_u32(value.size() + 1);
      for (char_type s : value) {
        serialize(s);
      }
      serialize('\0');
    } else {
      if (eversion == EncodingVersion::Legacy) {
        serialize_u32(value.size());
        for (char_type s : value) {
          serialize(static_cast<wchar_t>(s));
        }
      } else {
        serialize_u32(value.size() * sizeof(char_type));
        for (char_type s : value) {
          serialize(s);
        }
      }
    }
  }

  template<typename MetaMessage>
  void serialize(const MessageRef<MetaMessage> & message)
  {
    for (size_t i = 0; i < message.size(); i++) {
      auto member = message.at(i);
      switch (member.get_container_type()) {
        case MemberContainerType::SingleValue:
          member.with_single_value([&](auto m) {serialize(m.get());});
          break;
        case MemberContainerType::Array:
          member.with_array([&](auto m) {
              for (size_t j = 0; j < m.size(); j++) {
                serialize(m[j]);
              }
            });
          break;
        case MemberContainerType::Sequence:
          member.with_sequence([&](auto m) {
              serialize_u32(m.size());
              for (size_t j = 0; j < m.size(); j++) {
                serialize(m[j]);
              }
            });
      }
    }
  }
  void serialize_u32(size_t s)
  {
    assert(s < UINT32_MAX);
    serialize(static_cast<uint32_t>(s));
  }
};

void serialize(
  void * dest, size_t dest_size, const void * data, const rosidl_message_type_support_t & ts)
{
  DataAccumulator accum{dest, dest_size};
  CDRWriter<DataAccumulator> writer{accum};
  writer.serialize_top_level(data, ts);
}

size_t get_serialized_size(const void * data, const rosidl_message_type_support_t & ts)
{
  SizeAccumulator accum;
  CDRWriter<SizeAccumulator> writer{accum};
  writer.serialize_top_level(data, ts);
  return accum.size();
}

size_t get_serialized_size(
  const cdds_request_wrapper_t & request, const rosidl_message_type_support_t & ts)
{
  SizeAccumulator accum;
  CDRWriter<SizeAccumulator> writer{accum};
  writer.serialize_top_level(request, ts);
  return accum.size();
}

void serialize(
  void * dest, size_t dest_size, const cdds_request_wrapper_t & request,
  const rosidl_message_type_support_t & ts)
{
  DataAccumulator accum{dest, dest_size};
  CDRWriter<DataAccumulator> writer{accum};
  writer.serialize_top_level(request, ts);
}
}  // namespace rmw_cyclonedds_cpp
