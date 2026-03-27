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

// suppress definition of min/max macros on Windows.
// TODO(dan@digilabs.io): Move this closer to where Windows.h/Windef.h is included
#ifndef NOMINMAX
#define NOMINMAX

#include <dds/ddsi/ddsi_xqos.h>
#endif

#include "Serialization.hpp"

#include <array>
#include <cassert>
#include <cstring>
#include <string>
#include <limits>
#include <memory>
#include <iostream>
#include <sstream>
#include <iterator>
#include <unordered_map>
#include <utility>
#include <vector>

#include "TypeSupport2.hpp"
#include "trivsercache.hpp"
#include "bytewise.hpp"

namespace rmw_cyclonedds_cpp
{
template<typename Type>
static void bswap(Type *) // NOLINT
{
  static_assert("Byteswap for type not implementd");
}
template<> void bswap<uint16_t>(uint16_t * u)
{
  *u = static_cast<uint16_t>((*u >> 8) | (*u << 8));
}
template<> void bswap<uint32_t>(uint32_t * u)
{
  *u = ((*u >> 24) |
    ((*u & 0x00ff0000) >> 8) |
    ((*u & 0x0000ff00) << 8) |
    (*u << 24));
}
template<> void bswap<uint64_t>(uint64_t * u)
{
  *u = ((*u >> 56) |
    ((*u & 0x00ff000000000000) >> 40) |
    ((*u & 0x0000ff0000000000) >> 24) |
    ((*u & 0x000000ff00000000) >> 8) |
    ((*u & 0x00000000ff000000) << 8) |
    ((*u & 0x0000000000ff0000) << 24) |
    ((*u & 0x000000000000ff00) << 40) |
    (*u << 56));
}


template<size_t sz>
static void bswapN(void *);
template<> void bswapN<1>(void *) {}
template<> void bswapN<2>(void * x)
{
  uint16_t tmp;
  memcpy(&tmp, x, sizeof(tmp));
  bswap(&tmp);
  memcpy(x, &tmp, sizeof(tmp));
}
template<> void bswapN<4>(void * x)
{
  uint32_t tmp;
  memcpy(&tmp, x, sizeof(tmp));
  bswap(&tmp);
  memcpy(x, &tmp, sizeof(tmp));
}
template<> void bswapN<8>(void * x)
{
  uint64_t tmp;
  memcpy(&tmp, x, sizeof(tmp));
  bswap(&tmp);
  memcpy(x, &tmp, sizeof(tmp));
}

template<typename Derived>
struct CursorBase
{
  void align(size_t n_bytes)
  {
    assert(n_bytes > 0);
    size_t start_offset = static_cast<Derived *>(this)->offset();
    if (n_bytes == 1 || start_offset % n_bytes == 0) {
      return;
    }
    static_cast<Derived *>(this)->advance(n_bytes - start_offset % n_bytes);
    assert(static_cast<Derived *>(this)->offset() - start_offset < n_bytes);
    assert(static_cast<Derived *>(this)->offset() % n_bytes == 0);
  }

  ptrdiff_t operator-(const CursorBase & other) const
  {
    return static_cast<ptrdiff_t>(static_cast<Derived *>(this)->offset()) -
           static_cast<ptrdiff_t>(static_cast<Derived &>(other).offset());
  }
};

template<typename Derived>
struct WriteCursorBase : public CursorBase<Derived>
{
};

struct SizeCursor final : public WriteCursorBase<SizeCursor>
{
  SizeCursor()
  : SizeCursor(0) {}
  explicit SizeCursor(size_t initial_offset)
  : m_offset(initial_offset) {}

  size_t m_offset;
  size_t offset() const {return m_offset;}
  void advance(size_t n_bytes) {m_offset += n_bytes;}
  void * put_bytes(const void *, size_t n_bytes) {advance(n_bytes); return nullptr;}
  bool ignores_data() const {return true;}
  void rebase(ptrdiff_t relative_origin)
  {
    // we're moving the *origin* so this has to change in the *opposite* direction
    m_offset -= relative_origin;
  }
};

struct CheckingSizeCursor final : public WriteCursorBase<CheckingSizeCursor>
{
  CheckingSizeCursor()
  : CheckingSizeCursor(0) {}
  explicit CheckingSizeCursor(size_t initial_offset)
  : m_offset(initial_offset) {}

  size_t m_offset;
  size_t offset() const {return m_offset;}
  void advance(size_t n_bytes)
  {
    if (UINT32_MAX - m_offset < n_bytes) {
      m_offset = UINT32_MAX;
      throw std::overflow_error("oversize CDR");
    } else {
      m_offset += n_bytes;
    }
  }
  void * put_bytes(const void *, size_t n_bytes) {advance(n_bytes); return nullptr;}
  bool ignores_data() const {return true;}
  void rebase(ptrdiff_t relative_origin)
  {
    // we're moving the *origin* so this has to change in the *opposite* direction
    m_offset -= relative_origin;
  }
};

struct SerializeCursor final : public WriteCursorBase<SerializeCursor>
{
  const void * origin;
  void * position;

  explicit SerializeCursor(void * position)
  : origin(position), position(position) {}

  size_t offset() const {return (const byte *)position - (const byte *)origin;}
  void advance(size_t n_bytes)
  {
    std::memset(position, '\0', n_bytes);
    position = byte_offset(position, n_bytes);
  }
  void * put_bytes(const void * bytes, size_t n_bytes)
  {
    auto old_position = position;
    if (n_bytes > 0) {
      std::memcpy(position, bytes, n_bytes);
      position = byte_offset(position, n_bytes);
    }
    return old_position;
  }
  bool ignores_data() const {return false;}
  void rebase(ptrdiff_t relative_origin) {origin = byte_offset(origin, relative_origin);}
};

struct ByteVectorCursor final : public WriteCursorBase<ByteVectorCursor>
{
  size_t pos_;
  std::vector<byte> & data_;

  explicit ByteVectorCursor(std::vector<byte> & data)
  : pos_{0}, data_{data}
  {
  }

