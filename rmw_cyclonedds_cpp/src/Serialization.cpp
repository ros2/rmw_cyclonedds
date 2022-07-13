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
#endif

#include "Serialization.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "TypeSupport2.hpp"
#include "bytewise.hpp"

namespace rmw_cyclonedds_cpp
{

struct CDRCursor
{
  CDRCursor() = default;
  ~CDRCursor() = default;

  // don't want to accidentally copy
  explicit CDRCursor(CDRCursor const &) = delete;
  void operator=(CDRCursor const & x) = delete;

  // virtual functions to be implemented
  // get the cursor's current offset.
  virtual size_t offset() const = 0;
  // advance the cursor.
  virtual void advance(size_t n_bytes) = 0;
  // Copy bytes to the current cursor location (if needed) and advance the cursor
  virtual void put_bytes(const void * data, size_t size) = 0;
  virtual bool ignores_data() const = 0;
  // Move the logical origin this many places
  virtual void rebase(ptrdiff_t relative_origin) = 0;

  void align(size_t n_bytes)
  {
    assert(n_bytes > 0);
    size_t start_offset = offset();
    if (n_bytes == 1 || start_offset % n_bytes == 0) {
      return;
    }
    advance(n_bytes - start_offset % n_bytes);
    assert(offset() - start_offset < n_bytes);
    assert(offset() % n_bytes == 0);
  }
  ptrdiff_t operator-(const CDRCursor & other) const
  {
    return static_cast<ptrdiff_t>(offset()) - static_cast<ptrdiff_t>(other.offset());
  }
};

struct SizeCursor : public CDRCursor
{
  SizeCursor()
  : SizeCursor(0) {}
  explicit SizeCursor(size_t initial_offset)
  : m_offset(initial_offset) {}
  explicit SizeCursor(CDRCursor & c)
  : m_offset(c.offset()) {}

  size_t m_offset;
  size_t offset() const final {return m_offset;}
  void advance(size_t n_bytes) final {m_offset += n_bytes;}
  void put_bytes(const void *, size_t n_bytes) final {advance(n_bytes);}
  bool ignores_data() const final {return true;}
  void rebase(ptrdiff_t relative_origin) override
  {
    // we're moving the *origin* so this has to change in the *opposite* direction
    m_offset -= relative_origin;
  }
};

struct DataCursor : public CDRCursor
{
  const void * origin;
  void * position;

  explicit DataCursor(void * position)
  : origin(position), position(position) {}

  size_t offset() const final {return (const byte *)position - (const byte *)origin;}
  void advance(size_t n_bytes) final
  {
    std::memset(position, '\0', n_bytes);
    position = byte_offset(position, n_bytes);
  }
  void put_bytes(const void * bytes, size_t n_bytes) final
  {
    if (n_bytes == 0) {
      return;
    }
    std::memcpy(position, bytes, n_bytes);
    position = byte_offset(position, n_bytes);
  }
  bool ignores_data() const final {return false;}
  void rebase(ptrdiff_t relative_origin) final {origin = byte_offset(origin, relative_origin);}
};

enum class EncodingVersion
{
  CDR_Legacy,
  CDR1,
};

class CDRWriter : public BaseCDRWriter
{
public:
  struct CacheKey
  {
    size_t align;
    const AnyValueType * value_type;
    bool operator==(const CacheKey & other) const
    {
      return align == other.align && value_type == other.value_type;
    }

    struct Hash
    {
      size_t operator()(const CacheKey & k) const
      {
        return std::hash<decltype(align)>{}(k.align) ^
               ((std::hash<decltype(value_type)>{}(k.value_type)) << 1U);
      }
    };
  };

  const EncodingVersion eversion;
  const size_t max_align;
  std::unique_ptr<const StructValueType> m_root_value_type;
  std::unordered_map<CacheKey, bool, CacheKey::Hash> trivially_serialized_cache;

public:
  explicit CDRWriter(std::unique_ptr<const StructValueType> root_value_type)
  : eversion{EncodingVersion::CDR_Legacy}, max_align{8},
    m_root_value_type{std::move(root_value_type)},
    trivially_serialized_cache{}
  {
    assert(m_root_value_type);
    register_serializable_type(m_root_value_type.get());
  }

