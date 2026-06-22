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
#ifndef SER_TYPE_SUPPORT_HPP_
#define SER_TYPE_SUPPORT_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "SizeTypeSupport.hpp"   // for MessageMembersVariant, SampleOrKey, SampleOrRequest, CDRSizer
#include "BaseCDRWriter.hpp"
#include "SerDesInternals.hpp"   // kSerMaxAlign, TrivialArray, SerPrimitive

namespace rmw_cyclonedds_cpp
{

// ---------------------------------------------------------------------------
// Lightweight type descriptors for CDR serialization.
// kSerMaxAlign, TrivialArray, SerPrimitive are defined in SerDesInternals.hpp.
// ---------------------------------------------------------------------------

struct SerString
{
  const char * (*get_data)(const void * str);   // returns pointer to char data
  size_t (*get_size)(const void * str);         // returns char count (excl. NUL)
};

struct SerWString
{
  const char16_t * (*get_data)(const void * str);
  size_t (*get_size)(const void * str);
};

struct SerArray;
struct SerCSequence;
struct SerCppSequence;
struct SerCBoolVector;
struct SerCppBoolVector;
struct SerStruct;

using SerAnyType = std::variant<
  SerPrimitive,
  SerString,
  SerWString,
  SerArray,
  SerCSequence,
  SerCppSequence,
  SerCBoolVector,
  SerCppBoolVector,
  SerStruct
>;

struct SerMember
{
  const SerAnyType * type;    // non-owning; points into SerTypeStorage::nodes
  bool is_key;
  uint32_t native_offset;     // byte offset in the native C/C++ struct
};

struct SerStruct
{
  bool has_keys;
  bool is_self_contained;
  size_t native_size = 0;
  std::span<const SerMember> members;
  TrivialArray trivial_at_align = {};
  // trivial_at_align[a]: entire struct can be memcpy'd when cursor is at offset a
};

// Flat arena owning all SerAnyType nodes and SerMember entries for an entire
// message type tree.  Both vectors are pre-reserved so that pointers / spans
// into them remain stable after construction.
struct SerTypeStorage
{
  std::vector<SerAnyType> nodes;
  std::vector<SerMember> all_members;
};

struct SerArray
{
  const SerAnyType * element;    // non-owning
  size_t count;
  size_t native_elem_stride;     // sizeof() one element on this host
  size_t fixed_cdr_stride = 0;   // CDR bytes per element (0 if variable-size)
  TrivialArray trivial_at_align = {};
  // trivial_at_align[a]: all count elements can be memcpy'd from alignment a
};

// rosidl C sequence: { T* data; size_t size; size_t capacity }
struct SerCSequence
{
  const SerAnyType * element;    // non-owning
  size_t native_elem_stride;     // sizeof() one element on this host
  size_t fixed_cdr_stride = 0;   // CDR bytes per element (0 if variable-size)
  TrivialArray trivial_at_align = {};
};

// C++ std::vector<T> (T != bool): { T* begin; T* end; T* cap }
struct SerCppSequence
{
  const SerAnyType * element;    // non-owning
  size_t native_elem_stride;     // sizeof(T) on this host
  size_t fixed_cdr_stride = 0;   // CDR bytes per element (0 if variable-size)
  TrivialArray trivial_at_align = {};
};

// rosidl C bool sequence: { bool* data; size_t size; size_t capacity }
struct SerCBoolVector {};

// C++ std::vector<bool>: bit-packed; use introspection callbacks (always non-null)
struct SerCppBoolVector
{
  size_t (* size_function)(const void *);
  void (* fetch_function)(const void *, size_t, void *);
};

// ---------------------------------------------------------------------------
// Build a SerStruct from a MessageMembersVariant.
// ---------------------------------------------------------------------------
std::pair<SerStruct, SerTypeStorage> make_ser_struct(MessageMembersVariant members);

// ---------------------------------------------------------------------------
// CDRSerializer — implements BaseCDRWriter::serialize().
// Size methods delegate to CDRSizer.
// ---------------------------------------------------------------------------
class CDRSerializer final : public BaseCDRWriter
{
public:
  explicit CDRSerializer(MessageMembersVariant members, SampleOrRequest variant);

  // Implements actual CDR serialization.
  void serialize(void * dest, const void * data, SampleOrKey what) const override;

  // Size methods delegated to CDRSizer.
  size_t get_serialized_size(const void * data, SampleOrKey what) const override;
  size_t get_serialized_size_estimate(const void * data, SampleOrKey what) const override;
  size_t get_min_serialized_size(SampleOrKey what) const override;
  size_t get_max_serialized_size(SampleOrKey what) const override;

  TypeGenerator type_generator() const override;

private:
  SerTypeStorage m_storage;
  SerStruct m_root;
  CDRSizer m_sizer;
  SampleOrRequest m_variant;
  TypeGenerator m_type_generator;
};

}  // namespace rmw_cyclonedds_cpp
#endif  // SER_TYPE_SUPPORT_HPP_
