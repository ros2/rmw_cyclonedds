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

#include "SizeTypeSupport.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>


#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include <string>
#include "rosidl_runtime_c/string.h"
#include "rosidl_runtime_c/u16string.h"

namespace rmw_cyclonedds_cpp
{


// ---------------------------------------------------------------------------
// Helpers to query properties from SzAnyType
// ---------------------------------------------------------------------------

static bool sz_is_self_contained(const SzAnyType & t)
{
  return std::visit(
    [](const auto & v) -> bool {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) {
        return true;
      } else if constexpr (std::is_same_v<T, SzStruct>) {
        return v.is_self_contained;
      } else if constexpr (std::is_same_v<T, SzArray>) {
        return sz_is_self_contained(*v.element);
      } else {
        return false;  // SzString, SzWString, SzCSequence, SzCppSequence, SzCBoolVector, SzCppBoolVector
      }
    }, t);
}

// ---------------------------------------------------------------------------
// Native memory stride of a sequence element (for C-style seq traversal)
// ---------------------------------------------------------------------------

static size_t sz_native_elem_stride_c(const SzAnyType & elem)
{
  return std::visit(
    [](const auto & v) -> size_t {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) { return v.cdr_size; }
      if constexpr (std::is_same_v<T, SzStruct>)    { return v.native_size; }
      if constexpr (std::is_same_v<T, SzString>)    { return sizeof(rosidl_runtime_c__String); }
      if constexpr (std::is_same_v<T, SzWString>)   { return sizeof(rosidl_runtime_c__U16String); }
      return size_t{0};
    }, elem);
}

static size_t sz_native_elem_stride_cpp(const SzAnyType & elem)
{
  return std::visit(
    [](const auto & v) -> size_t {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) { return v.cdr_size; }
      if constexpr (std::is_same_v<T, SzStruct>)    { return v.native_size; }
      if constexpr (std::is_same_v<T, SzString>)    { return sizeof(std::string); }
      if constexpr (std::is_same_v<T, SzWString>)   { return sizeof(std::u16string); }
      return size_t{0};
    }, elem);
}

// ---------------------------------------------------------------------------
// make_sz_struct — builds SzStruct from introspection metadata
// ---------------------------------------------------------------------------

static SzStruct make_sz_struct_c(
  const rosidl_typesupport_introspection_c__MessageMembers * impl,
  SzTypeStorage & store);
static SzStruct make_sz_struct_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl,
  SzTypeStorage & store);
static void sz_stamp_fixed_struct(SzStruct & s);
static void sz_stamp_fixed(SzAnyType & t);
static size_t sz_first_field_align(const SzAnyType & t);
static size_t sz_max_field_align(const SzAnyType & t);


std::pair<SzStruct, SzTypeStorage> make_sz_struct(MessageMembersVariant members)
{
  SzTypeStorage store;
  // Count pass: determine how many nodes and members we need, then reserve.
  auto counts = std::visit(
    [](const auto * m) { return count_type_tree(m); }, members);
  store.nodes.reserve(counts.nodes);
  store.all_members.reserve(counts.members);

  auto root = std::visit(
    [&store](const auto * m) -> SzStruct {
      using T = std::decay_t<decltype(*m)>;
      if constexpr (std::is_same_v<T, MetaMessage<TypeGenerator::ROSIDL_C>>) {
        return make_sz_struct_c(m, store);
      } else {
        return make_sz_struct_cpp(m, store);
      }
    }, members);
  // Stamp pass: iterate all nodes in allocation order (children before parents).
  for (auto & node : store.nodes) {
    sz_stamp_fixed(node);
  }
  sz_stamp_fixed_struct(root);
  // Stamp fast-path cache on every SzMember so that sz_exact_struct can
  // handle most members via a switch without entering std::visit.
  for (auto & m : store.all_members) {
    std::visit(
      [&m](const auto & v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, SzPrimitive>) {
          m.cache = {SzMemberKind::Fixed, v.cdr_align, v.cdr_size, 0, nullptr, nullptr};
        } else if constexpr (std::is_same_v<T, SzStruct>) {
          if (v.is_fixed) {
            m.cache = {SzMemberKind::Fixed,
              static_cast<uint8_t>(v.first_field_align), v.fixed_cdr_size, 0, nullptr, nullptr};
          } else {
            m.cache = {SzMemberKind::Struct, 0, 0, 0, nullptr, &v};
          }
        } else if constexpr (std::is_same_v<T, SzArray>) {
          if (v.fixed_stride > 0 && v.count > 0) {
            m.cache = {SzMemberKind::Fixed,
              static_cast<uint8_t>(sz_first_field_align(*v.element)),
              v.count * v.fixed_stride, 0, nullptr, nullptr};
          }
        } else if constexpr (std::is_same_v<T, SzString>) {
          m.cache = {SzMemberKind::String, 0, 0, 0, nullptr, nullptr};
        } else if constexpr (std::is_same_v<T, SzWString>) {
          m.cache = {SzMemberKind::WString, 0, 0, 0, nullptr, nullptr};
        } else if constexpr (std::is_same_v<T, SzCSequence>) {
          if (v.fixed_stride > 0) {
            m.cache = {SzMemberKind::CSeqFixed,
              static_cast<uint8_t>(sz_max_field_align(*v.element)),
              v.fixed_stride, 0, nullptr, nullptr};
          }
        } else if constexpr (std::is_same_v<T, SzCppSequence>) {
          if (v.fixed_stride > 0) {
            m.cache = {SzMemberKind::CppVecFixed,
              static_cast<uint8_t>(sz_max_field_align(*v.element)),
              v.fixed_stride, v.native_elem_stride, nullptr, nullptr};
          }
        } else if constexpr (std::is_same_v<T, SzCBoolVector>) {
          m.cache = {SzMemberKind::BoolVecC, 0, 0, 0, nullptr, nullptr};
        } else if constexpr (std::is_same_v<T, SzCppBoolVector>) {
          m.cache = {SzMemberKind::BoolVecCpp, 0, 0, 0, v.size_function, nullptr};
        }
      }, *m.type);
  }
  return {std::move(root), std::move(store)};
}

