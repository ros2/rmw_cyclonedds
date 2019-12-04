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
#include <unordered_map>
#include <vector>

#include "TypeSupport2.hpp"
#include "bytewise.hpp"
#include "rmw_cyclonedds_cpp/TypeSupport_impl.hpp"

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
    advance((-start_offset) % n_bytes);
    assert(offset() - start_offset < n_bytes);
    assert(offset() % n_bytes == 0);
  }
  ptrdiff_t operator-(const CDRCursor & other) const {return offset() - other.offset();}
};

struct SizeCursor : CDRCursor
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
  void advance(size_t n_bytes) final {position = byte_offset(position, n_bytes);}
  void put_bytes(const void * bytes, size_t n_bytes) final
  {
    std::memcpy(position, bytes, n_bytes);
    advance(n_bytes);
  }
  bool ignores_data() const final {return false;}
  void rebase(ptrdiff_t relative_origin) final {origin = byte_offset(origin, relative_origin);}
};

enum class EncodingVersion
{
  CDR_Legacy,
  CDR1,
};

class CDRWriter
{
public:
  const EncodingVersion eversion;
  const size_t max_align;

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
        return std::hash<decltype(align)>{} (k.align) ^
               ((std::hash<decltype(value_type)>{} (k.value_type)) << 1U);
      }
    };
  };

  std::unordered_map<CacheKey, bool, CacheKey::Hash> trivially_serialized_cache;

public:
  CDRWriter()
  : eversion{EncodingVersion::CDR_Legacy}, max_align{8}, trivially_serialized_cache{} {}

  void serialize_top_level(CDRCursor * cursor, const void * data, const StructValueType & support)
  {
    put_rtps_header(cursor);

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor->rebase(+4);
    }

    if (support.n_members() == 0 && eversion == EncodingVersion::CDR_Legacy) {
      char dummy = '\0';
      cursor->put_bytes(&dummy, 1);
    } else {
      serialize(cursor, data, support);
    }

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor->rebase(-4);
    }
  }

  void serialize_top_level(
    CDRCursor * cursor, const cdds_request_wrapper_t & request, const StructValueType & support)
  {
    put_rtps_header(cursor);
    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor->rebase(+4);
    }

    cursor->put_bytes(&request.header.guid, sizeof(request.header.guid));
    cursor->put_bytes(&request.header.seq, sizeof(request.header.seq));
    serialize(cursor, request.data, support);

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor->rebase(-4);
    }
  }

