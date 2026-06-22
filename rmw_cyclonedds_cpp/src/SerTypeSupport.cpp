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

#include "SerTypeSupport.hpp"
#include "SerDesInternals.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "rosidl_runtime_c/string_functions.h"
#include "rosidl_runtime_c/u16string_functions.h"
#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"

namespace rmw_cyclonedds_cpp
{

// ---------------------------------------------------------------------------
// String accessors for C and C++ string types
// ---------------------------------------------------------------------------

static const char * c_string_data(const void * s)
{
  return static_cast<const rosidl_runtime_c__String *>(s)->data;
}
static size_t c_string_size(const void * s)
{
  return static_cast<const rosidl_runtime_c__String *>(s)->size;
}
static const char16_t * c_wstring_data(const void * s)
{
  return reinterpret_cast<const char16_t *>(
    static_cast<const rosidl_runtime_c__U16String *>(s)->data);
}
static size_t c_wstring_size(const void * s)
{
  return static_cast<const rosidl_runtime_c__U16String *>(s)->size;
}
static const char * cpp_string_data(const void * s)
{
  return static_cast<const std::string *>(s)->data();
}
static size_t cpp_string_size(const void * s)
{
  return static_cast<const std::string *>(s)->size();
}
static const char16_t * cpp_wstring_data(const void * s)
{
  return static_cast<const std::u16string *>(s)->data();
}
static size_t cpp_wstring_size(const void * s)
{
  return static_cast<const std::u16string *>(s)->size();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

template<typename T>
static const SerAnyType * alloc_ser(SerTypeStorage & store, T value)
{
  assert(store.nodes.size() < store.nodes.capacity());
  store.nodes.emplace_back(std::move(value));
  return &store.nodes.back();
}

// Forward declarations
static SerStruct make_ser_struct_c(
  const rosidl_typesupport_introspection_c__MessageMembers * impl,
  SerTypeStorage & store);
static SerStruct make_ser_struct_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl,
  SerTypeStorage & store);
static void ser_stamp_trivial_struct(SerStruct & s);

// ---------------------------------------------------------------------------
// Element builders — C
// ---------------------------------------------------------------------------

static const SerAnyType * make_ser_element_c(
  const rosidl_typesupport_introspection_c__MessageMember & m,
  SerTypeStorage & store)
{
  switch (ROSIDL_TypeKind(m.type_id_)) {
    case ROSIDL_TypeKind::FLOAT:
    case ROSIDL_TypeKind::DOUBLE:
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
      return alloc_ser(store, make_ser_primitive(ROSIDL_TypeKind(m.type_id_)));
    case ROSIDL_TypeKind::STRING:
      return alloc_ser(store, SerString{c_string_data, c_string_size});
    case ROSIDL_TypeKind::WSTRING:
      return alloc_ser(store, SerWString{c_wstring_data, c_wstring_size});
    case ROSIDL_TypeKind::MESSAGE: {
      auto sub = make_ser_struct_c(
        static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(
          m.members_->data), store);
      return alloc_ser(store, std::move(sub));
    }
    default:
      throw std::runtime_error("make_ser_element_c: unknown type kind");
  }
}

static SerStruct make_ser_struct_c(
  const rosidl_typesupport_introspection_c__MessageMembers * impl,
  SerTypeStorage & store)
{
  SerStruct st;
  st.has_keys = false;
  st.is_self_contained = true;
  st.native_size = impl->size_of_;

  struct LocalMember { const SerAnyType * type; bool is_key; uint32_t offset; };
  std::vector<LocalMember> local;
  local.reserve(impl->member_count_);

  for (uint32_t i = 0; i < impl->member_count_; ++i) {
    const auto & m = impl->members_[i];
    const ROSIDL_TypeKind kind = ROSIDL_TypeKind(m.type_id_);

    const SerAnyType * elem = make_ser_element_c(m, store);

    const SerAnyType * member_type;
    if (!m.is_array_) {
      member_type = elem;
    } else if (m.array_size_ != 0 && !m.is_upper_bound_) {
      // Fixed array
      size_t nat_stride;
      if (kind == ROSIDL_TypeKind::MESSAGE) {
        nat_stride = std::get<SerStruct>(*elem).native_size;
      } else if (kind == ROSIDL_TypeKind::STRING) {
        nat_stride = sizeof(rosidl_runtime_c__String);
      } else if (kind == ROSIDL_TypeKind::WSTRING) {
        nat_stride = sizeof(rosidl_runtime_c__U16String);
      } else {
        nat_stride = c_elem_native_stride(kind);
      }
      member_type = alloc_ser(store, SerArray{elem, m.array_size_, nat_stride});
    } else if (kind == ROSIDL_TypeKind::BOOLEAN) {
      // C bool sequence: { bool* data; size_t size; size_t capacity }
      member_type = alloc_ser(store, SerCBoolVector{});
    } else {
      // rosidl C dynamic sequence: { T* data; size_t size; size_t capacity }
      size_t nat_stride;
      if (kind == ROSIDL_TypeKind::MESSAGE) {
        nat_stride = std::get<SerStruct>(*elem).native_size;
      } else if (kind == ROSIDL_TypeKind::STRING) {
        nat_stride = sizeof(rosidl_runtime_c__String);
      } else if (kind == ROSIDL_TypeKind::WSTRING) {
        nat_stride = sizeof(rosidl_runtime_c__U16String);
      } else {
        nat_stride = c_elem_native_stride(kind);
      }
      member_type = alloc_ser(store, SerCSequence{elem, nat_stride});
    }

    if (m.is_key_) {
      st.has_keys = true;
    }
    local.push_back({member_type, m.is_key_, m.offset_});
  }
  const size_t member_start = store.all_members.size();
  for (auto & lm : local) {
    assert(store.all_members.size() < store.all_members.capacity());
    store.all_members.push_back(SerMember{lm.type, lm.is_key, lm.offset});
  }
  st.members = std::span<const SerMember>(
    store.all_members.data() + member_start,
    store.all_members.size() - member_start);
  return st;
}

// ---------------------------------------------------------------------------
// Element builders — CPP
// ---------------------------------------------------------------------------

static const SerAnyType * make_ser_element_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMember & m,
  SerTypeStorage & store)
{
  switch (ROSIDL_TypeKind(m.type_id_)) {
    case ROSIDL_TypeKind::FLOAT:
    case ROSIDL_TypeKind::DOUBLE:
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
      return alloc_ser(store, make_ser_primitive(ROSIDL_TypeKind(m.type_id_)));
    case ROSIDL_TypeKind::STRING:
      return alloc_ser(store, SerString{cpp_string_data, cpp_string_size});
    case ROSIDL_TypeKind::WSTRING:
      return alloc_ser(store, SerWString{cpp_wstring_data, cpp_wstring_size});
    case ROSIDL_TypeKind::MESSAGE: {
      auto sub = make_ser_struct_cpp(
        static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(
          m.members_->data), store);
      return alloc_ser(store, std::move(sub));
    }
    default:
      throw std::runtime_error("make_ser_element_cpp: unknown type kind");
  }
}