  void register_serializable_type(const AnyValueType * t)
  {
    for (size_t align = 0; align < max_align; align++) {
      CacheKey key{align, t};
      if (trivially_serialized_cache.find(key) != trivially_serialized_cache.end()) {
        continue;
      }

      bool & result = trivially_serialized_cache[key];

      switch (t->e_value_type()) {
        case EValueType::PrimitiveValueType: {
            auto tt = static_cast<const PrimitiveValueType *>(t);
            result = is_trivially_serialized(align, *tt);
          }
          break;
        case EValueType::ArrayValueType: {
            auto tt = static_cast<const ArrayValueType *>(t);
            result = compute_trivially_serialized(align, *tt);
            register_serializable_type(tt->element_value_type());
          }
          break;
        case EValueType::StructValueType: {
            auto tt = static_cast<const StructValueType *>(t);
            for (size_t i = 0; i < tt->n_members(); i++) {
              register_serializable_type(tt->get_member(i)->value_type);
            }
            result = is_trivially_serialized(align, *tt);
          }
          break;
        case EValueType::SpanSequenceValueType: {
            auto tt = static_cast<const SpanSequenceValueType *>(t);
            register_serializable_type(tt->element_value_type());
          }
          result = false;
          break;
        case EValueType::U8StringValueType:
        case EValueType::U16StringValueType:
        case EValueType::BoolVectorValueType:
          result = false;
          break;
        default:
          unreachable();
      }
    }
  }
  size_t get_serialized_size(const void * data) const override
  {
    SizeCursor cursor;

    serialize_top_level(&cursor, data);
    return cursor.offset();
  }

  void serialize(void * dest, const void * data) const override
  {
    DataCursor cursor(dest);
    serialize_top_level(&cursor, data);
  }

  size_t get_serialized_size(
    const cdds_request_wrapper_t & request) const override
  {
    SizeCursor cursor;
    serialize_top_level(&cursor, request);
    return cursor.offset();
  }

  void serialize(
    void * dest, const cdds_request_wrapper_t & request) const override
  {
    DataCursor cursor(dest);
    serialize_top_level(&cursor, request);
  }

  void serialize_top_level(
    CDRCursor * cursor, const void * data) const
  {
    put_rtps_header(cursor);

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor->rebase(+4);
    }

    if (m_root_value_type->n_members() == 0 && eversion == EncodingVersion::CDR_Legacy) {
      char dummy = '\0';
      cursor->put_bytes(&dummy, 1);
    } else {
      serialize(cursor, data, m_root_value_type.get());
    }

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor->rebase(-4);
    }
  }

  void serialize_top_level(
    CDRCursor * cursor, const cdds_request_wrapper_t & request) const
  {
    put_rtps_header(cursor);
    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor->rebase(+4);
    }
    cursor->put_bytes(&request.header.guid, sizeof(request.header.guid));
    cursor->put_bytes(&request.header.seq, sizeof(request.header.seq));

    serialize(cursor, request.data, m_root_value_type.get());

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor->rebase(-4);
    }
  }

