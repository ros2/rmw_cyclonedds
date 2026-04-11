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

#include "DeserTypeSupport.hpp"
#include "SerDesInternals.hpp"

#include <bit>
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

#include "TypeSupport2.hpp"   // for cdds_request_wrapper_t

namespace rmw_cyclonedds_cpp
{

// ---------------------------------------------------------------------------
// String assign helpers (write-side)
// ---------------------------------------------------------------------------

static void c_string_assign(void * s, const char * src, size_t n)
{
  rosidl_runtime_c__String__assignn(
    static_cast<rosidl_runtime_c__String *>(s), src, n);
}

static void c_wstring_assign(void * s, const char16_t * src, size_t n)
{
  rosidl_runtime_c__U16String__assignn(
    static_cast<rosidl_runtime_c__U16String *>(s),
    reinterpret_cast<const uint16_t *>(src), n);
}

static void cpp_string_assign(void * s, const char * src, size_t n)
{
  *static_cast<std::string *>(s) = std::string(src, n);
}

static void cpp_wstring_assign(void * s, const char16_t * src, size_t n)
{
  *static_cast<std::u16string *>(s) = std::u16string(src, n);
}

// ---------------------------------------------------------------------------
// Sequence resize + mut_contents helpers
// ---------------------------------------------------------------------------

// C rosidl sequence layout: {T* data, size_t size, size_t capacity}
struct RosidlCSeqLayout { void * data; size_t size; size_t capacity; };

static void * c_seq_mut_contents(void * seq)
{
  return static_cast<RosidlCSeqLayout *>(seq)->data;
}

// std::vector<T> layout: { T* _M_start, T* _M_finish, T* _M_end_of_storage }
static void * cpp_vec_mut_contents(void * seq)
{
  return *static_cast<void **>(seq);  // first field is T* begin
}

// ---------------------------------------------------------------------------
// Static resize/assign helpers for raw-function-pointer Deser types
// ---------------------------------------------------------------------------

// C bool seq assign: write into data[idx]
static void c_bool_seq_assign(void * seq, size_t idx, uint8_t val)
{
  auto * layout = static_cast<RosidlCSeqLayout *>(seq);
  static_cast<uint8_t *>(layout->data)[idx] = val;
}

// ---------------------------------------------------------------------------
// Deser element builders — forward declarations
// ---------------------------------------------------------------------------

static DeserStruct make_deser_struct_c(
  const rosidl_typesupport_introspection_c__MessageMembers * impl,
  DeserTypeStorage & store);
static DeserStruct make_deser_struct_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl,
  DeserTypeStorage & store);
static void deser_stamp_trivial_struct(DeserStruct & s);

// ---------------------------------------------------------------------------
// alloc_deser helper
// ---------------------------------------------------------------------------

template<typename T>
static const DeserAnyType * alloc_deser(DeserTypeStorage & store, T value)
{
  assert(store.nodes.size() < store.nodes.capacity());
  store.nodes.emplace_back(std::move(value));
  return &store.nodes.back();
}

// ---------------------------------------------------------------------------
// Deser stamp pass (mirrors ser_stamp_trivial but for DeserAnyType)
// ---------------------------------------------------------------------------

static TrivialArray deser_trivial_of(const DeserAnyType & t);
static size_t deser_fixed_cdr_stride(const DeserAnyType & t);
static void deser_stamp_trivial(DeserAnyType & t);

static TrivialArray deser_trivial_of(const DeserAnyType & t)
{
  return std::visit(
    [](const auto & v) -> TrivialArray {
      using T = std::decay_t<decltype(v)>;
      if constexpr (
        std::is_same_v<T, SerPrimitive> ||
        std::is_same_v<T, DeserStruct> ||
        std::is_same_v<T, DeserArray> ||
        std::is_same_v<T, DeserCSequence> ||
        std::is_same_v<T, DeserCppSequence>)
      {
        return v.trivial_at_align;
      }
      return TrivialArray{};
    }, t);
}

static size_t deser_fixed_cdr_stride(const DeserAnyType & t)
{
  return std::visit(
    [](const auto & v) -> size_t {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SerPrimitive>) {
        return v.cdr_size;
      } else if constexpr (std::is_same_v<T, DeserStruct>) {
        return v.trivial_at_align[0] ? v.native_size : 0;
      } else if constexpr (std::is_same_v<T, DeserArray>) {
        return (v.fixed_cdr_stride > 0) ? v.count * v.fixed_cdr_stride : 0;
      }
      return 0;
    }, t);
}