static SerStruct make_ser_struct_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl,
  SerTypeStorage & store)
{
  SerStruct st;
  st.has_keys = false;
  st.is_self_contained = true;
  st.native_size = impl->size_of_;

  struct LocalMember { const SerAnyType * type; bool is_key; uint32_t offset; };
  std::vector<LocalMember> local;
  local.reserve(impl->member_count_);

  for (uint32_t i = 0; i < impl->member_count_; ++i) {
    const auto & m = impl->members_[i];
    const ROSIDL_TypeKind kind = ROSIDL_TypeKind(m.type_id_);

    const SerAnyType * elem = make_ser_element_cpp(m, store);

    const SerAnyType * member_type;
    if (!m.is_array_) {
      member_type = elem;
    } else if (m.array_size_ != 0 && !m.is_upper_bound_) {
      // Fixed-size array
      size_t nat_stride;
      if (kind == ROSIDL_TypeKind::MESSAGE) {
        nat_stride = std::get<SerStruct>(*elem).native_size;
      } else if (kind == ROSIDL_TypeKind::STRING) {
        nat_stride = sizeof(std::string);
      } else if (kind == ROSIDL_TypeKind::WSTRING) {
        nat_stride = sizeof(std::u16string);
      } else {
        nat_stride = native_size_of_kind(kind);
      }
      member_type = alloc_ser(store, SerArray{elem, m.array_size_, nat_stride});
    } else if (kind == ROSIDL_TypeKind::BOOLEAN) {
      // C++ std::vector<bool>: bit-packed storage
      member_type = alloc_ser(store, SerCppBoolVector{m.size_function, m.fetch_function});
    } else {
      // C++ std::vector<T>: { T* begin; T* end; T* cap }
      size_t nat_stride;
      if (kind == ROSIDL_TypeKind::MESSAGE) {
        nat_stride = std::get<SerStruct>(*elem).native_size;
      } else if (kind == ROSIDL_TypeKind::STRING) {
        nat_stride = sizeof(std::string);
      } else if (kind == ROSIDL_TypeKind::WSTRING) {
        nat_stride = sizeof(std::u16string);
      } else {
        nat_stride = native_size_of_kind(kind);
      }
      member_type = alloc_ser(store, SerCppSequence{elem, nat_stride});
    }

    if (m.is_key_) {
      st.has_keys = true;
    }
    local.push_back({member_type, m.is_key_, m.offset_});
  }
  const size_t member_start = store.all_members.size();
  for (auto & lm : local) {
    assert(store.all_members.size() < store.all_members.capacity());
    store.all_members.push_back(SerMember{lm.type, lm.is_key, lm.offset});
  }
  st.members = std::span<const SerMember>(
    store.all_members.data() + member_start,
    store.all_members.size() - member_start);
  return st;
}