// Allocate an SzAnyType into the flat storage arena and return a raw pointer.
// Precondition: store.nodes has been reserve()'d so emplace_back won't reallocate.
template<typename T>
static const SzAnyType * alloc_sz(SzTypeStorage & store, T value)
{
  assert(store.nodes.size() < store.nodes.capacity());
  store.nodes.emplace_back(std::move(value));
  return &store.nodes.back();
}

// Build the element descriptor for a single member (before array/sequence wrapping).
// (Forward declaration of make_sz_element removed — it was unused.)
static const SzAnyType * make_sz_element_c(
  const rosidl_typesupport_introspection_c__MessageMember & m,
  SzTypeStorage & store)
{
  switch (ROSIDL_TypeKind(m.type_id_)) {
    case ROSIDL_TypeKind::FLOAT:
      return alloc_sz(store, SzPrimitive{4, 4});
    case ROSIDL_TypeKind::DOUBLE:
      return alloc_sz(store, SzPrimitive{8, 8});
    case ROSIDL_TypeKind::CHAR:
    case ROSIDL_TypeKind::WCHAR:
    case ROSIDL_TypeKind::BOOLEAN:
    case ROSIDL_TypeKind::OCTET:
    case ROSIDL_TypeKind::UINT8:
    case ROSIDL_TypeKind::INT8:
      return alloc_sz(store, SzPrimitive{1, 1});
    case ROSIDL_TypeKind::UINT16:
    case ROSIDL_TypeKind::INT16:
      return alloc_sz(store, SzPrimitive{2, 2});
    case ROSIDL_TypeKind::UINT32:
    case ROSIDL_TypeKind::INT32:
      return alloc_sz(store, SzPrimitive{4, 4});
    case ROSIDL_TypeKind::UINT64:
    case ROSIDL_TypeKind::INT64:
      return alloc_sz(store, SzPrimitive{8, 8});
    case ROSIDL_TypeKind::STRING:
      return alloc_sz(store, SzString{UINT32_MAX - 1});
    case ROSIDL_TypeKind::WSTRING:
      return alloc_sz(store, SzWString{UINT32_MAX / 2});
    case ROSIDL_TypeKind::MESSAGE: {
      auto sub = make_sz_struct_c(
        static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(
          m.members_->data), store);
      return alloc_sz(store, std::move(sub));
    }
    default:
      throw std::runtime_error("make_sz_element_c: unknown type kind");
  }
}