static bool deser_struct_trivial_at(const DeserStruct & s, size_t a)
{
  if (host_needs_bswap()) {
    return false;
  }
  size_t cursor = a;
  for (const auto & member : s.members) {
    const TrivialArray & ta = deser_trivial_of(*member.type);
    if (!ta[cursor % kSerMaxAlign]) {
      return false;
    }
    size_t member_cdr_size = std::visit(
      [](const auto & v) -> size_t {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, SerPrimitive>) {
          return v.cdr_size;
        } else if constexpr (std::is_same_v<T, DeserStruct>) {
          return v.native_size;
        } else if constexpr (std::is_same_v<T, DeserArray>) {
          return (v.fixed_cdr_stride > 0) ? v.count * v.fixed_cdr_stride : 0;
        }
        return 0;
      }, *member.type);
    if (member_cdr_size == 0) {
      return false;
    }
    cursor += member_cdr_size;
  }
  return (cursor - a) == s.native_size;
}

static void deser_stamp_trivial(DeserAnyType & t)
{
  std::visit(
    [](auto & v) {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, DeserStruct>) {
        deser_stamp_trivial_struct(v);
      } else if constexpr (std::is_same_v<T, DeserArray>) {
        v.fixed_cdr_stride = deser_fixed_cdr_stride(*v.element);
        for (size_t a = 0; a < kSerMaxAlign; ++a) {
          if (v.fixed_cdr_stride == 0) {
            v.trivial_at_align[a] = false;
          } else {
            const TrivialArray & et = deser_trivial_of(*v.element);
            v.trivial_at_align[a] =
              et[a % kSerMaxAlign] &&
              et[(a + v.fixed_cdr_stride) % kSerMaxAlign];
          }
        }
      } else if constexpr (
        std::is_same_v<T, DeserCSequence> ||
        std::is_same_v<T, DeserCppSequence>)
      {
        v.fixed_cdr_stride = deser_fixed_cdr_stride(*v.element);
        for (size_t a = 0; a < kSerMaxAlign; ++a) {
          if (v.fixed_cdr_stride == 0) {
            v.trivial_at_align[a] = false;
          } else {
            const TrivialArray & et = deser_trivial_of(*v.element);
            v.trivial_at_align[a] =
              et[a] &&
              et[(a + v.fixed_cdr_stride) % kSerMaxAlign];
          }
        }
      }
    }, t);
}

static void deser_stamp_trivial_struct(DeserStruct & s)
{
  for (size_t a = 0; a < kSerMaxAlign; ++a) {
    s.trivial_at_align[a] = deser_struct_trivial_at(s, a);
  }
}

// ---------------------------------------------------------------------------
// Deser element builders — C
// ---------------------------------------------------------------------------

static const DeserAnyType * make_deser_element_c(
  const rosidl_typesupport_introspection_c__MessageMember & m,
  DeserTypeStorage & store)
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
      return alloc_deser(store, make_ser_primitive(ROSIDL_TypeKind(m.type_id_)));
    case ROSIDL_TypeKind::STRING:
      return alloc_deser(store, DeserString{c_string_assign});
    case ROSIDL_TypeKind::WSTRING:
      return alloc_deser(store, DeserWString{c_wstring_assign});
    case ROSIDL_TypeKind::MESSAGE: {
      auto sub = make_deser_struct_c(
        static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(
          m.members_->data), store);
      return alloc_deser(store, std::move(sub));
    }
    default:
      throw std::runtime_error("make_deser_element_c: unknown type kind");
  }
}