// ---------------------------------------------------------------------------
// Post-build stamp pass: fill trivial_at_align and fixed_cdr_stride
// ---------------------------------------------------------------------------

static TrivialArray ser_trivial_of(const SerAnyType & t);
static void ser_stamp_trivial(SerAnyType & t);

// Simulate walking a struct from entry offset `a` and check all members are trivial.
// `a` is 0..7. Returns true if entire struct is a single memcpy from that alignment.
static bool struct_trivial_at(const SerStruct & s, size_t a)
{
  if (host_needs_bswap()) {
    return false;
  }
  size_t cursor = a;
  for (const auto & member : s.members) {
    const SerAnyType & t = *member.type;
    // Check trivial_at_align for this member at current cursor alignment
    const TrivialArray & ta = ser_trivial_of(t);
    if (!ta[cursor % kSerMaxAlign]) {
      return false;
    }
    // Advance cursor by CDR size of this member (only valid if fixed)
    size_t member_cdr_size = std::visit(
      [](const auto & v) -> size_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, SerPrimitive>) {
          return v.cdr_size;
        } else if constexpr (std::is_same_v<T, SerStruct>) {
          // only reachable if v itself is trivial
          return v.native_size;
        } else if constexpr (std::is_same_v<T, SerArray>) {
          return (v.fixed_cdr_stride > 0) ? v.count * v.fixed_cdr_stride : 0;
        }
        return 0;
      }, t);
    if (member_cdr_size == 0) {
      return false;
    }
    cursor += member_cdr_size;
  }
  // Also verify CDR struct size == native struct size (no layout gaps)
  return (cursor - a) == s.native_size;
}

// Retrieve trivial_at_align from any SerAnyType node (already stamped).
static TrivialArray ser_trivial_of(const SerAnyType & t)
{
  return std::visit(
    [](const auto & v) -> TrivialArray {
      using T = std::decay_t<decltype(v)>;
      if constexpr (
        std::is_same_v<T, SerPrimitive> ||
        std::is_same_v<T, SerStruct> ||
        std::is_same_v<T, SerArray> ||
        std::is_same_v<T, SerCSequence> ||
        std::is_same_v<T, SerCppSequence>)
      {
        return v.trivial_at_align;
      }
      return TrivialArray{};  // SerString/SerWString/SerCBoolVector/SerCppBoolVector: all false
    }, t);
}

static size_t ser_fixed_cdr_stride(const SerAnyType & t)
{
  return std::visit(
    [](const auto & v) -> size_t {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SerPrimitive>) {
        return v.cdr_size;
      } else if constexpr (std::is_same_v<T, SerStruct>) {
        // Fixed if trivial at alignment 0 (implies uniform CDR == native)
        return v.trivial_at_align[0] ? v.native_size : 0;
      } else if constexpr (std::is_same_v<T, SerArray>) {
        return (v.fixed_cdr_stride > 0) ? v.count * v.fixed_cdr_stride : 0;
      }
      return 0;
    }, t);
}