static SzStruct make_sz_struct_c(
  const rosidl_typesupport_introspection_c__MessageMembers * impl,
  SzTypeStorage & store)
{
  SzStruct st;
  st.has_keys = false;
  st.is_self_contained = true;
  st.native_size = impl->size_of_;

  // Collect members locally first — recursive make_sz_element_c calls may
  // push child-struct members into store.all_members, so we must not
  // interleave this struct's members with theirs.
  struct LocalMember { const SzAnyType * type; bool is_key; uint32_t offset; };
  std::vector<LocalMember> local;
  local.reserve(impl->member_count_);

  for (uint32_t i = 0; i < impl->member_count_; ++i) {
    const auto & m = impl->members_[i];

    const SzAnyType * elem = make_sz_element_c(m, store);

    const SzAnyType * member_type;
    if (!m.is_array_) {
      member_type = elem;
    } else if (m.array_size_ != 0 && !m.is_upper_bound_) {
      member_type = alloc_sz(store, SzArray{elem, m.array_size_, 0, sz_native_elem_stride_c(*elem)});
    } else {
      uint32_t bound = (m.array_size_ != 0 && m.is_upper_bound_) ?
        static_cast<uint32_t>(m.array_size_) : UINT32_MAX;
      member_type = alloc_sz(store, SzCSequence{elem, bound, 0, sz_native_elem_stride_c(*elem)});
    }

    if (m.is_key_) {
      st.has_keys = true;
    }
    if (!sz_is_self_contained(*member_type)) {
      st.is_self_contained = false;
    }
    local.push_back({member_type, m.is_key_, m.offset_});
  }
  // Batch-append this struct's members (after all children are done).
  const size_t member_start = store.all_members.size();
  for (auto & lm : local) {
    assert(store.all_members.size() < store.all_members.capacity());
    store.all_members.push_back(SzMember{lm.type, lm.is_key, lm.offset});
  }
  st.members = std::span<const SzMember>(
    store.all_members.data() + member_start,
    store.all_members.size() - member_start);
  return st;
}

static const SzAnyType * make_sz_element_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMember & m,
  SzTypeStorage & store)
{
  switch (ROSIDL_TypeKind(m.type_id_)) {
    case ROSIDL_TypeKind::FLOAT:
      return alloc_sz(store, SzPrimitive{4, 4});
    case ROSIDL_TypeKind::DOUBLE:
      return alloc_sz(store, SzPrimitive{8, 8});
    case ROSIDL_TypeKind::CHAR:
    case ROSIDL_TypeKind::WCHAR:
    case ROSIDL_TypeKind::BOOLEAN:
    case ROSIDL_TypeKind::OCTET:
    case ROSIDL_TypeKind::UINT8:
    case ROSIDL_TypeKind::INT8:
      return alloc_sz(store, SzPrimitive{1, 1});
    case ROSIDL_TypeKind::UINT16:
    case ROSIDL_TypeKind::INT16:
      return alloc_sz(store, SzPrimitive{2, 2});
    case ROSIDL_TypeKind::UINT32:
    case ROSIDL_TypeKind::INT32:
      return alloc_sz(store, SzPrimitive{4, 4});
    case ROSIDL_TypeKind::UINT64:
    case ROSIDL_TypeKind::INT64:
      return alloc_sz(store, SzPrimitive{8, 8});
    case ROSIDL_TypeKind::STRING:
      return alloc_sz(store, SzString{UINT32_MAX - 1});
    case ROSIDL_TypeKind::WSTRING:
      return alloc_sz(store, SzWString{UINT32_MAX / 2});
    case ROSIDL_TypeKind::MESSAGE: {
      auto sub = make_sz_struct_cpp(
        static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(
          m.members_->data), store);
      return alloc_sz(store, std::move(sub));
    }
    default:
      throw std::runtime_error("make_sz_element_cpp: unknown type kind");
  }
}

static SzStruct make_sz_struct_cpp(
  const rosidl_typesupport_introspection_cpp::MessageMembers * impl,
  SzTypeStorage & store)
{
  SzStruct st;
  st.has_keys = false;
  st.is_self_contained = true;
  st.native_size = impl->size_of_;

  // Collect members locally first — recursive make_sz_element_cpp calls may
  // push child-struct members into store.all_members, so we must not
  // interleave this struct's members with theirs.
  struct LocalMember { const SzAnyType * type; bool is_key; uint32_t offset; };
  std::vector<LocalMember> local;
  local.reserve(impl->member_count_);

  for (uint32_t i = 0; i < impl->member_count_; ++i) {
    const auto & m = impl->members_[i];

    const SzAnyType * elem = make_sz_element_cpp(m, store);

    const SzAnyType * member_type;
    if (!m.is_array_) {
      member_type = elem;
    } else if (m.array_size_ != 0 && !m.is_upper_bound_) {
      member_type = alloc_sz(
        store, SzArray{elem, m.array_size_, 0, sz_native_elem_stride_cpp(*elem)});
    } else {
      uint32_t bound = (m.array_size_ != 0 && m.is_upper_bound_) ?
        static_cast<uint32_t>(m.array_size_) : UINT32_MAX;
      if (ROSIDL_TypeKind(m.type_id_) == ROSIDL_TypeKind::BOOLEAN) {
        // std::vector<bool> has bit-packed storage; use introspection size_function.
        member_type = alloc_sz(store, SzCppBoolVector{bound, m.size_function});
      } else {
        member_type = alloc_sz(store, SzCppSequence{elem, bound, 0, sz_native_elem_stride_cpp(*elem)});
      }
    }

    if (m.is_key_) {
      st.has_keys = true;
    }
    if (!sz_is_self_contained(*member_type)) {
      st.is_self_contained = false;
    }
    local.push_back({member_type, m.is_key_, m.offset_});
  }
  // Batch-append this struct's members (after all children are done).
  const size_t member_start = store.all_members.size();
  for (auto & lm : local) {
    assert(store.all_members.size() < store.all_members.capacity());
    store.all_members.push_back(SzMember{lm.type, lm.is_key, lm.offset});
  }
  st.members = std::span<const SzMember>(
    store.all_members.data() + member_start,
    store.all_members.size() - member_start);
  return st;
}