  size_t offset() const {return pos_;}
  void advance(size_t n_bytes)
  {
    byte zero = static_cast<byte>(0);
    data_.insert(data_.end(), n_bytes, zero);
    pos_ += n_bytes;
  }
  void * put_bytes(const void * bytes, size_t n_bytes)
  {
    if (n_bytes == 0) {
      return nullptr;
    } else {
      auto ucbytes = static_cast<const byte *>(bytes);
      data_.insert(data_.end(), ucbytes, ucbytes + n_bytes);
      pos_ += n_bytes;
      return data_.data() + data_.size() - n_bytes;
    }
  }
  bool ignores_data() const
  {
    return false;
  }
  void rebase(ptrdiff_t relative_origin)
  {
    assert(relative_origin < 0 || pos_ >= static_cast<size_t>(relative_origin));
    pos_ -= relative_origin;
  }
};

enum class EncodingVersion
{
  XCDR1,
};

enum class MinOrMax
{
  Min,
  Max
};

class CDRWriter final : public BaseCDRWriter
{
public:
  const EncodingVersion eversion;
  const StructValueType * m_root_value_type;
  const TriviallySerializedCache tsc;
  const SampleOrRequest m_variant;
  const size_t min_serialized_data_size;  // Includes 4 bytes encoding header
  const size_t min_serialized_key_size;   // Includes 4 bytes encoding header
  const size_t max_serialized_data_size;  // Includes 4 bytes encoding header; SIZE_MAX if unbounded
  const size_t max_serialized_key_size;   // Includes 4 bytes encoding header; SIZE_MAX if unbounded

public:
  explicit CDRWriter(const StructValueType * root_value_type, SampleOrRequest variant)
  : eversion{EncodingVersion::XCDR1},
    m_root_value_type{root_value_type},
    tsc{root_value_type},
    m_variant{variant},
    min_serialized_data_size{compute_serialized_size_bound(SampleOrKey::Sample, MinOrMax::Min)},
    min_serialized_key_size{compute_serialized_size_bound(SampleOrKey::Key, MinOrMax::Min)},
    max_serialized_data_size{compute_serialized_size_bound(SampleOrKey::Sample, MinOrMax::Max)},
    max_serialized_key_size{compute_serialized_size_bound(SampleOrKey::Key, MinOrMax::Max)}
  {
    assert(m_root_value_type);
  }

  size_t get_serialized_size(const void * src, SampleOrKey what) const override
  {
    const size_t min = get_min_serialized_size(what);
    const size_t max = get_max_serialized_size(what);
    if (min == max) {
      return max;
    } else {
      SizeCursor cursor;
      serialize_top_level(cursor, src, what);
      return cursor.offset();
    }
  }

  size_t get_serialized_size_estimate(const void * src, SampleOrKey what) const override
  {
    const size_t min = get_min_serialized_size(what);
    const size_t max = get_max_serialized_size(what);
    if (max <= 1024 || (min > 0 && max / min == 1)) {
      // if always less than 1kB, max < 2*min: simply use max
      return max;
    } else {
      SizeCursor cursor;
      serialize_top_level(cursor, src, what);
      return cursor.offset();
    }
  }

  size_t get_min_serialized_size(SampleOrKey what) const override
  {
    return (what == SampleOrKey::Sample) ? min_serialized_data_size : min_serialized_key_size;
  }

  size_t get_max_serialized_size(SampleOrKey what) const override
  {
    return (what == SampleOrKey::Sample) ? max_serialized_data_size : max_serialized_key_size;
  }

  void serialize(void * dst, const void * src, SampleOrKey what) const override
  {
    SerializeCursor cursor(dst);
    serialize_top_level(cursor, src, what);
  }

protected:
  size_t compute_serialized_size_bound(SampleOrKey what, MinOrMax mm)
  {
    CheckingSizeCursor cursor;
    bool ok = true;
    try {
      ok = serialize_size_bound_top_level(cursor, what, mm);
    } catch (const std::overflow_error &) {
      ok = false;
    }
    if (!ok) {
      return SIZE_MAX;
    } else {
      return cursor.offset();
    }
  }

  template<typename WriteCursor>
  void serialize_top_level(WriteCursor & dst, const void * src, SampleOrKey what) const
  {
    put_rtps_header(dst);
    dst.rebase(+4);
    if (what == SampleOrKey::Sample && m_variant == SampleOrRequest::Request) {
      auto const request = static_cast<const cdds_request_wrapper_t *>(src);
      dst.put_bytes(&request->header.guid, sizeof(request->header.guid));
      dst.put_bytes(&request->header.seq, sizeof(request->header.seq));
      src = request->data;
    }
    if (what == SampleOrKey::Sample || m_root_value_type->has_keys()) {
      serialize(dst, src, m_root_value_type, what);
    }
    dst.rebase(-4);
  }

  template<typename WriteCursor>
  bool serialize_size_bound_top_level(WriteCursor & dst, SampleOrKey what, MinOrMax mm) const
  {
    assert(dst.ignores_data());
    put_rtps_header(dst);
    dst.rebase(+4);
    if (what == SampleOrKey::Sample && m_variant == SampleOrRequest::Request) {
      dst.put_bytes(nullptr, 8);
      dst.put_bytes(nullptr, 8);
    }
    bool fixed = true;
    if (what == SampleOrKey::Sample || m_root_value_type->has_keys()) {
      fixed = serialize_size_bound(dst, m_root_value_type, what, mm);
    }
    dst.rebase(-4);
    return fixed;
  }

  template<typename WriteCursor>
  void put_rtps_header(WriteCursor & dst) const
  {
    unsigned char encoding[2];
    switch (eversion) {
      case EncodingVersion::XCDR1:
        encoding[0] = 0;
        encoding[1] = (native_endian() == endian::little) ? 1 : 0;
        break;
    }
    std::array<unsigned char, 4> rtps_header{{encoding[0], encoding[1], 0, 0}};
    dst.put_bytes(rtps_header.data(), rtps_header.size());
  }

  template<typename WriteCursor>
  void serialize_u32(WriteCursor & dst, size_t value) const
  {
    assert(value <= std::numeric_limits<uint32_t>::max());
    auto u32_value = static_cast<uint32_t>(value);
    dst.align(4);
    dst.put_bytes(&u32_value, 4);
  }