static void ser_stamp_trivial(SerAnyType & t)
{
  std::visit(
    [](auto & v) {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SerStruct>) {
        ser_stamp_trivial_struct(v);
      } else if constexpr (std::is_same_v<T, SerArray>) {
        // element already stamped (owned by same parent struct, processed first)
        v.fixed_cdr_stride = ser_fixed_cdr_stride(*v.element);
        // Array trivial at alignment a: element trivial at a AND at (a + stride) % 8
        // (the "CLEVERNESS" condition from trivsercache: same alignment repeats)
        for (size_t a = 0; a < kSerMaxAlign; ++a) {
          if (v.fixed_cdr_stride == 0) {
            v.trivial_at_align[a] = false;
          } else {
            const TrivialArray & et = ser_trivial_of(*v.element);
            v.trivial_at_align[a] =
              et[a % kSerMaxAlign] &&
              et[(a + v.fixed_cdr_stride) % kSerMaxAlign];
          }
        }
      } else if constexpr (
        std::is_same_v<T, SerCSequence> ||
        std::is_same_v<T, SerCppSequence>)
      {
        v.fixed_cdr_stride = ser_fixed_cdr_stride(*v.element);
        // trivial_at_align[a]: element is trivially serializable when the cursor
        // is at offset 'a' AFTER the 4-byte length prefix has been written.
        for (size_t a = 0; a < kSerMaxAlign; ++a) {
          if (v.fixed_cdr_stride == 0) {
            v.trivial_at_align[a] = false;
          } else {
            const TrivialArray & et = ser_trivial_of(*v.element);
            v.trivial_at_align[a] =
              et[a] &&
              et[(a + v.fixed_cdr_stride) % kSerMaxAlign];
          }
        }
      }
      // SerPrimitive trivial_at_align already set at construction time.
      // SerString/SerWString/SerCBoolVector/SerCppBoolVector: all-false (default).
    }, t);
}

static void ser_stamp_trivial_struct(SerStruct & s)
{
  // Now compute trivial_at_align for this struct.
  for (size_t a = 0; a < kSerMaxAlign; ++a) {
    s.trivial_at_align[a] = struct_trivial_at(s, a);
  }
}

// ---------------------------------------------------------------------------
// Public builder
// ---------------------------------------------------------------------------

std::pair<SerStruct, SerTypeStorage> make_ser_struct(MessageMembersVariant members)
{
  SerTypeStorage store;
  // Count pass: determine how many nodes and members we need, then reserve.
  auto counts = std::visit(
    [](const auto * m) { return count_type_tree(m); }, members);
  store.nodes.reserve(counts.nodes);
  store.all_members.reserve(counts.members);

  auto root = std::visit(
    [&store](const auto * m) -> SerStruct {
      using T = std::decay_t<decltype(*m)>;
      if constexpr (std::is_same_v<T, MetaMessage<TypeGenerator::ROSIDL_C>>) {
        return make_ser_struct_c(m, store);
      } else {
        return make_ser_struct_cpp(m, store);
      }
    }, members);
  // Stamp pass: iterate all nodes in allocation order (children before parents).
  for (auto & node : store.nodes) {
    ser_stamp_trivial(node);
  }
  ser_stamp_trivial_struct(root);
  return {std::move(root), std::move(store)};
}

// ---------------------------------------------------------------------------
// SerializeCursor — inline write cursor, no virtual dispatch
// ---------------------------------------------------------------------------

struct SerializeCursor
{
  char * pos;
  const char * origin;

  explicit SerializeCursor(void * dst)
  : pos(static_cast<char *>(dst)), origin(static_cast<char *>(dst)) {}

  size_t offset() const {return static_cast<size_t>(pos - origin);}

  void align(size_t n)
  {
    size_t mask = n - 1;
    size_t rem = offset() & mask;
    if (rem) {
      size_t pad = n - rem;
      std::memset(pos, 0, pad);
      pos += pad;
    }
  }

  void put_bytes(const void * src, size_t n)
  {
    std::memcpy(pos, src, n);
    pos += n;
  }

  void put_zeros(size_t n)
  {
    std::memset(pos, 0, n);
    pos += n;
  }

  void rebase(ptrdiff_t delta) {origin += delta;}
};

// Write one primitive value, handling bswap and native_size != cdr_size.
// Bswap is a compile-time constant: true on BE hosts or when native != cdr size.
template<bool Bswap>
static void ser_write_primitive(
  SerializeCursor & c, const void * src, const SerPrimitive & p)
{
  c.align(p.cdr_align);
  if constexpr (!Bswap) {
    c.put_bytes(src, p.cdr_size);
  } else {
    // Advance to make room, then bswap into the just-written slot.
    char * dst_slot = c.pos;
    c.put_zeros(p.cdr_size);
    // Offset src by (native_size - cdr_size) on BE hosts (same as CDRWriter)
    const void * src_adj = (p.native_size > p.cdr_size)
      ? static_cast<const char *>(src) + (p.native_size - p.cdr_size) : src;
    switch (p.cdr_size) {
      case 1: bswap_n<1>(dst_slot, src_adj); break;
      case 2: bswap_n<2>(dst_slot, src_adj); break;
      case 4: bswap_n<4>(dst_slot, src_adj); break;
      case 8: bswap_n<8>(dst_slot, src_adj); break;
      default: break;
    }
  }
}