// ---------------------------------------------------------------------------
// CDRSizer — size cursor arithmetic mirroring CDRWriter::serialize_size_bound
// ---------------------------------------------------------------------------

enum class MinOrMax { Min, Max };

struct SzCursor
{
  size_t offset = 0;
  bool overflowed = false;

  void align(size_t n)
  {
    // n is always 1, 2, 4, or 8 — use bitmask, not division
    size_t mask = n - 1;
    size_t rem = offset & mask;
    if (rem) {
      advance(n - rem);
    }
  }

  void advance(size_t n)
  {
    if (__builtin_expect(n > SIZE_MAX - offset, 0)) {
      overflowed = true;
      offset = SIZE_MAX;
    } else {
      offset += n;
    }
  }

  void rebase(ptrdiff_t delta)
  {
    offset = static_cast<size_t>(static_cast<ptrdiff_t>(offset) - delta);
  }
};

static void sz_put_u32(SzCursor & c)
{
  c.align(4);
  c.advance(4);
}

// Forward declarations for mutual recursion.
template<SampleOrKey What>
static bool sz_bound(SzCursor & c, const SzAnyType & t, MinOrMax mm);
template<SampleOrKey What>
static bool sz_bound_struct(SzCursor & c, const SzStruct & s, MinOrMax mm);
template<SampleOrKey What>
static bool sz_bound_many(
  SzCursor & c, size_t count, const SzAnyType & elem, MinOrMax mm);

template<SampleOrKey What>
static bool sz_bound(SzCursor & c, const SzAnyType & t, MinOrMax mm)
{
  return std::visit(
    [&](const auto & v) -> bool {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) {
        c.align(v.cdr_align);
        c.advance(v.cdr_size);
        return true;
      } else if constexpr (std::is_same_v<T, SzString>) {
        sz_put_u32(c);  // length prefix
        size_t chars = (mm == MinOrMax::Min) ? 0 : (v.bound == UINT32_MAX - 1 ? UINT32_MAX - 1 :
          static_cast<size_t>(v.bound));
        if (mm == MinOrMax::Max && v.bound == UINT32_MAX - 1) {
          c.overflowed = true;
          c.offset = SIZE_MAX;
          return false;
        }
        c.advance(chars + 1);  // chars + NUL
        return mm == MinOrMax::Min;  // fixed only if we assume empty string
      } else if constexpr (std::is_same_v<T, SzWString>) {
        sz_put_u32(c);  // byte-length prefix
        if (mm == MinOrMax::Max && v.bound == UINT32_MAX / 2) {
          c.overflowed = true;
          c.offset = SIZE_MAX;
          return false;
        }
        size_t chars = (mm == MinOrMax::Min) ? 0 : static_cast<size_t>(v.bound);
        c.advance(chars * 2);
        return mm == MinOrMax::Min;
      } else if constexpr (std::is_same_v<T, SzArray>) {
        return sz_bound_many<What>(c, v.count, *v.element, mm);
      } else if constexpr (
        std::is_same_v<T, SzCSequence> ||
        std::is_same_v<T, SzCppSequence>)
      {
        sz_put_u32(c);
        if (mm == MinOrMax::Min) {
          return true;  // min = empty sequence = just the 4-byte length prefix
        }
        if (v.bound == UINT32_MAX) {
          c.overflowed = true;
          c.offset = SIZE_MAX;
          return false;  // truly unbounded sequence
        }
        return sz_bound_many<What>(c, v.bound, *v.element, mm);
      } else if constexpr (
        std::is_same_v<T, SzCBoolVector> ||
        std::is_same_v<T, SzCppBoolVector>)
      {
        size_t count = (mm == MinOrMax::Min) ? 0 : v.bound;
        sz_put_u32(c);
        c.advance(count);
        return mm == MinOrMax::Min;
      } else if constexpr (std::is_same_v<T, SzStruct>) {
        return sz_bound_struct<What>(c, v, mm);
      }
      return false;
    }, t);
}

