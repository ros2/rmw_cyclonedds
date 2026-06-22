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
#ifndef SIZE_TYPE_SUPPORT_HPP_
#define SIZE_TYPE_SUPPORT_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "TypeSupport2.hpp"

namespace rmw_cyclonedds_cpp
{

// ---------------------------------------------------------------------------
// Lightweight value-type descriptors — only what CDR size computation needs.
// These do NOT carry any data accessor callbacks; they are pure metadata.
// ---------------------------------------------------------------------------

struct SzPrimitive
{
  uint8_t cdr_size;   // bytes in CDR (1/2/4/8)
  uint8_t cdr_align;  // alignment in CDR (same as cdr_size for all XCDR1 primitives)
};

struct SzString
{
  uint32_t bound;  // max char count excluding NUL; UINT32_MAX-1 means unbounded
};

struct SzWString
{
  uint32_t bound;  // max char16_t count; UINT32_MAX/2 means unbounded
};

struct SzArray;
struct SzCSequence;
struct SzCppSequence;
struct SzCBoolVector;
struct SzCppBoolVector;
struct SzStruct;

// Forward-declare the variant before the types that hold it.
using SzAnyType = std::variant<
  SzPrimitive,
  SzString,
  SzWString,
  SzArray,
  SzCSequence,
  SzCppSequence,
  SzCBoolVector,
  SzCppBoolVector,
  SzStruct
>;

// Precomputed fast-path tag for each member, set during the stamp pass.
// Allows sz_exact_struct to size every common member type without std::visit.
enum class SzMemberKind : uint8_t
{
  Fixed,          // align + advance (primitives, fixed structs, fixed arrays)
  String,         // u32 + read native size + 1
  WString,        // u32 + read native size * 2
  CSeqFixed,      // u32 + read count from {ptr,size,cap} + align + count*stride
  CppVecFixed,    // u32 + compute count from {begin,end,cap}/stride + align + count*stride
  BoolVecC,       // u32 + read count from {ptr,size,cap}
  BoolVecCpp,     // u32 + size_function(data)
  Struct,         // recurse into sz_exact_struct directly (non-fixed nested struct)
  Slow,           // fallback: call sz_exact (variable-stride arrays/sequences)
};

struct SzStruct;  // forward-declare for pointer in SzMemberCache

struct SzMemberCache
{
  SzMemberKind kind = SzMemberKind::Slow;
  uint8_t elem_align = 0;                    // CDR alignment (Fixed/seq fast paths)
  size_t  cdr_stride = 0;                    // CDR bytes (Fixed) or CDR stride per element (seq)
  size_t  native_elem_stride = 0;            // native bytes per element (CppVecFixed: divisor)
  size_t (* size_fn)(const void *) = nullptr; // BoolVecCpp only
  const SzStruct * sub_struct = nullptr;     // Struct kind: direct pointer, avoids std::visit
};

struct SzMember
{
  const SzAnyType * type;  // non-owning; points into SzTypeStorage::nodes
  bool is_key;
  uint32_t native_offset;  // byte offset of this field in the native C/C++ struct
  SzMemberCache cache;     // precomputed fast-path info (set during stamp pass)
};

struct SzStruct
{
  bool has_keys;
  bool is_self_contained;
  bool is_fixed = false;       // true if all Sample fields have fixed CDR size
  bool alignment_invariant = false;  // true if fixed CDR size is the same at every starting alignment
  size_t first_field_align = 1;  // CDR alignment of the first Sample field
  size_t max_field_align = 1;    // maximum CDR alignment of any Sample field
  size_t fixed_cdr_size = 0;  // valid only when is_fixed; CDR bytes of all Sample fields (from offset 0)
  size_t native_size = 0;     // sizeof() the native C/C++ struct
  std::span<const SzMember> members;
};

// Flat arena owning all SzAnyType nodes and SzMember entries for an entire
// message type tree.  Both vectors are pre-reserved so that pointers / spans
// into them remain stable after construction.
struct SzTypeStorage
{
  std::vector<SzAnyType> nodes;
  std::vector<SzMember> all_members;
};

struct SzArray
{
  const SzAnyType * element;  // non-owning; owned by enclosing SzStruct
  size_t count;
  size_t fixed_stride = 0;   // non-zero if element is fixed-size: CDR bytes per element
  size_t native_elem_stride = 0;  // sizeof element in native memory
};

// rosidl C sequence: { T* data; size_t size; size_t capacity }
struct SzCSequence
{
  const SzAnyType * element;  // non-owning; owned by enclosing SzStruct
  uint32_t bound;             // UINT32_MAX means unbounded
  size_t fixed_stride = 0;   // non-zero if element is fixed-size: CDR bytes per element
  size_t native_elem_stride = 0;  // sizeof element in native memory
};

// C++ std::vector<T> (T != bool): { T* begin; T* end; T* cap }
struct SzCppSequence
{
  const SzAnyType * element;  // non-owning; owned by enclosing SzStruct
  uint32_t bound;             // UINT32_MAX means unbounded
  size_t fixed_stride = 0;   // non-zero if element is fixed-size: CDR bytes per element
  size_t native_elem_stride = 0;  // sizeof(T) in native memory
};

// rosidl C bool sequence: { bool* data; size_t size; size_t capacity }
struct SzCBoolVector
{
  uint32_t bound;  // UINT32_MAX means unbounded
};

// C++ std::vector<bool>: bit-packed storage; use introspection size_function
struct SzCppBoolVector
{
  uint32_t bound;  // UINT32_MAX means unbounded
  size_t (* size_function)(const void *);  // always non-null; returns element count
};

// ---------------------------------------------------------------------------
// Build a SzStruct from a MessageMembersVariant.
// Returns the root struct and a storage arena owning all SzAnyType nodes.
// ---------------------------------------------------------------------------
std::pair<SzStruct, SzTypeStorage> make_sz_struct(MessageMembersVariant members);

// ---------------------------------------------------------------------------
// CDRSizer — implements the four BaseCDRWriter size methods without building
// a full StructValueType.  Constructed from a MessageMembersVariant.
// ---------------------------------------------------------------------------
class CDRSizer
{
public:
  explicit CDRSizer(MessageMembersVariant members, SampleOrRequest variant);

  size_t get_serialized_size(const void * data, SampleOrKey what) const;
  size_t get_serialized_size_estimate(const void * data, SampleOrKey what) const;
  size_t get_min_serialized_size(SampleOrKey what) const;
  size_t get_max_serialized_size(SampleOrKey what) const;

private:
  SzTypeStorage m_storage;
  SzStruct m_root;
  SampleOrRequest m_variant;
  size_t m_min_data_size;
  size_t m_min_key_size;
  size_t m_max_data_size;
  size_t m_max_key_size;
};

}  // namespace rmw_cyclonedds_cpp
#endif  // SIZE_TYPE_SUPPORT_HPP_