static DeserStruct make_deser_struct_c(
  const rosidl_typesupport_introspection_c__MessageMembers * impl,
  DeserTypeStorage & store)
{
  DeserStruct st;
  st.has_keys = false;
  st.native_size = impl->size_of_;

  struct LocalMember { const DeserAnyType * type; bool is_key; uint32_t offset; };
  std::vector<LocalMember> local;
  local.reserve(impl->member_count_);

  for (uint32_t i = 0; i < impl->member_count_; ++i) {
    const auto & m = impl->members_[i];
    const ROSIDL_TypeKind kind = ROSIDL_TypeKind(m.type_id_);

    const DeserAnyType * elem = make_deser_element_c(m, store);

    const DeserAnyType * member_type;
    if (!m.is_array_) {
      member_type = elem;
    } else if (m.array_size_ != 0 && !m.is_upper_bound_) {
      // Fixed array
      size_t nat_stride;
      if (kind == ROSIDL_TypeKind::MESSAGE) {
        nat_stride = std::get<DeserStruct>(*elem).native_size;
      } else if (kind == ROSIDL_TypeKind::STRING) {
        nat_stride = sizeof(rosidl_runtime_c__String);
      } else if (kind == ROSIDL_TypeKind::WSTRING) {
        nat_stride = sizeof(rosidl_runtime_c__U16String);
      } else {
        nat_stride = c_elem_native_stride(kind);
      }
      member_type = alloc_deser(store, DeserArray{elem, m.array_size_, nat_stride});
    } else {
      // Dynamic sequence — C rosidl {data*, size, capacity}
      if (kind == ROSIDL_TypeKind::BOOLEAN) {
        member_type = alloc_deser(store, DeserCBoolVector{m.resize_function, c_bool_seq_assign});
      } else {
        size_t nat_stride;
        if (kind == ROSIDL_TypeKind::MESSAGE) {
          nat_stride = std::get<DeserStruct>(*elem).native_size;
        } else if (kind == ROSIDL_TypeKind::STRING) {
          nat_stride = sizeof(rosidl_runtime_c__String);
        } else if (kind == ROSIDL_TypeKind::WSTRING) {
          nat_stride = sizeof(rosidl_runtime_c__U16String);
        } else {
          nat_stride = c_elem_native_stride(kind);
        }
        member_type = alloc_deser(store, DeserCSequence{
          elem, nat_stride, 0, {}, m.resize_function, c_seq_mut_contents});
      }
    }

    if (m.is_key_) {
      st.has_keys = true;
    }
    local.push_back({member_type, m.is_key_, m.offset_});
  }
  const size_t member_start = store.all_members.size();
  for (auto & lm : local) {
    assert(store.all_members.size() < store.all_members.capacity());
    store.all_members.push_back(DeserMember{lm.type, lm.is_key, lm.offset});
  }
  st.members = std::span<const DeserMember>(
    store.all_members.data() + member_start,
    store.all_members.size() - member_start);
  return st;
}

// ---------------------------------------------------------------------------
// Deser element builders — C++
// ---------------------------------------------------------------------------

static const DeserAnyType * make_deser_element_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMember & m,
  DeserTypeStorage & store)
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
      return alloc_deser(store, make_ser_primitive(ROSIDL_TypeKind(m.type_id_)));
    case ROSIDL_TypeKind::STRING:
      return alloc_deser(store, DeserString{cpp_string_assign});
    case ROSIDL_TypeKind::WSTRING:
      return alloc_deser(store, DeserWString{cpp_wstring_assign});
    case ROSIDL_TypeKind::MESSAGE: {
      auto sub = make_deser_struct_cpp(
        static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(
          m.members_->data), store);
      return alloc_deser(store, std::move(sub));
    }
    default:
      throw std::runtime_error("make_deser_element_cpp: unknown type kind");
  }
}