// Forward declarations
template<bool Bswap>
static void ser_write(SerializeCursor & c, const void * data,
  const SerAnyType & t, SampleOrKey what);
template<bool Bswap>
static void ser_write_struct(SerializeCursor & c, const void * data,
  const SerStruct & s, SampleOrKey what);
template<bool Bswap>
static void ser_write_many(SerializeCursor & c, const void * base,
  size_t native_stride, size_t count, const SerAnyType & elem, SampleOrKey what);

template<bool Bswap>
static void ser_write_many(
  SerializeCursor & c, const void * base,
  size_t native_stride, size_t count, const SerAnyType & elem, SampleOrKey what)
{
  for (size_t i = 0; i < count; ++i) {
    const void * p = static_cast<const char *>(base) + i * native_stride;
    ser_write<Bswap>(c, p, elem, what);
  }
}

template<bool Bswap>
static void ser_write(
  SerializeCursor & c, const void * data,
  const SerAnyType & t, SampleOrKey what)
{
  std::visit(
    [&](const auto & v) {
      using T = std::decay_t<decltype(v)>;

      if constexpr (std::is_same_v<T, SerPrimitive>) {
        ser_write_primitive<Bswap>(c, data, v);

      } else if constexpr (std::is_same_v<T, SerString>) {
        const char * str_data = v.get_data(data);
        size_t str_size = v.get_size(data);
        uint32_t len = static_cast<uint32_t>(str_size + 1);
        c.align(4);
        c.put_bytes(&len, 4);
        c.put_bytes(str_data, str_size);
        char nul = '\0';
        c.put_bytes(&nul, 1);

      } else if constexpr (std::is_same_v<T, SerWString>) {
        const char16_t * str_data = v.get_data(data);
        size_t str_size = v.get_size(data);
        uint32_t byte_len = static_cast<uint32_t>(str_size * 2);
        c.align(4);
        c.put_bytes(&byte_len, 4);
        if constexpr (!Bswap) {
          c.put_bytes(str_data, byte_len);
        } else {
          for (size_t i = 0; i < str_size; ++i) {
            uint16_t ch = static_cast<uint16_t>(str_data[i]);
            bswap_n<2>(c.pos, &ch);
            c.pos += 2;
          }
        }

      } else if constexpr (std::is_same_v<T, SerCBoolVector>) {
        // C bool sequence: { bool* data; size_t size; size_t cap }
        struct SeqLayout { const void * ptr; size_t size; size_t cap; };
        auto * seq = static_cast<const SeqLayout *>(data);
        uint32_t cnt = static_cast<uint32_t>(seq->size);
        c.align(4);
        c.put_bytes(&cnt, 4);
        c.put_bytes(seq->ptr, seq->size);

      } else if constexpr (std::is_same_v<T, SerCppBoolVector>) {
        // C++ std::vector<bool>: use introspection callbacks
        uint32_t cnt = static_cast<uint32_t>(v.size_function(data));
        c.align(4);
        c.put_bytes(&cnt, 4);
        for (uint32_t bi = 0; bi < cnt; ++bi) {
          uint8_t val = 0;
          v.fetch_function(data, bi, &val);
          c.put_bytes(&val, 1);
        }

      } else if constexpr (std::is_same_v<T, SerArray>) {
        if (v.trivial_at_align[c.offset() % kSerMaxAlign]) {
          // Bulk memcpy: all elements trivially serialized from this alignment
          c.put_bytes(data, v.count * v.native_elem_stride);
        } else {
          ser_write_many<Bswap>(c, data, v.native_elem_stride, v.count, *v.element, what);
        }

      } else if constexpr (std::is_same_v<T, SerCSequence>) {
        // rosidl C sequence: { T* data; size_t size; size_t capacity }
        struct SeqLayout { const void * ptr; size_t size; size_t cap; };
        auto * seq = static_cast<const SeqLayout *>(data);
        uint32_t cnt32 = static_cast<uint32_t>(seq->size);
        c.align(4);
        c.put_bytes(&cnt32, 4);
        if (seq->size == 0) {
          return;
        }
        if (v.trivial_at_align[c.offset() % kSerMaxAlign]) {
          c.put_bytes(seq->ptr, seq->size * v.native_elem_stride);
        } else {
          ser_write_many<Bswap>(c, seq->ptr, v.native_elem_stride, seq->size, *v.element, what);
        }

      } else if constexpr (std::is_same_v<T, SerCppSequence>) {
        // C++ std::vector<T>: { T* begin; T* end; T* cap }
        struct VecLayout { const char * begin; const char * end; const char * cap; };
        auto * vec = static_cast<const VecLayout *>(data);
        size_t count = static_cast<size_t>(vec->end - vec->begin) / v.native_elem_stride;
        uint32_t cnt32 = static_cast<uint32_t>(count);
        c.align(4);
        c.put_bytes(&cnt32, 4);
        if (count == 0) {
          return;
        }
        if (v.trivial_at_align[c.offset() % kSerMaxAlign]) {
          c.put_bytes(vec->begin, count * v.native_elem_stride);
        } else {
          ser_write_many<Bswap>(c, vec->begin, v.native_elem_stride, count, *v.element, what);
        }

      } else if constexpr (std::is_same_v<T, SerStruct>) {
        ser_write_struct<Bswap>(c, data, v, what);
      }
    }, t);
}