template<SampleOrKey What>
static bool sz_bound_many(
  SzCursor & c, size_t count, const SzAnyType & elem, MinOrMax mm)
{
  if (count == 0) {
    return false;
  }
  if (!sz_bound<What>(c, elem, mm)) {
    return false;
  }
  if (count > 1) {
    size_t before = c.offset;
    if (!sz_bound<What>(c, elem, mm)) {
      return false;
    }
    size_t elt_size = c.offset - before;
    c.advance(elt_size * (count - 2));
  }
  return true;
}

template<SampleOrKey What>
static bool sz_bound_struct(SzCursor & c, const SzStruct & s, MinOrMax mm)
{
  constexpr bool is_sample = (What == SampleOrKey::Sample);
  bool fixed = true;
  for (const auto & member : s.members) {
    if constexpr (is_sample) {
      if (!sz_bound<What>(c, *member.type, mm)) {
        fixed = false;
      }
    } else {
      if (!s.has_keys || member.is_key) {
        if (!sz_bound<What>(c, *member.type, mm)) {
          fixed = false;
        }
      }
    }
  }
  return fixed;
}

template<SampleOrKey What>
static size_t compute_bound(
  const SzStruct & root, SampleOrRequest variant, MinOrMax mm)
{
  SzCursor c;

  // RTPS header (4 bytes encoding descriptor, no alignment)
  c.advance(4);
  c.rebase(+4);

  // Request wrapper header: 8-byte GUID + 8-byte sequence number
  if constexpr (What == SampleOrKey::Sample) {
    if (variant == SampleOrRequest::Request) {
      c.advance(8);
      c.advance(8);
    }
  }

  if constexpr (What == SampleOrKey::Sample) {
    bool ok = true;
    try {
      ok = sz_bound_struct<What>(c, root, mm);
    } catch (...) {
      ok = false;
    }
    if (!ok || c.overflowed) {
      return SIZE_MAX;
    }
  } else {
    if (root.has_keys) {
      bool ok = true;
      try {
        ok = sz_bound_struct<What>(c, root, mm);
      } catch (...) {
        ok = false;
      }
      if (!ok || c.overflowed) {
        return SIZE_MAX;
      }
    }
  }

  c.rebase(-4);
  return c.overflowed ? SIZE_MAX : c.offset;
}

CDRSizer::CDRSizer(MessageMembersVariant members, SampleOrRequest variant)
: m_storage{},
  m_root{},
  m_variant{variant},
  m_min_data_size{0},
  m_min_key_size{0},
  m_max_data_size{0},
  m_max_key_size{0}
{
  auto [root, storage] = make_sz_struct(members);
  m_storage = std::move(storage);
  m_root = std::move(root);
  m_min_data_size = compute_bound<SampleOrKey::Sample>(m_root, variant, MinOrMax::Min);
  m_min_key_size = compute_bound<SampleOrKey::Key>(m_root, variant, MinOrMax::Min);
  m_max_data_size = compute_bound<SampleOrKey::Sample>(m_root, variant, MinOrMax::Max);
  m_max_key_size = compute_bound<SampleOrKey::Key>(m_root, variant, MinOrMax::Max);
}

// ---------------------------------------------------------------------------
// Exact size from data — walk the live message, measure variable parts.
// For fixed-size types (min==max) this is never called.
// ---------------------------------------------------------------------------

template<SampleOrKey What>
static size_t sz_exact(SzCursor & c, const void * data, const SzAnyType & t);
template<SampleOrKey What>
static size_t sz_exact_struct(SzCursor & c, const void * data, const SzStruct & s);

template<SampleOrKey What>
static void sz_exact_many(
  SzCursor & c, const void * base, size_t elem_stride,
  size_t count, const SzAnyType & elem)
{
  for (size_t i = 0; i < count; ++i) {
    const void * p = static_cast<const char *>(base) + i * elem_stride;
    sz_exact<What>(c, p, elem);
  }
}

// ---------------------------------------------------------------------------
// Post-build pass: stamp fixed_stride / is_fixed / fixed_cdr_size into nodes.
// ---------------------------------------------------------------------------

// Returns the CDR stride of a fixed-size element (0 if variable).
// For SzStruct, requires alignment_invariant so the stride is safe at any
// starting offset (used for SzArray members and as element of arrays).
static size_t sz_fixed_stride(const SzAnyType & t)
{
  return std::visit(
    [](const auto & v) -> size_t {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) {
        return v.cdr_size;
      } else if constexpr (std::is_same_v<T, SzStruct>) {
        return (v.is_fixed && v.alignment_invariant) ? v.fixed_cdr_size : 0;
      } else if constexpr (std::is_same_v<T, SzArray>) {
        return (v.fixed_stride > 0) ? v.count * v.fixed_stride : 0;
      }
      return 0;
    }, t);
}