static DeserStruct make_deser_struct_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl,
  DeserTypeStorage & store)
{
  DeserStruct st;
  st.has_keys = false;
  st.native_size = impl->size_of_;

  struct LocalMember { const DeserAnyType * type; bool is_key; uint32_t offset; };
  std::vector<LocalMember> local;
  local.reserve(impl->member_count_);

  for (uint32_t i = 0; i < impl->member_count_; ++i) {
    const auto & m = impl->members_[i];
    const ROSIDL_TypeKind kind = ROSIDL_TypeKind(m.type_id_);

    const DeserAnyType * elem = make_deser_element_cpp(m, store);

    const DeserAnyType * member_type;
    if (!m.is_array_) {
      member_type = elem;
    } else if (m.array_size_ != 0 && !m.is_upper_bound_) {
      // Fixed array
      size_t nat_stride;
      if (kind == ROSIDL_TypeKind::MESSAGE) {
        nat_stride = std::get<DeserStruct>(*elem).native_size;
      } else if (kind == ROSIDL_TypeKind::STRING) {
        nat_stride = sizeof(std::string);
      } else if (kind == ROSIDL_TypeKind::WSTRING) {
        nat_stride = sizeof(std::u16string);
      } else {
        nat_stride = native_size_of_kind(kind);
      }
      member_type = alloc_deser(store, DeserArray{elem, m.array_size_, nat_stride});
    } else {
      if (kind == ROSIDL_TypeKind::BOOLEAN) {
        // C++ vector<bool>: resize_function(void*,size_t)->void, assign_function(void*,size_t,const void*)
        member_type = alloc_deser(store, DeserCppBoolVector{m.resize_function, m.assign_function});
      } else {
        size_t nat_stride;
        if (kind == ROSIDL_TypeKind::MESSAGE) {
          nat_stride = std::get<DeserStruct>(*elem).native_size;
        } else if (kind == ROSIDL_TypeKind::STRING) {
          nat_stride = sizeof(std::string);
        } else if (kind == ROSIDL_TypeKind::WSTRING) {
          nat_stride = sizeof(std::u16string);
        } else {
          nat_stride = native_size_of_kind(kind);
        }
        member_type = alloc_deser(store, DeserCppSequence{
          elem, nat_stride, 0, {}, m.resize_function, cpp_vec_mut_contents});
      }
    }

    if (m.is_key_) {
      st.has_keys = true;
    }
    local.push_back({member_type, m.is_key_, m.offset_});
  }
  const size_t member_start = store.all_members.size();
  for (auto & lm : local) {
    assert(store.all_members.size() < store.all_members.capacity());
    store.all_members.push_back(DeserMember{lm.type, lm.is_key, lm.offset});
  }
  st.members = std::span<const DeserMember>(
    store.all_members.data() + member_start,
    store.all_members.size() - member_start);
  return st;
}

// ---------------------------------------------------------------------------
// Public builder
// ---------------------------------------------------------------------------

std::pair<DeserStruct, DeserTypeStorage> make_deser_struct(MessageMembersVariant members)
{
  DeserTypeStorage store;
  // Count pass: determine how many nodes and members we need, then reserve.
  auto counts = std::visit(
    [](const auto * m) { return count_type_tree(m); }, members);
  store.nodes.reserve(counts.nodes);
  store.all_members.reserve(counts.members);

  auto root = std::visit(
    [&store](const auto * m) -> DeserStruct {
      using T = std::decay_t<decltype(*m)>;
      if constexpr (std::is_same_v<T, MetaMessage<TypeGenerator::ROSIDL_C>>) {
        return make_deser_struct_c(m, store);
      } else {
        return make_deser_struct_cpp(m, store);
      }
    }, members);
  // Stamp pass: iterate all nodes in allocation order (children before parents).
  for (auto & node : store.nodes) {
    deser_stamp_trivial(node);
  }
  deser_stamp_trivial_struct(root);
  return {std::move(root), std::move(store)};
}

// ---------------------------------------------------------------------------
// DeserializeCursor — inline read cursor, no virtual dispatch
// ---------------------------------------------------------------------------

struct DeserializeCursor
{
  const unsigned char * pos;
  const unsigned char * origin;
  const unsigned char * end;

  DeserializeCursor(const void * data, size_t size)
  : pos(static_cast<const unsigned char *>(data)),
    origin(static_cast<const unsigned char *>(data)),
    end(static_cast<const unsigned char *>(data) + size) {}

  size_t offset() const {return static_cast<size_t>(pos - origin);}

  void rebase(ptrdiff_t delta) {origin += delta;}

  void align(size_t n)
  {
    size_t rem = offset() & (n - 1);
    if (rem) {
      pos += n - rem;
    }
  }

  const unsigned char * advance(size_t n)
  {
    if (static_cast<size_t>(end - pos) < n) {
      throw std::runtime_error("CDR deserialization: truncated input");
    }
    const unsigned char * p = pos;
    pos += n;
    return p;
  }

  void get_bytes(void * dst, size_t n)
  {
    std::memcpy(dst, advance(n), n);
  }
};

// ---------------------------------------------------------------------------
// WriteVecCursor — output cursor for extractkey (appends to std::vector<std::byte>)
// ---------------------------------------------------------------------------