template<bool Bswap>
static void ser_write_struct(
  SerializeCursor & c, const void * data,
  const SerStruct & s, SampleOrKey what)
{
  if (what == SampleOrKey::Sample && s.trivial_at_align[c.offset() % kSerMaxAlign]) {
    c.put_bytes(data, s.native_size);
    return;
  }
  bool all_fields = (what == SampleOrKey::Sample || !s.has_keys);
  for (const auto & member : s.members) {
    if (all_fields || member.is_key) {
      const void * field = static_cast<const char *>(data) + member.native_offset;
      ser_write<Bswap>(c, field, *member.type, what);
    }
  }
}

// ---------------------------------------------------------------------------
// CDRSerializer implementation
// ---------------------------------------------------------------------------

CDRSerializer::CDRSerializer(MessageMembersVariant members, SampleOrRequest variant)
: m_storage{},
  m_root{},
  m_sizer(members, variant),
  m_variant(variant),
  m_type_generator(
    std::holds_alternative<const MetaMessage<TypeGenerator::ROSIDL_C> *>(members)
    ? TypeGenerator::ROSIDL_C : TypeGenerator::ROSIDL_Cpp)
{
  auto [root, storage] = make_ser_struct(members);
  m_storage = std::move(storage);
  m_root = std::move(root);
}

void CDRSerializer::serialize(void * dest, const void * data, SampleOrKey what) const
{
  SerializeCursor c(dest);

  // 4-byte RTPS encoding header: {0x00, 0x01 (LE) or 0x00 (BE), 0x00, 0x00}
  const unsigned char header[4] = {0, static_cast<unsigned char>(host_needs_bswap() ? 0 : 1), 0, 0};
  c.put_bytes(header, 4);
  c.rebase(+4);

  if (what == SampleOrKey::Sample && m_variant == SampleOrRequest::Request) {
    auto * req = static_cast<const cdds_request_wrapper_t *>(data);
    if constexpr (!host_needs_bswap()) {
      c.put_bytes(&req->header.guid, sizeof(req->header.guid));
      c.put_bytes(&req->header.seq,  sizeof(req->header.seq));
    } else {
      bswap_n<8>(c.pos, &req->header.guid); c.pos += 8;
      bswap_n<8>(c.pos, &req->header.seq);  c.pos += 8;
    }
    data = req->data;
  }

  if (what == SampleOrKey::Sample || m_root.has_keys) {
    ser_write_struct<host_needs_bswap()>(c, data, m_root, what);
  }
  c.rebase(-4);
}

size_t CDRSerializer::get_serialized_size(const void * data, SampleOrKey what) const
{
  return m_sizer.get_serialized_size(data, what);
}

size_t CDRSerializer::get_serialized_size_estimate(const void * data, SampleOrKey what) const
{
  return m_sizer.get_serialized_size_estimate(data, what);
}

size_t CDRSerializer::get_min_serialized_size(SampleOrKey what) const
{
  return m_sizer.get_min_serialized_size(what);
}

size_t CDRSerializer::get_max_serialized_size(SampleOrKey what) const
{
  return m_sizer.get_max_serialized_size(what);
}

TypeGenerator CDRSerializer::type_generator() const
{
  return m_type_generator;
}

}  // namespace rmw_cyclonedds_cpp