  template<typename WriteCursor>
  void serialize(
    WriteCursor & dst, const void * src, const PrimitiveValueType & value_type,
    SampleOrKey) const
  {
    dst.align(value_type.cdralignof_type());
    const size_t n_bytes = value_type.cdrsizeof_type();

    switch (value_type.type_kind()) {
      case ROSIDL_TypeKind::FLOAT:
        assert(std::numeric_limits<float>::is_iec559);
        dst.put_bytes(src, n_bytes);
        return;
      case ROSIDL_TypeKind::DOUBLE:
        assert(std::numeric_limits<double>::is_iec559);
        dst.put_bytes(src, n_bytes);
        return;
      case ROSIDL_TypeKind::CHAR:
      case ROSIDL_TypeKind::WCHAR:
      case ROSIDL_TypeKind::BOOLEAN:
      case ROSIDL_TypeKind::OCTET:
      case ROSIDL_TypeKind::UINT8:
      case ROSIDL_TypeKind::INT8:
      case ROSIDL_TypeKind::UINT16:
      case ROSIDL_TypeKind::INT16:
      case ROSIDL_TypeKind::UINT32:
      case ROSIDL_TypeKind::INT32:
      case ROSIDL_TypeKind::UINT64:
      case ROSIDL_TypeKind::INT64:
        if (value_type.sizeof_type() == n_bytes || native_endian() == endian::little) {
          dst.put_bytes(src, n_bytes);
        } else {
          const void * offset_src = byte_offset(src, value_type.sizeof_type() - n_bytes);
          dst.put_bytes(offset_src, n_bytes);
        }
        return;
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
      default:
        unreachable();
    }
  }

  template<typename WriteCursor>
  void serialize(
    WriteCursor & dst, const void * src, const U8StringValueType & value_type,
    SampleOrKey) const
  {
    auto str = value_type.data(src);
    serialize_u32(dst, str.size() + 1);
    dst.put_bytes(str.data(), str.size());
    char terminator = '\0';
    dst.put_bytes(&terminator, 1);
  }

  template<typename WriteCursor>
  void serialize(
    WriteCursor & dst, const void * src, const U16StringValueType & value_type,
    SampleOrKey) const
  {
    auto str = value_type.data(src);
    serialize_u32(dst, str.size_bytes());
    if (dst.ignores_data()) {
      dst.advance(str.size_bytes());
    } else {
      dst.put_bytes(str.data(), str.size_bytes());
    }
  }

  template<typename WriteCursor>
  void serialize_many(
    WriteCursor & dst, const void * src, size_t count,
    const AnyValueType * vt, SampleOrKey what) const
  {
    // nothing to do; not even alignment
    if (count == 0) {
      return;
    }

    // Serialize the first element.
    serialize(dst, src, vt, what);

    // If the value type is primitive, we are now aligned.
    // It might be that the first element is not trivially serialized but the rest are;
    // e.g. if any element in a struct has CDR alignment more stringent than the first element.

    src = byte_offset(src, vt->sizeof_type());
    --count;
    if (count == 0) {
      return;
    }

    if (tsc.lookup_many_trivially_serialized(dst.offset(), vt)) {
      size_t value_size = vt->sizeof_type();
      dst.put_bytes(src, count * value_size);
      return;
    } else {
      for (size_t i = 0; i < count; i++) {
        auto element = byte_offset(src, i * vt->sizeof_type());
        serialize(dst, element, vt, what);
      }
    }
  }

  template<typename WriteCursor>
  void serialize(
    WriteCursor & dst, const void * src, const ArrayValueType & value_type,
    SampleOrKey what) const
  {
    serialize_many(
      dst, value_type.get_data(src), value_type.array_size(), value_type.element_value_type(),
      what);
  }

  template<typename WriteCursor>
  void serialize(
    WriteCursor & dst, const void * src,
    const SpanSequenceValueType & value_type, SampleOrKey what) const
  {
    size_t count = value_type.sequence_size(src);
    serialize_u32(dst, count);
    serialize_many(
      dst, value_type.sequence_contents(src), count, value_type.element_value_type(), what);
  }

  template<typename WriteCursor>
  void serialize(
    WriteCursor & dst, const void * src,
    const BoolVectorValueType & value_type, SampleOrKey) const
  {
    size_t count = value_type.size(src);
    serialize_u32(dst, count);
    if (dst.ignores_data()) {
      dst.advance(count);
    } else {
      for (auto iter = value_type.begin(src); iter != value_type.end(src); ++iter) {
        bool b = *iter;
        dst.put_bytes(&b, 1);
      }
    }
  }

  template<typename WriteCursor>
  void serialize(
    WriteCursor & dst, const void * src, const AnyValueType * value_type,
    SampleOrKey what) const
  {
    if (what == SampleOrKey::Sample && tsc.lookup_trivially_serialized(dst.offset(), value_type)) {
      dst.put_bytes(src, value_type->sizeof_type());
    } else {
      value_type->apply([&](const auto & vt) {return serialize(dst, src, vt, what);});
    }
  }

  template<typename WriteCursor>
  void serialize(
    WriteCursor & dst, const void * struct_src,
    const StructValueType & struct_info,
    SampleOrKey what) const
  {
    bool all_fields = (what != SampleOrKey::Key || !struct_info.has_keys());
    for (size_t i = 0; i < struct_info.n_members(); i++) {
      auto member_info = struct_info.get_member(i);
      if (all_fields || member_info->is_key) {
        auto value_type = member_info->value_type;
        auto member_src = byte_offset(struct_src, member_info->member_offset);
        serialize(dst, member_src, value_type, what);
      }
    }
  }

  template<typename WriteCursor>
  bool serialize_size_bound(
    WriteCursor & dst, const PrimitiveValueType & value_type, SampleOrKey,
    MinOrMax) const
  {
    dst.align(value_type.cdralignof_type());
    dst.put_bytes(nullptr, value_type.cdrsizeof_type());
    return true;
  }

  template<typename WriteCursor>
  bool serialize_size_bound(
    WriteCursor & dst, const U8StringValueType & value_type, SampleOrKey,
    MinOrMax mm) const
  {
    const uint32_t bound = (mm == MinOrMax::Min) ? 0 : value_type.string_bound();
    serialize_u32(dst, bound);
    dst.put_bytes(nullptr, bound);
    dst.put_bytes(nullptr, 1);
    return true;
  }

  template<typename WriteCursor>
  bool serialize_size_bound(
    WriteCursor & dst, const U16StringValueType & value_type, SampleOrKey,
    MinOrMax mm) const
  {
    const uint32_t bound = (mm == MinOrMax::Min) ? 0 : value_type.string_bound();
    serialize_u32(dst, bound);
    dst.put_bytes(nullptr, 2 * bound);
    return true;
  }

  template<typename WriteCursor>
  bool serialize_size_bound_many(
    WriteCursor & dst, size_t count,
    const AnyValueType * vt, SampleOrKey what, MinOrMax mm) const
  {
    if (count == 0) {
      return true;
    }
    if (count >= UINT32_MAX) {
      return false;
    }
    // 1st element padding varies
    if (!serialize_size_bound(dst, vt, what, mm)) {
      return false;
    }
    if (count > 1) {
      // 2nd and further elements: padding is the same if types are fixed size
      // non-fixed size types have bounds that cause it to bail out early
      const size_t offset = dst.offset();
      if (!serialize_size_bound(dst, vt, what, mm)) {
        return false;
      }
      const size_t elt_size = dst.offset() - offset;
      const size_t max_count = SIZE_MAX / elt_size;
      if (count > max_count) {
        return false;
      }
      dst.put_bytes(nullptr, count * elt_size);
    }
    return true;
  }