struct WriteVecCursor
{
  std::vector<std::byte> & buf;
  size_t origin_offset = 0;

  explicit WriteVecCursor(std::vector<std::byte> & b)
  : buf(b), origin_offset(0) {}

  size_t offset() const {return buf.size() - origin_offset;}

  void rebase(ptrdiff_t delta) {origin_offset = static_cast<size_t>(
    static_cast<ptrdiff_t>(origin_offset) + delta);}

  void align(size_t n)
  {
    size_t rem = offset() & (n - 1);
    if (rem) {
      buf.insert(buf.end(), n - rem, std::byte{0});
    }
  }

  void put_bytes(const void * src, size_t n)
  {
    const auto * p = static_cast<const std::byte *>(src);
    buf.insert(buf.end(), p, p + n);
  }
};

// ---------------------------------------------------------------------------
// Deser read helpers
// ---------------------------------------------------------------------------

static void deser_read(DeserializeCursor & c, void * dst,
  const DeserAnyType & t, SampleOrKey what);
static void deser_read_struct(DeserializeCursor & c, void * dst,
  const DeserStruct & s, SampleOrKey what);
static void deser_read_many(DeserializeCursor & c, void * base,
  size_t native_stride, size_t count, const DeserAnyType & elem, SampleOrKey what);

static void deser_read_many(
  DeserializeCursor & c, void * base,
  size_t native_stride, size_t count, const DeserAnyType & elem, SampleOrKey what)
{
  for (size_t i = 0; i < count; ++i) {
    void * p = static_cast<char *>(base) + i * native_stride;
    deser_read(c, p, elem, what);
  }
}

static void deser_read(
  DeserializeCursor & c, void * dst,
  const DeserAnyType & t, SampleOrKey what)
{
  std::visit(
    [&](const auto & v) {
      using T = std::decay_t<decltype(v)>;

      if constexpr (std::is_same_v<T, SerPrimitive>) {
        c.align(v.cdr_align);
        if (!v.needs_bswap) {
          c.get_bytes(dst, v.native_size);
        } else {
          const unsigned char * src = c.advance(v.cdr_size);
          switch (v.cdr_size) {
            case 1: std::memcpy(dst, src, 1); break;
            case 2: bswap_n<2>(dst, src); break;
            case 4: bswap_n<4>(dst, src); break;
            case 8: bswap_n<8>(dst, src); break;
            default: break;
          }
        }

      } else if constexpr (std::is_same_v<T, DeserString>) {
        uint32_t len;
        c.align(4);
        c.get_bytes(&len, 4);
        if (len == 0) {
          throw std::runtime_error("CDR deserialization: size-0 string");
        }
        const unsigned char * str_bytes = c.advance(len);
        if (str_bytes[len - 1] != '\0') {
          throw std::runtime_error("CDR deserialization: unterminated string");
        }
        v.assign(dst, reinterpret_cast<const char *>(str_bytes), len - 1);

      } else if constexpr (std::is_same_v<T, DeserWString>) {
        uint32_t byte_len;
        c.align(4);
        c.get_bytes(&byte_len, 4);
        if (byte_len % 2) {
          throw std::runtime_error("CDR deserialization: odd wstring byte count");
        }
        const unsigned char * str_bytes = c.advance(byte_len);
        size_t n_chars = byte_len / 2;
        if (host_needs_bswap()) {
          std::vector<char16_t> tmp(n_chars);
          for (size_t i = 0; i < n_chars; ++i) {
            uint16_t ch;
            bswap_n<2>(&ch, str_bytes + i * 2);
            tmp[i] = static_cast<char16_t>(ch);
          }
          v.assign(dst, tmp.data(), n_chars);
        } else {
          v.assign(dst, reinterpret_cast<const char16_t *>(str_bytes), n_chars);
        }

      } else if constexpr (std::is_same_v<T, DeserCBoolVector>) {
        uint32_t count;
        c.align(4);
        c.get_bytes(&count, 4);
        if (!v.resize(dst, count)) { throw std::bad_alloc(); }
        for (uint32_t i = 0; i < count; ++i) {
          const unsigned char * b = c.advance(1);
          v.assign_byte(dst, i, *b);
        }

      } else if constexpr (std::is_same_v<T, DeserCppBoolVector>) {
        uint32_t count;
        c.align(4);
        c.get_bytes(&count, 4);
        v.resize(dst, count);
        for (uint32_t i = 0; i < count; ++i) {
          const unsigned char * b = c.advance(1);
          bool val = (*b != 0);
          v.assign_fn(dst, i, &val);
        }

      } else if constexpr (std::is_same_v<T, DeserArray>) {
        if (v.trivial_at_align[c.offset() % kSerMaxAlign]) {
          c.get_bytes(dst, v.count * v.native_elem_stride);
        } else {
          deser_read_many(c, dst, v.native_elem_stride, v.count, *v.element, what);
        }

      } else if constexpr (std::is_same_v<T, DeserCSequence>) {
        uint32_t count;
        c.align(4);
        c.get_bytes(&count, 4);
        if (!v.resize(dst, count)) { throw std::bad_alloc(); }
        if (count == 0) {
          return;
        }
        void * base = v.mut_contents(dst);
        if (v.trivial_at_align[c.offset() % kSerMaxAlign]) {
          c.get_bytes(base, count * v.native_elem_stride);
        } else {
          deser_read_many(c, base, v.native_elem_stride, count, *v.element, what);
        }

      } else if constexpr (std::is_same_v<T, DeserCppSequence>) {
        uint32_t count;
        c.align(4);
        c.get_bytes(&count, 4);
        v.resize(dst, count);
        if (count == 0) {
          return;
        }
        void * base = v.mut_contents(dst);
        if (v.trivial_at_align[c.offset() % kSerMaxAlign]) {
          c.get_bytes(base, count * v.native_elem_stride);
        } else {
          deser_read_many(c, base, v.native_elem_stride, count, *v.element, what);
        }

      } else if constexpr (std::is_same_v<T, DeserStruct>) {
        deser_read_struct(c, dst, v, what);
      }
    }, t);
}

