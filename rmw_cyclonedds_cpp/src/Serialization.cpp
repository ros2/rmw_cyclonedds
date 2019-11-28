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
  void rebase(ptrdiff_t relative_origin) override {m_offset += relative_origin;}
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

public:
  CDRWriter()
  : eversion{EncodingVersion::CDR_Legacy}, max_align{8} {}

  void serialize_top_level(
    CDRCursor & cursor, const void * data, const AnyStructValueType & support)
  {
    put_rtps_header(cursor);

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor.rebase(-4);
    }

    if (support.n_members() == 0 && eversion == EncodingVersion::CDR_Legacy) {
      char dummy = '\0';
      cursor.put_bytes(&dummy, 1);
    } else {
      serialize(cursor, data, support);
    }

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor.rebase(+4);
    }
  }

  void serialize_top_level(
    CDRCursor & cursor, const cdds_request_wrapper_t & request, const AnyStructValueType & support)
  {
    put_rtps_header(cursor);
    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor.rebase(-4);
    }
    cursor.put_bytes(&request.header.guid, sizeof(request.header.guid));
    cursor.put_bytes(&request.header.seq, sizeof(request.header.seq));
    serialize(cursor, request.data, support);

    if (eversion == EncodingVersion::CDR_Legacy) {
      cursor.rebase(+4);
    }
  }