protected:
  void put_rtps_header(CDRCursor * cursor)
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
    cursor->put_bytes(rtps_header.data(), rtps_header.size());
  }

  void serialize_u32(CDRCursor * cursor, size_t value)
  {
    assert(value <= std::numeric_limits<uint32_t>::max());
    cursor->align(std::min(size_t(4), max_align));
    cursor->put_bytes(&value, 4);
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

  bool is_trivially_serialized(size_t align, const StructValueType & p)
  {
    align %= max_align;

    size_t offset = align;
    for (size_t i = 0; i < p.n_members(); i++) {
      auto m = p.get_member(i);
      if (m->member_offset != offset - align) {
        return false;
      }
      if (!is_trivially_serialized(offset % max_align, m->value_type)) {
        return false;
      }
      offset += m->value_type->sizeof_type();
    }

    return offset == align + p.sizeof_struct();
  }

  bool is_trivially_serialized(size_t align, const PrimitiveValueType & v)
  {
    align %= max_align;

    if (align % get_cdr_alignof_primitive(v.type_kind()) != 0) {
      return false;
    }
    return v.sizeof_type() == get_cdr_size_of_primitive(v.type_kind());
  }

  bool is_trivially_serialized(size_t align, const ArrayValueType & v)
  {
    align %= max_align;
    // if the first element is aligned, we take advantage of the foreknowledge that all future
    // elements will be aligned as well
    return is_trivially_serialized(align, v.element_value_type());
  }

  /// Returns true if a memcpy is all it takes to serialize this value
  bool is_trivially_serialized(size_t align, const AnyValueType * p)
  {
    align %= max_align;

    CacheKey key{align, p};
    auto iter = trivially_serialized_cache.find(key);
    bool result;
    if (iter != trivially_serialized_cache.end()) {
      result = iter->second;
    } else {
      switch (p->e_value_type()) {
        case EValueType::PrimitiveValueType:
          result = is_trivially_serialized(align, *static_cast<const PrimitiveValueType *>(p));
          break;
        case EValueType::StructValueType:
          result = is_trivially_serialized(align, *static_cast<const StructValueType *>(p));
          break;
        case EValueType::ArrayValueType:
          result = is_trivially_serialized(align, *static_cast<const ArrayValueType *>(p));
          break;
        case EValueType::U8StringValueType:
        case EValueType::U16StringValueType:
        case EValueType::SpanSequenceValueType:
        case EValueType::BoolVectorValueType:
          result = false;
      }
      trivially_serialized_cache.emplace(key, result);
    }
    return result;
  }

  size_t get_cdr_alignof_primitive(ROSIDL_TypeKind vt)
  {
    /// return 0 if the value type is not primitive
    /// else returns the number of bytes it should align to
    return std::min(get_cdr_size_of_primitive(vt), max_align);
  }

  void serialize(CDRCursor * cursor, const void * data, const PrimitiveValueType & value_type)
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
      case ROSIDL_TypeKind::INT64: {
          auto bytes = static_cast<const byte *>(data);
          if (native_endian() == endian::big) {
            bytes += value_type.sizeof_type() - n_bytes;
          }
          cursor->put_bytes(bytes, n_bytes);
        }
        return;
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
        throw std::logic_error("not a primitive");
    }
  }

  void serialize(CDRCursor * cursor, const void * data, const U8StringValueType & value_type)
  {
    auto str = value_type.data(data);
    serialize_u32(cursor, str.size() + 1);
    cursor->put_bytes(str.data(), str.size());
    char terminator = '\0';
    cursor->put_bytes(&terminator, 1);
  }

  void serialize(CDRCursor * cursor, const void * data, const U16StringValueType & value_type)
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

  void serialize(CDRCursor * cursor, const void * data, const ArrayValueType & value_type)
  {
    serialize_many(
      cursor, value_type.get_data(data), value_type.array_size(), value_type.element_value_type());
  }

  void serialize(CDRCursor * cursor, const void * data, const SpanSequenceValueType & value_type)
  {
    size_t count = value_type.sequence_size(data);
    serialize_u32(cursor, count);
    serialize_many(
      cursor, value_type.sequence_contents(data), count, value_type.element_value_type());
  }

  void serialize(CDRCursor * cursor, const void * data, const BoolVectorValueType & value_type)
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

  void serialize(CDRCursor * cursor, const void * data, const AnyValueType * value_type)
  {
    if (is_trivially_serialized(cursor->offset(), value_type)) {
      cursor->put_bytes(data, value_type->sizeof_type());
    } else {
      value_type->apply([&](const auto & vt) {return serialize(cursor, data, vt);});
      //    if (auto s = dynamic_cast<const PrimitiveValueType *>(value_type)) {
      //      return serialize(cursor, data, *s);
      //    }
      //    if (auto s = dynamic_cast<const AnyU8StringValueType *>(value_type)) {
      //      return serialize(cursor, data, *s);
      //    }
      //    if (auto s = dynamic_cast<const AnyU16StringValueType *>(value_type)) {
      //      return serialize(cursor, data, *s);
      //    }
      //    if (auto s = dynamic_cast<const AnyStructValueType *>(value_type)) {
      //      return serialize(cursor, data, *s);
      //    }
      //    if (auto s = dynamic_cast<const ArrayValueType *>(value_type)) {
      //      return serialize(cursor, data, *s);
      //    }
      //    if (auto s = dynamic_cast<const SpanSequenceValueType *>(value_type)) {
      //      return serialize(cursor, data, *s);
      //    }
      //    if (auto s = dynamic_cast<const BoolVectorValueType *>(value_type)) {
      //      return serialize(cursor, data, *s);
      //    }
      //    throw std::logic_error("Unhandled case");
    }
  }

  void serialize_many(CDRCursor * cursor, const void * data, size_t count, const AnyValueType * vt)
  {
    // nothing to do; not even alignment
    if (count == 0) {
      return;
    }

    if (auto p = dynamic_cast<const PrimitiveValueType *>(vt)) {
      cursor->align(get_cdr_alignof_primitive(p->type_kind()));
      size_t value_size = get_cdr_size_of_primitive(p->type_kind());
      assert(value_size);
      if (cursor->ignores_data()) {
        cursor->advance(count * value_size);
        return;
      }
      if (is_trivially_serialized(cursor->offset(), p)) {
        cursor->put_bytes(data, count * value_size);
        return;
      }
    }
    for (size_t i = 0; i < count; i++) {
      auto element = byte_offset(data, i * vt->sizeof_type());
      serialize(cursor, element, vt);
    }
  }

  void serialize(CDRCursor * cursor, const void * struct_data, const StructValueType & struct_info)
  {
    for (size_t i = 0; i < struct_info.n_members(); i++) {
      auto member_info = struct_info.get_member(i);
      auto value_type = member_info->value_type;
      auto member_data = byte_offset(struct_data, member_info->member_offset);
      serialize(cursor, member_data, value_type);
    }
  }
};

size_t get_serialized_size(const void * data, const rosidl_message_type_support_t & ts)
{
  SizeCursor cursor;
  CDRWriter().serialize_top_level(&cursor, data, *struct_type_from_rosidl(&ts));
  return cursor.offset();
}

void serialize(
  void * dest, const void * data, const rosidl_message_type_support_t & ts)
{
  DataCursor cursor(dest);
  CDRWriter().serialize_top_level(&cursor, data, *struct_type_from_rosidl(&ts));
}

size_t get_serialized_size(
  const cdds_request_wrapper_t & request, const rosidl_message_type_support_t & ts)
{
  SizeCursor cursor;
  CDRWriter().serialize_top_level(&cursor, request, *struct_type_from_rosidl(&ts));
  return cursor.offset();
}

void serialize(
  void * dest, const cdds_request_wrapper_t & request,
  const rosidl_message_type_support_t & ts)
{
  DataCursor cursor(dest);
  CDRWriter().serialize_top_level(&cursor, request, *struct_type_from_rosidl(&ts));
}
}  // namespace rmw_cyclonedds_cpp