static void deser_read_struct(
  DeserializeCursor & c, void * dst,
  const DeserStruct & s, SampleOrKey what)
{
  if (what == SampleOrKey::Sample && s.trivial_at_align[c.offset() % kSerMaxAlign]) {
    c.get_bytes(dst, s.native_size);
    return;
  }
  bool all_fields = (what == SampleOrKey::Sample || !s.has_keys);
  for (const auto & member : s.members) {
    if (all_fields || member.is_key) {
      void * field = static_cast<char *>(dst) + member.native_offset;
      deser_read(c, field, *member.type, what);
    }
  }
}

// ---------------------------------------------------------------------------
// extractkey helpers — scan CDR src, write key fields to WriteVecCursor dst
// ---------------------------------------------------------------------------

enum class ExtractKeyMode2 { Sample, Key, Skip };

static void extract_key_write(DeserializeCursor & src, WriteVecCursor & dst,
  const DeserAnyType & t, ExtractKeyMode2 mode, bool bswap_src, bool bswap_dst);
static void extract_key_write_struct(DeserializeCursor & src, WriteVecCursor & dst,
  const DeserStruct & s, ExtractKeyMode2 mode, bool bswap_src, bool bswap_dst);

static void extract_key_write_primitive(
  DeserializeCursor & src, WriteVecCursor & dst,
  const SerPrimitive & p, ExtractKeyMode2 mode, bool bswap_src, bool bswap_dst)
{
  src.align(p.cdr_align);
  const unsigned char * data = src.advance(p.cdr_size);
  if (mode == ExtractKeyMode2::Skip) {
    return;
  }
  dst.align(p.cdr_align);
  if (bswap_src == bswap_dst) {
    dst.put_bytes(data, p.cdr_size);
  } else {
    unsigned char tmp[8];
    switch (p.cdr_size) {
      case 1: std::memcpy(tmp, data, 1); break;
      case 2: bswap_n<2>(tmp, data); break;
      case 4: bswap_n<4>(tmp, data); break;
      case 8: bswap_n<8>(tmp, data); break;
      default: std::memcpy(tmp, data, p.cdr_size); break;
    }
    dst.put_bytes(tmp, p.cdr_size);
  }
}