protected:
  void put_rtps_header(CDRCursor * cursor) const
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
      default:
        unreachable();
    }
    std::array<char, 4> rtps_header{{eversion_byte,
      // encoding format = PLAIN_CDR
      (native_endian() == endian::little) ? '\1' : '\0',
      // options
      '\0', '\0'}};
    cursor->put_bytes(rtps_header.data(), rtps_header.size());
  }

  void serialize_u32(CDRCursor * cursor, size_t value) const
  {
    assert(value <= std::numeric_limits<uint32_t>::max());
    auto u32_value = static_cast<uint32_t>(value);
    cursor->align(4);
    cursor->put_bytes(&u32_value, 4);
  }

  static size_t get_cdr_size_of_primitive(ROSIDL_TypeKind tk)
  {
    /// return 0 if the value type is not primitive
    /// else returns the number of bytes it should serialize to
    switch (tk) {
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
      case ROSIDL_TypeKind::LONG_DOUBLE:
        return 16;
      default:
        return 0;
    }
  }

  bool is_trivially_serialized(size_t align, const StructValueType & p) const
  {
    align %= max_align;

    size_t offset = align;
    for (size_t i = 0; i < p.n_members(); i++) {
      auto m = p.get_member(i);
      if (m->member_offset != offset - align) {
        return false;
      }
      if (!compute_trivially_serialized(offset % max_align, m->value_type)) {
        return false;
      }
      offset += m->value_type->sizeof_type();
    }

    return offset == align + p.sizeof_struct();
  }

  bool is_trivially_serialized(size_t align, const PrimitiveValueType & v) const
  {
    align %= max_align;

    // Value of 0 implies it is not a primitive, which should not happen and is checked elsewhere
    const size_t cdr_alignof = get_cdr_alignof_primitive(v.type_kind());
    assert(0 != cdr_alignof);
    if (align % cdr_alignof != 0) {
      return false;
    }
    return v.sizeof_type() == get_cdr_size_of_primitive(v.type_kind());
  }

  bool lookup_many_trivially_serialized(size_t align, const AnyValueType * evt) const
  {
    align %= max_align;
    // CLEVERNESS ALERT
    // we take advantage of the fact that if something is aligned at offset A and at offset A+N
    // then the alignment requirement of its elements divides A+k*N for all k
    return lookup_trivially_serialized(align, evt) &&
           lookup_trivially_serialized((align + evt->sizeof_type()) % max_align, evt);
  }

  bool compute_trivially_serialized(size_t align, const ArrayValueType & v) const
  {
    auto evt = v.element_value_type();
    align %= max_align;
    // CLEVERNESS ALERT
    // we take advantage of the fact that if something is aligned at offset A and at offset A+N
    // then the alignment requirement of its elements divides A+k*N for all k
    return compute_trivially_serialized(align, evt) &&
           compute_trivially_serialized((align + evt->sizeof_type()) % max_align, evt);
  }

  /// Returns true if a memcpy is all it takes to serialize this value
  bool lookup_trivially_serialized(size_t align, const AnyValueType * p) const
  {
    CacheKey key{align % max_align, p};
    return trivially_serialized_cache.at(key);
  }

  /// Returns true if a memcpy is all it takes to serialize this value
  bool compute_trivially_serialized(size_t align, const AnyValueType * p) const
  {
    align %= max_align;

    bool result;
    switch (p->e_value_type()) {
      case EValueType::PrimitiveValueType:
        result = is_trivially_serialized(align, *static_cast<const PrimitiveValueType *>(p));
        break;
      case EValueType::StructValueType:
        result = is_trivially_serialized(align, *static_cast<const StructValueType *>(p));
        break;
      case EValueType::ArrayValueType:
        result = compute_trivially_serialized(align, *static_cast<const ArrayValueType *>(p));
        break;
      case EValueType::U8StringValueType:
      case EValueType::U16StringValueType:
      case EValueType::SpanSequenceValueType:
      case EValueType::BoolVectorValueType:
        result = false;
        break;
      default:
        unreachable();
    }
    return result;
  }

  size_t get_cdr_alignof_primitive(ROSIDL_TypeKind tk) const
  {
    /// return 0 if the value type is not primitive
    /// else returns the number of bytes it should align to
    size_t sizeof_ = get_cdr_size_of_primitive(tk);
    return sizeof_ < max_align ? sizeof_ : max_align;
  }

  void serialize(CDRCursor * cursor, const void * data, const PrimitiveValueType & value_type) const
  {
    cursor->align(get_cdr_alignof_primitive(value_type.type_kind()));
    size_t n_bytes = get_cdr_size_of_primitive(value_type.type_kind());

    switch (value_type.type_kind()) {
      case ROSIDL_TypeKind::FLOAT:
        assert(std::numeric_limits<float>::is_iec559);
        cursor->put_bytes(data, n_bytes);
        return;
      case ROSIDL_TypeKind::DOUBLE:
        assert(std::numeric_limits<double>::is_iec559);
        cursor->put_bytes(data, n_bytes);
        return;
      case ROSIDL_TypeKind::LONG_DOUBLE:
        assert(std::numeric_limits<long double>::is_iec559);
        cursor->put_bytes(data, n_bytes);
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
          cursor->put_bytes(data, n_bytes);
        } else {
          const void * offset_data = byte_offset(data, value_type.sizeof_type() - n_bytes);
          cursor->put_bytes(offset_data, n_bytes);
        }
        return;
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
      default:
        unreachable();
    }
  }

  void serialize(CDRCursor * cursor, const void * data, const U8StringValueType & value_type) const
  {
    auto str = value_type.data(data);
    serialize_u32(cursor, str.size() + 1);
    cursor->put_bytes(str.data(), str.size());
    char terminator = '\0';
    cursor->put_bytes(&terminator, 1);
  }

  void serialize(CDRCursor * cursor, const void * data, const U16StringValueType & value_type) const
  {
    auto str = value_type.data(data);
    if (eversion == EncodingVersion::CDR_Legacy) {
      serialize_u32(cursor, str.size());
      if (cursor->ignores_data()) {
        cursor->advance(sizeof(wchar_t) * str.size());
      } else {
        for (wchar_t c : str) {
          cursor->put_bytes(&c, sizeof(wchar_t));
        }
      }
    } else {
      serialize_u32(cursor, str.size_bytes());
      cursor->put_bytes(str.data(), str.size_bytes());
    }
  }

  void serialize(CDRCursor * cursor, const void * data, const ArrayValueType & value_type) const
  {
    serialize_many(
      cursor, value_type.get_data(data), value_type.array_size(), value_type.element_value_type());
  }

  void serialize(
    CDRCursor * cursor, const void * data,
    const SpanSequenceValueType & value_type) const
  {
    size_t count = value_type.sequence_size(data);
    serialize_u32(cursor, count);
    serialize_many(
      cursor, value_type.sequence_contents(data), count, value_type.element_value_type());
  }

  void serialize(
    CDRCursor * cursor, const void * data,
    const BoolVectorValueType & value_type) const
  {
    size_t count = value_type.size(data);
    serialize_u32(cursor, count);
    if (cursor->ignores_data()) {
      cursor->advance(count);
    } else {
      for (auto iter = value_type.begin(data); iter != value_type.end(data); ++iter) {
        bool b = *iter;
        cursor->put_bytes(&b, 1);
      }
    }
  }

  void serialize(CDRCursor * cursor, const void * data, const AnyValueType * value_type) const
  {
    if (lookup_trivially_serialized(cursor->offset(), value_type)) {
      cursor->put_bytes(data, value_type->sizeof_type());
    } else {
//      value_type->apply([&](const auto & vt) {return serialize(cursor, data, vt);});
      if (auto s = dynamic_cast<const PrimitiveValueType *>(value_type)) {
        return serialize(cursor, data, *s);
      }
      if (auto s = dynamic_cast<const U8StringValueType *>(value_type)) {
        return serialize(cursor, data, *s);
      }
      if (auto s = dynamic_cast<const U16StringValueType *>(value_type)) {
        return serialize(cursor, data, *s);
      }
      if (auto s = dynamic_cast<const StructValueType *>(value_type)) {
        return serialize(cursor, data, *s);
      }
      if (auto s = dynamic_cast<const ArrayValueType *>(value_type)) {
        return serialize(cursor, data, *s);
      }
      if (auto s = dynamic_cast<const SpanSequenceValueType *>(value_type)) {
        return serialize(cursor, data, *s);
      }
      if (auto s = dynamic_cast<const BoolVectorValueType *>(value_type)) {
        return serialize(cursor, data, *s);
      }
      unreachable();
    }
  }

  void serialize_many(
    CDRCursor * cursor, const void * data, size_t count,
    const AnyValueType * vt) const
  {
    // nothing to do; not even alignment
    if (count == 0) {
      return;
    }

    // Serialize the first element.
    serialize(cursor, data, vt);

    // If the value type is primitive, we are now aligned.
    // It might be that the first element is not trivially serialized but the rest are;
    // e.g. if any element in a struct has CDR alignment more stringent than the first element.

    data = byte_offset(data, vt->sizeof_type());
    --count;
    if (count == 0) {
      return;
    }

    if (lookup_many_trivially_serialized(cursor->offset(), vt)) {
      size_t value_size = vt->sizeof_type();
      cursor->put_bytes(data, count * value_size);
      return;
    } else {
      for (size_t i = 0; i < count; i++) {
        auto element = byte_offset(data, i * vt->sizeof_type());
        serialize(cursor, element, vt);
      }
    }
  }

  void serialize(
    CDRCursor * cursor, const void * struct_data,
    const StructValueType & struct_info) const
  {
    for (size_t i = 0; i < struct_info.n_members(); i++) {
      auto member_info = struct_info.get_member(i);
      auto value_type = member_info->value_type;
      auto member_data = byte_offset(struct_data, member_info->member_offset);
      serialize(cursor, member_data, value_type);
    }
  }
};

std::unique_ptr<BaseCDRWriter> make_cdr_writer(std::unique_ptr<StructValueType> value_type)
{
  return std::make_unique<CDRWriter>(std::move(value_type));
}

}  // namespace rmw_cyclonedds_cpp