  template<typename WriteCursor>
  bool serialize_size_bound(
    WriteCursor & dst, const ArrayValueType & value_type, SampleOrKey what,
    MinOrMax mm) const
  {
    return serialize_size_bound_many(
      dst, value_type.array_size(), value_type.element_value_type(), what, mm);
  }

  template<typename WriteCursor>
  bool serialize_size_bound(
    WriteCursor & dst, const SpanSequenceValueType & value_type,
    SampleOrKey what, MinOrMax mm) const
  {
    size_t count = (mm == MinOrMax::Min) ? 0 : value_type.sequence_bound();
    serialize_u32(dst, count);
    return serialize_size_bound_many(dst, count, value_type.element_value_type(), what, mm);
  }

  template<typename WriteCursor>
  bool serialize_size_bound(
    WriteCursor & dst, const BoolVectorValueType & value_type, SampleOrKey,
    MinOrMax mm) const
  {
    size_t count = (mm == MinOrMax::Min) ? 0 : value_type.sequence_bound();
    serialize_u32(dst, count);
    dst.put_bytes(nullptr, count);
    return true;
  }

  template<typename WriteCursor>
  bool serialize_size_bound(
    WriteCursor & dst, const AnyValueType * value_type, SampleOrKey what,
    MinOrMax mm) const
  {
    bool ret;
    value_type->apply([&](const auto & vt) {ret = serialize_size_bound(dst, vt, what, mm);});
    return ret;
  }

  template<typename WriteCursor>
  bool serialize_size_bound(
    WriteCursor & dst, const StructValueType & struct_info,
    SampleOrKey what, MinOrMax mm) const
  {
    bool all_fields = (what != SampleOrKey::Key || !struct_info.has_keys());
    for (size_t i = 0; i < struct_info.n_members(); i++) {
      auto member_info = struct_info.get_member(i);
      if (all_fields || member_info->is_key) {
        auto value_type = member_info->value_type;
        if (!serialize_size_bound(dst, value_type, what, mm)) {
          return false;
        }
      }
    }
    return true;
  }
};

std::unique_ptr<BaseCDRWriter> make_cdr_writer(
  const StructValueType * value_type,
  SampleOrRequest variant)
{
  return std::make_unique<CDRWriter>(value_type, variant);
}

template<typename Derived>
struct ReadCursorBase : public CursorBase<Derived>
{
};

struct DeserializeCursor : public ReadCursorBase<DeserializeCursor>
{
  const unsigned char * origin;
  const unsigned char * position;
  const unsigned char * end;

  explicit DeserializeCursor(const void * position, size_t size)
  : origin(static_cast<const unsigned char *>(position)),
    position(static_cast<const unsigned char *>(position)),
    end(static_cast<const unsigned char *>(position) + size)
  {
  }

  size_t offset() const
  {
    return static_cast<size_t>(position - origin);
  }

  const unsigned char * advance(size_t n_bytes)
  {
    if (static_cast<size_t>(end - position) < n_bytes) {
      throw std::runtime_error("CDR deserialization: truncated input");
    }
    unsigned char const * const p = position;
    position += n_bytes;
    return p;
  }

  void get_bytes(void * dst, size_t n_bytes)
  {
    void const * const p = advance(n_bytes);
    std::memcpy(dst, p, n_bytes);
  }

  void rebase(ptrdiff_t relative_origin)
  {
    origin += relative_origin;
  }
};

class CDRReader final : public BaseCDRReader
{
public:
  const EncodingVersion eversion;
  const StructValueType * m_root_value_type;
  const TriviallySerializedCache tsc;
  const SampleOrRequest m_variant;

public:
  explicit CDRReader(const StructValueType * root_value_type, SampleOrRequest variant)
  : eversion{EncodingVersion::XCDR1},
    m_root_value_type{root_value_type},
    tsc{root_value_type},
    m_variant{variant}
  {
  }

  void deserialize(void * dst, const void * cdr, size_t cdrsize, SampleOrKey what) const override
  {
    if (what != SampleOrKey::Key || m_root_value_type->has_keys()) {
      DeserializeCursor cursor(cdr, cdrsize);
      deserialize_top_level(cursor, dst, what);
    }
  }

  void extractkey(
    std::vector<byte> & dst, const void * cdr, size_t cdrsize,
    SampleOrKey what) const override
  {
    DeserializeCursor rdcursor(cdr, cdrsize);
    ByteVectorCursor wrcursor(dst);
    extractkey_top_level(rdcursor, wrcursor, what, false);
  }

  void extractkey_be(
    std::vector<byte> & dst, const void * cdr, size_t cdrsize,
    SampleOrKey what) const override
  {
    DeserializeCursor rdcursor(cdr, cdrsize);
    ByteVectorCursor wrcursor(dst);
    extractkey_top_level(rdcursor, wrcursor, what, native_endian() != endian::big);
  }

  size_t print(
    char * dst, size_t dstsize, const void * cdr, size_t cdrsize,
    SampleOrKey what) const override
  {
    if (dstsize == 0) {
      return 0;
    }
    if (what == SampleOrKey::Key && !m_root_value_type->has_keys()) {
      dst[0] = '\0';
      return 0;
    }
    DeserializeCursor rdcursor(cdr, cdrsize);
    std::ostringstream dststream;
    print_top_level(rdcursor, dststream, what, dstsize);
    auto dststring = dststream.str();
    if (dststring.size() < dstsize - 1) {
      std::memcpy(dst, dststring.data(), dststring.size());
      dst[dststring.size()] = '\0';
      return dststring.size();
    } else {
      std::memcpy(dst, dststring.data(), dstsize - 1);
      dst[dstsize - 1] = '\0';
      return dstsize - 1;
    }
  }

protected:
  template<typename ReadCursor>
  void deserialize_top_level(ReadCursor & src, void * dst, SampleOrKey what) const
  {
    unsigned char const * const rtps_header = src.advance(4);
    src.rebase(+4);
    // don't care about options, only allow CDR_BE, CDR_LE
    if (rtps_header[0] != 0 || rtps_header[1] > 1) {
      throw std::runtime_error("CDR deserialization: unrecognized header");
    }
    const bool bswap_src = (rtps_header[1] == 0) == (native_endian() == endian::little);
    if (what == SampleOrKey::Sample && m_variant == SampleOrRequest::Request) {
      auto const request = static_cast<cdds_request_wrapper_t *>(dst);
      src.get_bytes(&request->header.guid, sizeof(request->header.guid));
      src.get_bytes(&request->header.seq, sizeof(request->header.seq));
      if (bswap_src) {
        bswap(&request->header.guid);
        bswap(&request->header.seq);
      }
      dst = request->data;
    }
    if (what == SampleOrKey::Sample || m_root_value_type->has_keys()) {
      deserialize_maybe_bswap(
        src, static_cast<unsigned char *>(dst), m_root_value_type, what,
        bswap_src);
    }
    src.rebase(-4);
  }