static void extract_key_write_u32(
  DeserializeCursor & src, WriteVecCursor & dst,
  uint32_t * out_val, ExtractKeyMode2 mode, bool bswap_src, bool bswap_dst)
{
  src.align(4);
  const unsigned char * data = src.advance(4);
  uint32_t val;
  std::memcpy(&val, data, 4);
  if (bswap_src) {
    uint32_t swapped;
    bswap_n<4>(&swapped, &val);
    val = swapped;
  }
  *out_val = val;
  if (mode == ExtractKeyMode2::Skip) {
    return;
  }
  dst.align(4);
  if (!bswap_dst) {
    dst.put_bytes(&val, 4);
  } else {
    uint32_t be_val;
    bswap_n<4>(&be_val, &val);
    dst.put_bytes(&be_val, 4);
  }
}

static void extract_key_write_many(
  DeserializeCursor & src, WriteVecCursor & dst,
  size_t count, const DeserAnyType & elem,
  ExtractKeyMode2 mode, bool bswap_src, bool bswap_dst)
{
  for (size_t i = 0; i < count; ++i) {
    extract_key_write(src, dst, elem, mode, bswap_src, bswap_dst);
  }
}

static void extract_key_write(
  DeserializeCursor & src, WriteVecCursor & dst,
  const DeserAnyType & t, ExtractKeyMode2 mode, bool bswap_src, bool bswap_dst)
{
  std::visit(
    [&](const auto & v) {
      using T = std::decay_t<decltype(v)>;

      if constexpr (std::is_same_v<T, SerPrimitive>) {
        extract_key_write_primitive(src, dst, v, mode, bswap_src, bswap_dst);

      } else if constexpr (
        std::is_same_v<T, DeserString> || std::is_same_v<T, DeserWString>)
      {
        uint32_t len;
        src.align(4);
        src.get_bytes(&len, 4);
        if constexpr (std::is_same_v<T, DeserString>) {
          const unsigned char * str_bytes = src.advance(len);
          if (mode != ExtractKeyMode2::Skip) {
            dst.align(4);
            uint32_t dlen = len;
            if (bswap_dst) { bswap_n<4>(&dlen, &dlen); }
            dst.put_bytes(&dlen, 4);
            dst.put_bytes(str_bytes, len);
          }
        } else {
          const unsigned char * str_bytes = src.advance(len);
          if (mode != ExtractKeyMode2::Skip) {
            dst.align(4);
            uint32_t dlen = len;
            if (bswap_dst) { bswap_n<4>(&dlen, &dlen); }
            dst.put_bytes(&dlen, 4);
            if (bswap_src == bswap_dst) {
              dst.put_bytes(str_bytes, len);
            } else {
              for (uint32_t i = 0; i < len; i += 2) {
                unsigned char tmp[2];
                bswap_n<2>(tmp, str_bytes + i);
                dst.put_bytes(tmp, 2);
              }
            }
          }
        }

      } else if constexpr (
        std::is_same_v<T, DeserCBoolVector> ||
        std::is_same_v<T, DeserCppBoolVector>)
      {
        uint32_t count;
        src.align(4);
        src.get_bytes(&count, 4);
        const unsigned char * data = src.advance(count);
        if (mode != ExtractKeyMode2::Skip) {
          dst.align(4);
          uint32_t dcount = count;
          if (bswap_dst) { bswap_n<4>(&dcount, &dcount); }
          dst.put_bytes(&dcount, 4);
          dst.put_bytes(data, count);
        }

      } else if constexpr (std::is_same_v<T, DeserArray>) {
        extract_key_write_many(src, dst, v.count, *v.element, mode, bswap_src, bswap_dst);

      } else if constexpr (
        std::is_same_v<T, DeserCSequence> ||
        std::is_same_v<T, DeserCppSequence>)
      {
        uint32_t count;
        extract_key_write_u32(src, dst, &count, mode, bswap_src, bswap_dst);
        extract_key_write_many(src, dst, count, *v.element, mode, bswap_src, bswap_dst);

      } else if constexpr (std::is_same_v<T, DeserStruct>) {
        extract_key_write_struct(src, dst, v, mode, bswap_src, bswap_dst);
      }
    }, t);
}