// Returns the CDR stride suitable for bulk-advancing through a homogeneous
// sequence whose first element has been aligned.  Unlike sz_fixed_stride, this
// does not require alignment_invariant — it only requires that the per-element
// CDR size is a multiple of the element's first-field alignment, so that every
// subsequent element is naturally aligned after the previous one.
static size_t sz_seq_elem_stride(const SzAnyType & t)
{
  return std::visit(
    [](const auto & v) -> size_t {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) {
        return v.cdr_size;
      } else if constexpr (std::is_same_v<T, SzStruct>) {
        if (!v.is_fixed) { return 0; }
        // After c.align(max_field_align), element i sits at
        // base + i * fixed_cdr_size.  That offset is correctly aligned for
        // ALL fields iff fixed_cdr_size % max_field_align == 0.
        if (v.fixed_cdr_size == 0) { return 0; }
        return (v.fixed_cdr_size % v.max_field_align == 0)
          ? v.fixed_cdr_size : 0;
      } else if constexpr (std::is_same_v<T, SzArray>) {
        return (v.fixed_stride > 0) ? v.count * v.fixed_stride : 0;
      }
      return 0;
    }, t);
}

static void sz_stamp_fixed(SzAnyType & t)
{
  std::visit(
    [](auto & v) {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzStruct>) {
        sz_stamp_fixed_struct(v);
      } else if constexpr (std::is_same_v<T, SzArray>) {
        v.fixed_stride = sz_fixed_stride(*v.element);
      } else if constexpr (
        std::is_same_v<T, SzCSequence> ||
        std::is_same_v<T, SzCppSequence>)
      {
        v.fixed_stride = sz_seq_elem_stride(*v.element);
      }
    }, t);
}

static size_t sz_first_field_align(const SzAnyType & t);

static size_t sz_first_field_align_struct(const SzStruct & s)
{
  for (const auto & member : s.members) {
    return sz_first_field_align(*member.type);
  }
  return 1;
}

static size_t sz_first_field_align(const SzAnyType & t)
{
  return std::visit(
    [](const auto & v) -> size_t {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) {
        return v.cdr_align;
      } else if constexpr (std::is_same_v<T, SzStruct>) {
        return sz_first_field_align_struct(v);
      } else if constexpr (std::is_same_v<T, SzArray>) {
        return sz_first_field_align(*v.element);
      }
      return 4;  // strings, sequences start with u32
    }, t);
}

static size_t sz_max_field_align_struct(const SzStruct & s)
{
  size_t mx = 1;
  for (const auto & m : s.members) {
    mx = std::max(mx, sz_max_field_align(*m.type));
  }
  return mx;
}

static size_t sz_max_field_align(const SzAnyType & t)
{
  return std::visit(
    [](const auto & v) -> size_t {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) {
        return v.cdr_align;
      } else if constexpr (std::is_same_v<T, SzStruct>) {
        return v.max_field_align;
      } else if constexpr (
        std::is_same_v<T, SzArray> ||
        std::is_same_v<T, SzCSequence> ||
        std::is_same_v<T, SzCppSequence>)
      {
        return sz_max_field_align(*v.element);
      }
      return 4;  // strings, sequences: u32 header
    }, t);
}

static void sz_stamp_fixed_struct(SzStruct & s)
{
  // Compute first-field alignment for the fast path in sz_exact_struct.
  s.first_field_align = sz_first_field_align_struct(s);
  s.max_field_align = sz_max_field_align_struct(s);
  SzCursor min_c, max_c;
  bool fixed = sz_bound_struct<SampleOrKey::Sample>(min_c, s, MinOrMax::Min);
  if (fixed) {
    sz_bound_struct<SampleOrKey::Sample>(max_c, s, MinOrMax::Max);
    if (!max_c.overflowed && min_c.offset == max_c.offset) {
      s.is_fixed = true;
      // fixed_cdr_size is the net bytes consumed FROM OFFSET 0.
      // For the fast path we need to know bytes after aligning: store as-is,
      // sz_exact_struct will align first then advance by (min_c.offset - padding).
      s.fixed_cdr_size = min_c.offset;

      // Check whether the struct's CDR size is the same regardless of starting
      // alignment.  When used as element stride inside arrays/sequences, the
      // fast path can only bulk-advance if the size is alignment-invariant.
      s.alignment_invariant = true;
      for (size_t start = 1; start < 8; ++start) {
        SzCursor probe;
        probe.offset = start;
        sz_bound_struct<SampleOrKey::Sample>(probe, s, MinOrMax::Min);
        if (probe.offset - start != s.fixed_cdr_size) {
          s.alignment_invariant = false;
          break;
        }
      }
    }
  }
}