protected:
  void put_rtps_header(CDRCursor & c)
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
    c.put_bytes(rtps_header.data(), rtps_header.size());
  }

  void serialize_u32(CDRCursor & cursor, size_t value)
  {
    assert(value <= std::numeric_limits<uint32_t>::max());
    cursor.align(4);
    cursor.put_bytes(&value, 4);
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

  size_t get_cdr_alignof_primitive(ROSIDL_TypeKind vt)
  {
    /// return 0 if the value type is not primitive
    /// else returns the number of bytes it should align to
    return std::min(get_cdr_size_of_primitive(vt), max_align);
  }

  bool is_value_type_trivially_serialized(const AnyValueType & v)
  {
    switch (v.type_kind()) {
      case ROSIDL_TypeKind::FLOAT:
      case ROSIDL_TypeKind::DOUBLE:
      case ROSIDL_TypeKind::LONG_DOUBLE:
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
        return v.sizeof_type() == get_cdr_size_of_primitive(v.type_kind());
      case ROSIDL_TypeKind::STRING:
      case ROSIDL_TypeKind::WSTRING:
      case ROSIDL_TypeKind::MESSAGE:
        return false;
    }
  }

  void serialize(CDRCursor & cursor, const void * data, const PrimitiveValueType & value_type)
  {
    cursor.align(get_cdr_alignof_primitive(value_type.type_kind()));
    // todo: correct for endianness if the serialized size is different than the in-memory size
    cursor.put_bytes(data, get_cdr_size_of_primitive(value_type.type_kind()));
  }

  void serialize(CDRCursor & cursor, const void * data, const AnyU8StringValueType & value_type)
  {
    auto str = value_type.data(data);
    serialize_u32(cursor, str.size() + 1);
    cursor.put_bytes(str.data(), str.size_bytes());
    char terminator = '\0';
    cursor.put_bytes(&terminator, 1);
  }

  void serialize(CDRCursor & cursor, const void * data, const AnyU16StringValueType & value_type)
  {
    auto str = value_type.data(data);
    if (eversion == EncodingVersion::CDR_Legacy) {
      serialize_u32(cursor, str.size());
      if (cursor.ignores_data()) {
        cursor.advance(sizeof(wchar_t) * str.size());
      } else {
        for (wchar_t c : str) {
          cursor.put_bytes(&c, sizeof(wchar_t));
        }
      }
    } else {
      serialize_u32(cursor, str.size_bytes());
      cursor.put_bytes(str.data(), str.size_bytes());
    }
  }

  void serialize(CDRCursor & cursor, const void * data, const AnyValueType & value_type)
  {
    if (auto s = dynamic_cast<const PrimitiveValueType *>(&value_type)) {
      return serialize(cursor, data, *s);
    }
    if (auto s = dynamic_cast<const AnyU8StringValueType *>(&value_type)) {
      return serialize(cursor, data, *s);
    }
    if (auto s = dynamic_cast<const AnyU16StringValueType *>(&value_type)) {
      return serialize(cursor, data, *s);
    }
    if (auto s = dynamic_cast<const AnyStructValueType *>(&value_type)) {
      return serialize(cursor, data, *s);
    }
    throw std::logic_error("Unhandled case");
  }

  void serialize_many(
    CDRCursor & cursor, const UntypedSpan & span, size_t count, const AnyValueType & vt)
  {
    size_t maybe_value_size = get_cdr_size_of_primitive(vt.type_kind());
    if (cursor.ignores_data() && maybe_value_size) {
      cursor.align(get_cdr_alignof_primitive(vt.type_kind()));
      cursor.advance(count * maybe_value_size);
      return;
    }
    if (is_value_type_trivially_serialized(vt)) {
      cursor.align(get_cdr_alignof_primitive(vt.type_kind()));
      cursor.put_bytes(span.data(), span.size_bytes());
      return;
    }
    // slow fallback:
    for (size_t i = 0; i < count; i++) {
      serialize(cursor, byte_offset(span.data(), i * vt.sizeof_type()), vt);
    }
    return;
  }

  void serialize(
    CDRCursor & cursor, const void * struct_data, const AnyStructValueType & struct_info)
  {
    for (size_t i = 0; i < struct_info.n_members(); i++) {
      auto member_info = struct_info.get_member(i);
      auto p_member_info = member_info.get();
      auto value_type = member_info->get_value_type();
      if (auto c = dynamic_cast<const SingleValueMember *>(p_member_info)) {
        serialize(cursor, c->get_member_data(struct_data), *value_type);
        return;
      } else if (auto c = dynamic_cast<const ArrayValueMember *>(p_member_info)) {
        serialize_many(cursor, c->array_contents(struct_data), c->array_size(), *value_type);
        return;
      } else if (auto c = dynamic_cast<const SpanSequenceValueMember *>(p_member_info)) {
        serialize_u32(cursor, c->sequence_size(struct_data));
        serialize_many(
          cursor, c->sequence_contents(struct_data), c->sequence_size(struct_data), *value_type);
        return;
      } else if (auto c = dynamic_cast<const ROSIDLCPP_BoolSequenceMember *>(p_member_info)) {
        auto & v = c->get_bool_vector(struct_data);
        size_t count = v.size();
        size_t prim_size = get_cdr_size_of_primitive(c->value_type->type_kind());
        serialize_u32(cursor, v.size());
        if (cursor.ignores_data() && prim_size) {
          cursor.advance(count * prim_size);
          return;
        }
        for (uint8_t b : c->get_bool_vector(struct_data)) {
          cursor.put_bytes(&b, 1);
          return;
        }
      }
    }
  }
};

void serialize(
  void * dest, size_t dest_size, const void * data, const rosidl_message_type_support_t & ts)
{
  DataCursor cursor(dest);
  CDRWriter().serialize_top_level(cursor, data, *from_rosidl(&ts));
  auto o = cursor.offset();
  assert(o == dest_size);
}

size_t get_serialized_size(const void * data, const rosidl_message_type_support_t & ts)
{
  SizeCursor cursor;
  CDRWriter().serialize_top_level(cursor, data, *from_rosidl(&ts));
  return cursor.offset();
}

size_t get_serialized_size(
  const cdds_request_wrapper_t & request, const rosidl_message_type_support_t & ts)
{
  SizeCursor cursor;
  CDRWriter().serialize_top_level(cursor, request, *from_rosidl(&ts));
  return cursor.offset();
}

void serialize(
  void * dest, size_t dest_size, const cdds_request_wrapper_t & request,
  const rosidl_message_type_support_t & ts)
{
  DataCursor cursor(dest);
  CDRWriter().serialize_top_level(cursor, request, *from_rosidl(&ts));
  assert(cursor.offset() == dest_size);
}
}  // namespace rmw_cyclonedds_cpp