static void extract_key_write_struct(
  DeserializeCursor & src, WriteVecCursor & dst,
  const DeserStruct & s, ExtractKeyMode2 mode, bool bswap_src, bool bswap_dst)
{
  bool all_key = !s.has_keys;
  for (const auto & member : s.members) {
    ExtractKeyMode2 m = mode;
    if (mode == ExtractKeyMode2::Sample) {
      m = (member.is_key || all_key) ? ExtractKeyMode2::Key : ExtractKeyMode2::Skip;
    } else if (mode == ExtractKeyMode2::Key) {
      if (!member.is_key && !all_key) {
        m = ExtractKeyMode2::Skip;
      }
    }
    extract_key_write(src, dst, *member.type, m, bswap_src, bswap_dst);
  }
}

// ---------------------------------------------------------------------------
// CDRDeserializer implementation
// ---------------------------------------------------------------------------

CDRDeserializer::CDRDeserializer(MessageMembersVariant members, SampleOrRequest variant)
: m_storage{},
  m_root{},
  m_variant(variant)
{
  auto [root, storage] = make_deser_struct(members);
  m_storage = std::move(storage);
  m_root = std::move(root);
}

void CDRDeserializer::deserialize(
  void * dst, const void * cdr, size_t cdrsize, SampleOrKey what) const
{
  if (what == SampleOrKey::Key && !m_root.has_keys) {
    return;
  }
  DeserializeCursor c(cdr, cdrsize);
  const unsigned char * hdr = c.advance(4);
  c.rebase(+4);
  if (hdr[0] != 0 || hdr[1] > 1) {
    throw std::runtime_error("CDR deserialization: unrecognized header");
  }
  const bool bswap_src = (hdr[1] == 0) == (std::endian::native == std::endian::little);

  if (what == SampleOrKey::Sample && m_variant == SampleOrRequest::Request) {
    auto * req = static_cast<cdds_request_wrapper_t *>(dst);
    c.get_bytes(&req->header.guid, sizeof(req->header.guid));
    c.get_bytes(&req->header.seq,  sizeof(req->header.seq));
    if (bswap_src) {
      bswap_n<8>(&req->header.guid, &req->header.guid);
      bswap_n<8>(&req->header.seq,  &req->header.seq);
    }
    dst = req->data;
  }

  if (what == SampleOrKey::Sample || m_root.has_keys) {
    deser_read_struct(c, dst, m_root, what);
  }
  c.rebase(-4);
}

void CDRDeserializer::extractkey(
  std::vector<std::byte> & dst, const void * cdr, size_t cdrsize,
  SampleOrKey what) const
{
  extractkey_impl(dst, cdr, cdrsize, what, false);
}

void CDRDeserializer::extractkey_be(
  std::vector<std::byte> & dst, const void * cdr, size_t cdrsize,
  SampleOrKey what) const
{
  extractkey_impl(dst, cdr, cdrsize, what, true);
}

void CDRDeserializer::extractkey_impl(
  std::vector<std::byte> & dst, const void * cdr, size_t cdrsize,
  SampleOrKey what, bool output_be) const
{
  if (!m_root.has_keys) {
    return;
  }
  DeserializeCursor src(cdr, cdrsize);
  const unsigned char * hdr = src.advance(4);
  src.rebase(+4);
  if (hdr[0] != 0 || hdr[1] > 1) {
    throw std::runtime_error("CDR deserialization: unrecognized header");
  }
  const bool bswap_src = (hdr[1] == 0) == (std::endian::native == std::endian::little);
  const bool bswap_dst = output_be;

  const unsigned char rtps_hdr[4] = {0,
    static_cast<unsigned char>(output_be ? 0 : 1), 0, 0};
  dst.insert(dst.end(),
    reinterpret_cast<const std::byte *>(rtps_hdr),
    reinterpret_cast<const std::byte *>(rtps_hdr) + 4);

  WriteVecCursor wc(dst);
  wc.rebase(+4);

  if (what == SampleOrKey::Sample && m_variant == SampleOrRequest::Request) {
    src.advance(16);
  }

  const ExtractKeyMode2 kmode =
    (what == SampleOrKey::Key) ? ExtractKeyMode2::Key : ExtractKeyMode2::Sample;
  extract_key_write_struct(src, wc, m_root, kmode, bswap_src, bswap_dst);

  wc.rebase(-4);
  src.rebase(-4);
}

size_t CDRDeserializer::print(
  char * dst, size_t dstsize, const void *, size_t, SampleOrKey) const
{
  if (dstsize > 0) { dst[0] = '\0'; }
  return 0;
}

}  // namespace rmw_cyclonedds_cpp