template<SampleOrKey What>
static size_t sz_exact(SzCursor & c, const void * data, const SzAnyType & t)
{
  std::visit(
    [&](const auto & v) {
      using T = std::decay_t<decltype(v)>;
      if constexpr (std::is_same_v<T, SzPrimitive>) {
        c.align(v.cdr_align);
        c.advance(v.cdr_size);
      } else if constexpr (std::is_same_v<T, SzString>) {
        struct CStrLayout { const char * data; size_t size; size_t capacity; };
        auto * s = static_cast<const CStrLayout *>(data);
        sz_put_u32(c);
        c.advance(s->size + 1);
      } else if constexpr (std::is_same_v<T, SzWString>) {
        struct WStrLayout { const char16_t * data; size_t size; size_t capacity; };
        auto * s = static_cast<const WStrLayout *>(data);
        sz_put_u32(c);
        c.advance(s->size * 2);
      } else if constexpr (std::is_same_v<T, SzArray>) {
        if (v.fixed_stride > 0 && v.count > 0) {
          // Align to first element, then bulk advance; stride already includes
          // inter-element padding for identical alignment.
          size_t elem_align = sz_first_field_align(*v.element);
          c.align(elem_align);
          c.advance(v.count * v.fixed_stride);
        } else {
          sz_exact_many<What>(c, data, v.native_elem_stride, v.count, *v.element);
        }
      } else if constexpr (std::is_same_v<T, SzCSequence>) {
        sz_put_u32(c);
        struct SeqLayout { const void * data; size_t size; size_t capacity; };
        auto * seq = static_cast<const SeqLayout *>(data);
        size_t count = seq->size;
        if (v.fixed_stride > 0) {
          if (count > 0) {
            c.align(sz_first_field_align(*v.element));
          }
          c.advance(count * v.fixed_stride);
        } else {
          sz_exact_many<What>(c, seq->data, v.native_elem_stride, count, *v.element);
        }
      } else if constexpr (std::is_same_v<T, SzCppSequence>) {
        sz_put_u32(c);
        struct VecLayout { const char * begin; const char * end; const char * cap; };
        auto * vec = static_cast<const VecLayout *>(data);
        size_t count = v.fixed_stride > 0
          ? static_cast<size_t>(vec->end - vec->begin) / v.fixed_stride
          : (v.native_elem_stride > 0
            ? static_cast<size_t>(vec->end - vec->begin) / v.native_elem_stride
            : 0);
        if (v.fixed_stride > 0) {
          if (count > 0) {
            c.align(sz_first_field_align(*v.element));
          }
          c.advance(count * v.fixed_stride);
        } else {
          sz_exact_many<What>(c, vec->begin, v.native_elem_stride, count, *v.element);
        }
      } else if constexpr (std::is_same_v<T, SzCBoolVector>) {
        sz_put_u32(c);
        struct SeqLayout { const void * data; size_t size; size_t capacity; };
        size_t count = static_cast<const SeqLayout *>(data)->size;
        c.advance(count);
      } else if constexpr (std::is_same_v<T, SzCppBoolVector>) {
        sz_put_u32(c);
        c.advance(v.size_function(data));
      } else if constexpr (std::is_same_v<T, SzStruct>) {
        sz_exact_struct<What>(c, data, v);
      }
    }, t);
  return c.offset;
}