  enum class ExtractKeyMode
  {
    Sample,
    Key,
    Skip
  };

  template<typename ReadCursor, typename WriteCursor>
  void extractkey_top_level(
    ReadCursor & src, WriteCursor & dst, SampleOrKey what,
    bool bswap_dst) const
  {
    // nothing to extract if type has no keys
    if (!m_root_value_type->has_keys()) {
      return;
    }

    unsigned char const * const rtps_header = src.advance(4);
    src.rebase(+4);
    // don't care about options, only allow CDR_BE, CDR_LE
    if (rtps_header[0] != 0 || rtps_header[1] > 1) {
      throw std::runtime_error("CDR deserialization: unrecognized header");
    }
    const bool bswap_src = (rtps_header[1] == 0) == (native_endian() == endian::little);
    const unsigned char rtps_header_native[] = {
      0,
      static_cast<unsigned char>((native_endian() ==
      (bswap_dst ? endian::big : endian::little)) ? 1 : 0),
      0, 0};
    dst.put_bytes(rtps_header_native, 4);
    dst.rebase(+4);
    if (what == SampleOrKey::Sample && m_variant == SampleOrRequest::Request) {
      src.advance(8);
    }
    // key-to-key transformation is needed when byteswapping
    // the top-level type has keys and so "all_fields_are_key" will be false
    ExtractKeyMode kmode = (what ==
      SampleOrKey::Key) ? ExtractKeyMode::Key : ExtractKeyMode::Sample;
    extractkey_maybe_bswap(src, dst, m_root_value_type, kmode, bswap_src, bswap_dst);
    dst.rebase(-4);
    src.rebase(-4);
  }

  template<typename ReadCursor>
  void print_top_level(ReadCursor & src, std::ostream & dst, SampleOrKey what, size_t limit) const
  {
    unsigned char const * const rtps_header = src.advance(4);
    src.rebase(+4);
    // don't care about options, only allow CDR_BE, CDR_LE
    if (rtps_header[0] != 0 || rtps_header[1] > 1) {
      throw std::runtime_error("CDR deserialization: unrecognized header");
    }
    const bool bswap_src = (rtps_header[1] == 0) == (native_endian() == endian::little);
    if (what == SampleOrKey::Sample && m_variant == SampleOrRequest::Request) {
      const auto u64 = PrimitiveValueType(ROSIDL_TypeKind::UINT64);
      print_maybe_bswap(src, dst, &u64, what, limit, bswap_src);
      print_maybe_bswap(src, dst, &u64, what, limit, bswap_src);
    }
    if (what == SampleOrKey::Sample || m_root_value_type->has_keys()) {
      print_maybe_bswap(src, dst, m_root_value_type, what, limit, bswap_src);
    }
    src.rebase(-4);
  }