template<SampleOrKey What>
static size_t sz_exact_struct(SzCursor & c, const void * data, const SzStruct & s)
{
  constexpr bool is_sample = (What == SampleOrKey::Sample);

  // Fast path — fixed-size alignment-invariant struct, just advance.
  if constexpr (is_sample) {
    if (s.is_fixed && s.alignment_invariant) {
      c.align(s.first_field_align);
      c.advance(s.fixed_cdr_size);
      return c.offset;
    }
  } else {
    if (s.is_fixed && s.alignment_invariant && !s.has_keys) {
      c.align(s.first_field_align);
      c.advance(s.fixed_cdr_size);
      return c.offset;
    }
  }

  // Per-member sizing via precomputed SzMemberCache — avoids std::visit for
  // all common types (primitives, fixed structs, strings, fixed-stride
  // sequences).  Only the Slow fallback enters sz_exact / std::visit.
  for (const auto & member : s.members) {
    if constexpr (!is_sample) {
      if (s.has_keys && !member.is_key) {
        continue;
      }
    }
    const void * field = static_cast<const char *>(data) + member.native_offset;
    const auto & mc = member.cache;
    switch (mc.kind) {
      case SzMemberKind::Fixed:
        c.align(mc.elem_align);
        c.advance(mc.cdr_stride);
        break;
      case SzMemberKind::String: {
        // Both rosidl_runtime_c__String and std::string have {ptr, size, ...}
        size_t len = *reinterpret_cast<const size_t *>(
          static_cast<const char *>(field) + sizeof(const char *));
        sz_put_u32(c);
        c.advance(len + 1);
        break;
      }
      case SzMemberKind::WString: {
        size_t len = *reinterpret_cast<const size_t *>(
          static_cast<const char *>(field) + sizeof(const char16_t *));
        sz_put_u32(c);
        c.advance(len * 2);
        break;
      }
      case SzMemberKind::CSeqFixed: {
        // rosidl C sequence: { T* data; size_t size; size_t capacity; }
        struct SeqLayout { const void * d; size_t sz; size_t cap; };
        size_t n = static_cast<const SeqLayout *>(field)->sz;
        sz_put_u32(c);
        if (n > 0) { c.align(mc.elem_align); }
        c.advance(n * mc.cdr_stride);
        break;
      }
      case SzMemberKind::CppVecFixed: {
        // C++ std::vector<T>: { T* begin; T* end; T* cap; }
        struct VecLayout { const char * b; const char * e; const char * cap; };
        auto * v = static_cast<const VecLayout *>(field);
        size_t n = static_cast<size_t>(v->e - v->b) / mc.native_elem_stride;
        sz_put_u32(c);
        if (n > 0) { c.align(mc.elem_align); }
        c.advance(n * mc.cdr_stride);
        break;
      }
      case SzMemberKind::BoolVecC: {
        struct SeqLayout { const void * d; size_t sz; size_t cap; };
        size_t n = static_cast<const SeqLayout *>(field)->sz;
        sz_put_u32(c);
        c.advance(n);
        break;
      }
      case SzMemberKind::BoolVecCpp: {
        size_t n = mc.size_fn(field);
        sz_put_u32(c);
        c.advance(n);
        break;
      }
      case SzMemberKind::Struct:
        sz_exact_struct<What>(c, field, *mc.sub_struct);
        break;
      case SzMemberKind::Slow:
        sz_exact<What>(c, field, *member.type);
        break;
    }
  }
  return c.offset;
}

// Internal helper: run the exact-size computation with a specific SampleOrKey
// template argument.
template<SampleOrKey What>
static size_t sz_get_serialized_size_impl(
  const SzStruct & root, SampleOrRequest variant, const void * data,
  size_t mn, size_t mx)
{
  if (mn == mx) {
    return mx;
  }
  SzCursor c;
  c.advance(4);
  c.rebase(+4);
  if constexpr (What == SampleOrKey::Sample) {
    if (variant == SampleOrRequest::Request) {
      auto * req = static_cast<const cdds_request_wrapper_t *>(data);
      c.advance(sizeof(req->header.guid));
      c.advance(sizeof(req->header.seq));
      data = req->data;
    }
    sz_exact_struct<What>(c, data, root);
  } else {
    if (root.has_keys) {
      sz_exact_struct<What>(c, data, root);
    }
  }
  c.rebase(-4);
  return c.offset;
}

size_t CDRSizer::get_min_serialized_size(SampleOrKey what) const
{
  return (what == SampleOrKey::Sample) ? m_min_data_size : m_min_key_size;
}

size_t CDRSizer::get_max_serialized_size(SampleOrKey what) const
{
  return (what == SampleOrKey::Sample) ? m_max_data_size : m_max_key_size;
}

size_t CDRSizer::get_serialized_size(const void * data, SampleOrKey what) const
{
  if (what == SampleOrKey::Sample) {
    return sz_get_serialized_size_impl<SampleOrKey::Sample>(
      m_root, m_variant, data, m_min_data_size, m_max_data_size);
  } else {
    return sz_get_serialized_size_impl<SampleOrKey::Key>(
      m_root, m_variant, data, m_min_key_size, m_max_key_size);
  }
}

size_t CDRSizer::get_serialized_size_estimate(const void * data, SampleOrKey what) const
{
  const size_t mn = get_min_serialized_size(what);
  const size_t mx = get_max_serialized_size(what);
  if (mx <= 1024 || (mn > 0 && mx / mn == 1)) {
    return mx;
  }
  return get_serialized_size(data, what);
}

}  // namespace rmw_cyclonedds_cpp