  template<bool bswap_src, size_t sizeof_type, typename ReadCursor>
  void deserialize_primitive(ReadCursor & src, unsigned char * dst) const
  {
    src.align(sizeof_type);
    src.get_bytes(dst, sizeof_type);
    if constexpr (bswap_src) {
      bswapN<sizeof_type>(dst);
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize_u32(ReadCursor & src, uint32_t * dst) const
  {
    src.align(sizeof(*dst));
    src.get_bytes(dst, sizeof(*dst));
    if constexpr (bswap_src) {
      bswap(dst);
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize(
    ReadCursor & src, unsigned char * dst, const PrimitiveValueType & value_type,
    SampleOrKey) const
  {
    switch (value_type.type_kind()) {
      case ROSIDL_TypeKind::CHAR:
      case ROSIDL_TypeKind::BOOLEAN:
      case ROSIDL_TypeKind::OCTET:
      case ROSIDL_TypeKind::UINT8:
      case ROSIDL_TypeKind::INT8:
        deserialize_primitive<bswap_src, 1>(src, dst);
        break;
      case ROSIDL_TypeKind::WCHAR:
      case ROSIDL_TypeKind::UINT16:
      case ROSIDL_TypeKind::INT16:
        deserialize_primitive<bswap_src, 2>(src, dst);
        break;
      case ROSIDL_TypeKind::UINT32:
      case ROSIDL_TypeKind::INT32:
      case ROSIDL_TypeKind::FLOAT:
        deserialize_primitive<bswap_src, 4>(src, dst);
        break;
      case ROSIDL_TypeKind::UINT64:
      case ROSIDL_TypeKind::INT64:
      case ROSIDL_TypeKind::DOUBLE:
        deserialize_primitive<bswap_src, 8>(src, dst);
        break;
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
        unreachable();
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize(
    ReadCursor & src, unsigned char * dst, const U8StringValueType & value_type,
    SampleOrKey) const
  {
    uint32_t size;
    deserialize_u32<bswap_src>(src, &size);
    if (size == 0) {
      throw std::runtime_error("CDR deserialization: size-0 string");
    }
    using type = const std::char_traits<char>::char_type;
    const TypedSpan<type> srcdata{reinterpret_cast<type *>(src.advance(size)), size};
    if (srcdata.data()[size - 1] != '\0') {
      throw std::runtime_error("CDR deserialization: unterminated string");
    }
    const TypedSpan<type> srcslice{srcdata.data(), size - 1};
    value_type.assign(dst, srcslice);
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize(
    ReadCursor & src, unsigned char * dst, const U16StringValueType & value_type,
    SampleOrKey) const
  {
    uint32_t size;
    deserialize_u32<bswap_src>(src, &size);
    if (size % 2) {
      throw std::runtime_error("CDR deserialization: odd number of bytes in wstring");
    }
    using type = const std::char_traits<char16_t>::char_type;
    const TypedSpan<type> srcdata{reinterpret_cast<type *>(src.advance(size)), size / 2};
    value_type.assign(dst, srcdata);
    if constexpr (bswap_src) {
      auto dstdata = value_type.data(dst).data();
      for (size_t i = 0; i < size / 2; i++) {
        bswapN<2>(&dstdata[i]);
      }
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize_many(
    ReadCursor & src, void * vdst, size_t count,
    const AnyValueType * vt, SampleOrKey what) const
  {
    auto dst = static_cast<unsigned char *>(vdst);

    // nothing to do; not even alignment
    if (count == 0) {
      return;
    }

    // Deserialize the first element.
    deserialize<bswap_src>(src, dst, vt, what);
    if (--count == 0) {
      return;
    }

    // If the value type is primitive, we are now aligned.
    // It might be that the first element is not trivially serialized but the rest are;
    // e.g. if any element in a struct has CDR alignment more stringent than the first element.
    size_t value_size = vt->sizeof_type();
    dst += value_size;

    if (!bswap_src && tsc.lookup_many_trivially_serialized(src.offset(), vt)) {
      src.get_bytes(dst, count * value_size);
    } else {
      for (size_t i = 0; i < count; i++) {
        auto element = dst + i * value_size;
        deserialize<bswap_src>(src, element, vt, what);
      }
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize(
    ReadCursor & src, unsigned char * dst, const ArrayValueType & value_type,
    SampleOrKey what) const
  {
    deserialize_many<bswap_src>(
      src, value_type.get_data(dst), value_type.array_size(), value_type.element_value_type(),
      what);
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize(
    ReadCursor & src, unsigned char * dst,
    const SpanSequenceValueType & value_type, SampleOrKey what) const
  {
    uint32_t count;
    deserialize_u32<bswap_src>(src, &count);
    value_type.resize(dst, count);
    deserialize_many<bswap_src>(
      src, value_type.sequence_contents(dst), count, value_type.element_value_type(), what);
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize(
    ReadCursor & src, unsigned char * dst,
    const BoolVectorValueType & value_type, SampleOrKey) const
  {
    uint32_t count;
    deserialize_u32<bswap_src>(src, &count);
    auto srcdata = static_cast<const uint8_t *>(src.advance(count));
    value_type.assign(dst, srcdata, count);
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize(
    ReadCursor & src, unsigned char * struct_dst,
    const StructValueType & struct_info,
    SampleOrKey what) const
  {
    bool all_fields = (what != SampleOrKey::Key || !struct_info.has_keys());
    for (size_t i = 0; i < struct_info.n_members(); i++) {
      auto member_info = struct_info.get_member(i);
      if (all_fields || member_info->is_key) {
        auto value_type = member_info->value_type;
        auto member_dst = struct_dst + member_info->member_offset;
        deserialize<bswap_src>(src, member_dst, value_type, what);
      }
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void deserialize(
    ReadCursor & src, unsigned char * dst, const AnyValueType * value_type,
    SampleOrKey what) const
  {
    if (!bswap_src && what == SampleOrKey::Sample &&
      tsc.lookup_trivially_serialized(src.offset(), value_type))
    {
      src.get_bytes(dst, value_type->sizeof_type());
    } else {
      value_type->apply([&](const auto & vt) {return deserialize<bswap_src>(src, dst, vt, what);});
    }
  }

  template<typename ReadCursor>
  void deserialize_maybe_bswap(
    ReadCursor & src, unsigned char * dst,
    const AnyValueType * value_type, SampleOrKey what, bool bswap_src) const
  {
    if (bswap_src) {
      deserialize<true>(src, dst, value_type, what);
    } else {
      deserialize<false>(src, dst, value_type, what);
    }
  }

  template<bool bswap_src, bool bswap_dst, size_t sizeof_type, typename ReadCursor,
    typename WriteCursor>
  void extractkey_primitive(ReadCursor & src, WriteCursor & dst, ExtractKeyMode mode) const
  {
    src.align(sizeof_type);
    auto srcdata = src.advance(sizeof_type);
    if (mode != ExtractKeyMode::Skip) {
      dst.align(sizeof_type);
      auto dstdata = dst.put_bytes(srcdata, sizeof_type);
      if (bswap_src != bswap_dst) {
        bswapN<sizeof_type>(dstdata);
      }
    }
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey(
    ReadCursor & src, WriteCursor & dst, const PrimitiveValueType & value_type,
    ExtractKeyMode mode) const
  {
    switch (value_type.type_kind()) {
      case ROSIDL_TypeKind::CHAR:
      case ROSIDL_TypeKind::BOOLEAN:
      case ROSIDL_TypeKind::OCTET:
      case ROSIDL_TypeKind::UINT8:
      case ROSIDL_TypeKind::INT8:
        extractkey_primitive<bswap_src, bswap_dst, 1>(src, dst, mode);
        break;
      case ROSIDL_TypeKind::WCHAR:
      case ROSIDL_TypeKind::UINT16:
      case ROSIDL_TypeKind::INT16:
        extractkey_primitive<bswap_src, bswap_dst, 2>(src, dst, mode);
        break;
      case ROSIDL_TypeKind::UINT32:
      case ROSIDL_TypeKind::INT32:
      case ROSIDL_TypeKind::FLOAT:
        extractkey_primitive<bswap_src, bswap_dst, 4>(src, dst, mode);
        break;
      case ROSIDL_TypeKind::UINT64:
      case ROSIDL_TypeKind::INT64:
      case ROSIDL_TypeKind::DOUBLE:
        extractkey_primitive<bswap_src, bswap_dst, 8>(src, dst, mode);
        break;
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
        unreachable();
    }
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey(
    ReadCursor & src, WriteCursor & dst, const U8StringValueType &,
    ExtractKeyMode mode) const
  {
    uint32_t size;
    deserialize_u32<bswap_src>(src, &size);
    if (size == 0) {
      throw std::runtime_error("CDR deserialization: size-0 string");
    }
    using type = const std::char_traits<char>::char_type;
    const TypedSpan<type> srcdata{reinterpret_cast<type *>(src.advance(size)), size};
    if (srcdata.data()[size - 1] != '\0') {
      throw std::runtime_error("CDR deserialization: unterminated string");
    }
    if (mode != ExtractKeyMode::Skip) {
      dst.align(4);
      auto dstsize = dst.put_bytes(&size, 4);
      if (bswap_dst) {
        bswapN<4>(dstsize);
      }
      dst.put_bytes(srcdata.data(), srcdata.size_bytes());
    }
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey(
    ReadCursor & src, WriteCursor & dst, const U16StringValueType &,
    ExtractKeyMode mode) const
  {
    uint32_t size;
    deserialize_u32<bswap_src>(src, &size);
    if (size % 2) {
      throw std::runtime_error("CDR deserialization: odd number of bytes in wstring");
    }
    using type = const std::char_traits<char16_t>::char_type;
    const TypedSpan<type> srcdata{reinterpret_cast<type *>(src.advance(size)), size / 2};
    if (mode != ExtractKeyMode::Skip) {
      dst.align(4);
      auto dstsize = dst.put_bytes(&size, 4);
      if (bswap_dst) {
        bswapN<4>(dstsize);
      }
      auto dstdata =
        static_cast<uint16_t *>(dst.put_bytes(srcdata.data(), srcdata.size_bytes()));
      if constexpr (bswap_src) {
        for (size_t i = 0; i < size / 2; i++) {
          bswap(&dstdata[i]);
        }
      }
    }
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey_many(
    ReadCursor & src, WriteCursor & dst, size_t count,
    const AnyValueType * vt, ExtractKeyMode mode) const
  {
    // nothing to do; not even alignment
    if (count == 0) {
      return;
    }

    // Extract the first element.
    extractkey<bswap_src, bswap_dst>(src, dst, vt, mode);
    if (--count == 0) {
      return;
    }

    // If the value type is primitive, we are now aligned.
    // It might be that the first element is not trivially serialized but the rest are;
    // e.g. if any element in a struct has CDR alignment more stringent than the first element.
    size_t value_size = vt->sizeof_type();

    if (!(bswap_src || bswap_dst) && tsc.lookup_many_trivially_serialized(src.offset(), vt)) {
      size_t sz = count * value_size;
      auto srcdata = src.advance(sz);
      if (mode != ExtractKeyMode::Skip) {
        dst.put_bytes(srcdata, sz);
      }
    } else {
      for (size_t i = 0; i < count; i++) {
        extractkey<bswap_src, bswap_dst>(src, dst, vt, mode);
      }
    }
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey(
    ReadCursor & src, WriteCursor & dst, const ArrayValueType & value_type,
    ExtractKeyMode mode) const
  {
    extractkey_many<bswap_src, bswap_dst>(
      src, dst, value_type.array_size(),
      value_type.element_value_type(), mode);
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey(
    ReadCursor & src, WriteCursor & dst,
    const SpanSequenceValueType & value_type, ExtractKeyMode mode) const
  {
    uint32_t count;
    deserialize_u32<bswap_src>(src, &count);
    if (mode != ExtractKeyMode::Skip) {
      dst.align(4);
      auto dstdata = dst.put_bytes(&count, 4);
      if (bswap_dst) {
        bswapN<4>(dstdata);
      }
    }
    extractkey_many<bswap_src, bswap_dst>(src, dst, count, value_type.element_value_type(), mode);
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey(
    ReadCursor & src, WriteCursor & dst,
    const BoolVectorValueType &, ExtractKeyMode mode) const
  {
    uint32_t count;
    deserialize_u32<bswap_src>(src, &count);
    auto srcdata = static_cast<const uint8_t *>(src.advance(count));
    if (mode != ExtractKeyMode::Skip) {
      dst.align(4);
      auto dstdata = dst.put_bytes(&count, 4);
      if (bswap_dst) {
        bswapN<4>(dstdata);
      }
      dst.put_bytes(srcdata, count);
    }
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey(
    ReadCursor & src, WriteCursor & dst,
    const StructValueType & struct_info,
    ExtractKeyMode mode) const
  {
    bool all_fields_are_key = !struct_info.has_keys();
    for (size_t i = 0; i < struct_info.n_members(); i++) {
      auto member_info = struct_info.get_member(i);
      auto value_type = member_info->value_type;
      switch (mode) {
        case ExtractKeyMode::Skip:
          extractkey<bswap_src, bswap_dst>(src, dst, value_type, mode);
          break;
        case ExtractKeyMode::Key:
          if (member_info->is_key || all_fields_are_key) {
            extractkey<bswap_src, bswap_dst>(src, dst, value_type, mode);
          }
          break;
        case ExtractKeyMode::Sample:
          if (member_info->is_key || all_fields_are_key) {
            extractkey<bswap_src, bswap_dst>(src, dst, value_type, mode);
          } else {
            extractkey<bswap_src, bswap_dst>(src, dst, value_type, ExtractKeyMode::Skip);
          }
          break;
      }
    }
  }

  template<bool bswap_src, bool bswap_dst, typename ReadCursor, typename WriteCursor>
  void extractkey(
    ReadCursor & src, WriteCursor & dst, const AnyValueType * value_type,
    ExtractKeyMode mode) const
  {
    if (mode == ExtractKeyMode::Skip && tsc.lookup_trivially_serialized(src.offset(), value_type)) {
      // Optimise for skipping non-key fields
      const size_t sz = value_type->sizeof_type();
      static_cast<void>(src.advance(sz));
    } else {
      auto f = [&](const auto & vt) {
          return extractkey<bswap_src, bswap_dst>(src, dst, vt, mode);
        };
      value_type->apply(f);
    }
  }

  template<typename ReadCursor, typename WriteCursor>
  void extractkey_maybe_bswap(
    ReadCursor & src, WriteCursor & dst, const AnyValueType * value_type,
    ExtractKeyMode mode, bool bswap_src, bool bswap_dst) const
  {
    if (bswap_src) {
      if (bswap_dst) {
        extractkey<true, true>(src, dst, value_type, mode);
      } else {
        extractkey<true, false>(src, dst, value_type, mode);
      }
    } else {
      if (bswap_dst) {
        extractkey<false, true>(src, dst, value_type, mode);
      } else {
        extractkey<false, false>(src, dst, value_type, mode);
      }
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void print(
    ReadCursor & src, std::ostream & dst, const PrimitiveValueType & value_type,
    SampleOrKey, size_t) const
  {
    src.align(value_type.cdralignof_type());
    const unsigned char * srcdata = src.advance(value_type.cdrsizeof_type());
    union {
      unsigned char buf[8];
      double d;
      uint64_t u;
    } tmp;
    std::memcpy(tmp.buf, srcdata, value_type.cdrsizeof_type());
    if constexpr (bswap_src) {
      switch (value_type.cdrsizeof_type()) {
        case 2: bswapN<2>(tmp.buf); break;
        case 4: bswapN<4>(tmp.buf); break;
        case 8: bswapN<8>(tmp.buf); break;
      }
    }
    switch (value_type.type_kind()) {
      case ROSIDL_TypeKind::CHAR:
        dst << "'" << std::string(reinterpret_cast<char *>(tmp.buf), 1) << "'";
        break;
      case ROSIDL_TypeKind::BOOLEAN:
        dst << (tmp.buf[0] ? "true" : "false");
        break;
      case ROSIDL_TypeKind::OCTET:
      case ROSIDL_TypeKind::UINT8:
        dst << static_cast<uint32_t>(*reinterpret_cast<uint8_t *>(tmp.buf));
        break;
      case ROSIDL_TypeKind::INT8:
        dst << static_cast<int32_t>(*reinterpret_cast<int8_t *>(tmp.buf));
        break;
      case ROSIDL_TypeKind::WCHAR:
      case ROSIDL_TypeKind::UINT16:
        dst << static_cast<uint32_t>(*reinterpret_cast<uint16_t *>(tmp.buf));
        break;
      case ROSIDL_TypeKind::INT16:
        dst << static_cast<int32_t>(*reinterpret_cast<int16_t *>(tmp.buf));
        break;
      case ROSIDL_TypeKind::UINT32:
        dst << *reinterpret_cast<uint32_t *>(tmp.buf);
        break;
      case ROSIDL_TypeKind::INT32:
        dst << *reinterpret_cast<int32_t *>(tmp.buf);
        break;
      case ROSIDL_TypeKind::FLOAT:
        dst << *reinterpret_cast<float *>(tmp.buf);
        break;
      case ROSIDL_TypeKind::UINT64:
        dst << *reinterpret_cast<uint64_t *>(tmp.buf);
        break;
      case ROSIDL_TypeKind::INT64:
        dst << *reinterpret_cast<int64_t *>(tmp.buf);
        break;
      case ROSIDL_TypeKind::DOUBLE:
        dst << *reinterpret_cast<double *>(tmp.buf);
        break;
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
        unreachable();
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void print(
    ReadCursor & src, std::ostream & dst, const U8StringValueType &, SampleOrKey,
    size_t) const
  {
    uint32_t size;
    deserialize_u32<bswap_src>(src, &size);
    if (size == 0) {
      throw std::runtime_error("CDR deserialization: size-0 string");
    }
    using type = const std::char_traits<char>::char_type;
    const TypedSpan<type> srcdata{reinterpret_cast<type *>(src.advance(size)), size};
    if (srcdata.data()[size - 1] != '\0') {
      throw std::runtime_error("CDR deserialization: unterminated string");
    }
    dst << "\"" << std::string(srcdata.data(), size - 1) << "\"";
  }

  template<bool bswap_src, typename ReadCursor>
  void print(
    ReadCursor & src, std::ostream & dst, const U16StringValueType &, SampleOrKey,
    size_t) const
  {
    uint32_t size;
    deserialize_u32<bswap_src>(src, &size);
    if (size % 2) {
      throw std::runtime_error("CDR deserialization: odd number of bytes in wstring");
    }
    static_cast<void>(src.advance(size));
    dst << std::string("(wstring)");
  }

  template<bool bswap_src, typename ReadCursor>
  void print_many(
    ReadCursor & src, std::ostream & dst, size_t count, const AnyValueType * vt,
    SampleOrKey what, size_t limit) const
  {
    dst << "{";
    for (size_t i = 0; i < count; i++) {
      const auto pos = dst.tellp();
      if (pos < 0 || static_cast<size_t>(pos) > limit) {
        return;
      }
      if (i > 0) {dst << ",";}
      print<bswap_src>(src, dst, vt, what, limit);
    }
    dst << "}";
  }

  template<bool bswap_src, typename ReadCursor>
  void print(
    ReadCursor & src, std::ostream & dst, const ArrayValueType & value_type,
    SampleOrKey what, size_t limit) const
  {
    print_many<bswap_src>(
      src, dst, value_type.array_size(), value_type.element_value_type(), what,
      limit);
  }

  template<bool bswap_src, typename ReadCursor>
  void print(
    ReadCursor & src, std::ostream & dst,
    const SpanSequenceValueType & value_type, SampleOrKey what, size_t limit) const
  {
    uint32_t count;
    deserialize_u32<bswap_src>(src, &count);
    print_many<bswap_src>(src, dst, count, value_type.element_value_type(), what, limit);
  }

  template<bool bswap_src, typename ReadCursor>
  void print(
    ReadCursor & src, std::ostream & dst,
    const BoolVectorValueType & value_type, SampleOrKey what, size_t limit) const
  {
    const auto vt = PrimitiveValueType(ROSIDL_TypeKind::BOOLEAN);
    uint32_t count;
    deserialize_u32<bswap_src>(src, &count);
    print_many<bswap_src>(src, dst, count, value_type.element_value_type(), what, limit);
  }

  template<bool bswap_src, typename ReadCursor>
  void print(
    ReadCursor & src, std::ostream & dst,
    const StructValueType & struct_info,
    SampleOrKey what, size_t limit) const
  {
    bool all_fields = (what != SampleOrKey::Key || !struct_info.has_keys());
    bool first = true;
    for (size_t i = 0; i < struct_info.n_members(); i++) {
      const auto pos = dst.tellp();
      if (pos < 0 || static_cast<size_t>(pos) > limit) {
        return;
      }
      auto member_info = struct_info.get_member(i);
      if (member_info->is_key || all_fields) {
        if (first) {
          dst << "{";
          first = false;
        } else {
          dst << ",";
        }
        auto value_type = member_info->value_type;
        print<bswap_src>(src, dst, value_type, what, limit);
      }
    }
    if (!first) {
      dst << "}";
    }
  }

  template<bool bswap_src, typename ReadCursor>
  void print(
    ReadCursor & src, std::ostream & dst, const AnyValueType * value_type,
    SampleOrKey what, size_t limit) const
  {
    value_type->apply(
      [&](const auto & vt) {
        return print<bswap_src>(src, dst, vt, what, limit);
      });
  }

  template<typename ReadCursor>
  void print_maybe_bswap(
    ReadCursor & src, std::ostream & dst, const AnyValueType * value_type,
    SampleOrKey what, size_t limit, bool bswap_src) const
  {
    if (bswap_src) {
      print<true>(src, dst, value_type, what, limit);
    } else {
      print<false>(src, dst, value_type, what, limit);
    }
  }
};

std::unique_ptr<BaseCDRReader> make_cdr_reader(
  const StructValueType * value_type,
  SampleOrRequest variant)
{
  return std::make_unique<CDRReader>(value_type, variant);
}
}  // namespace rmw_cyclonedds_cpp
