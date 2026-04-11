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
#ifndef DESER_TYPE_SUPPORT_HPP_
#define DESER_TYPE_SUPPORT_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "SizeTypeSupport.hpp"   // MessageMembersVariant, SampleOrKey, SampleOrRequest
#include "SerDesInternals.hpp"   // SerPrimitive, TrivialArray, kSerMaxAlign
#include "BaseCDRReader.hpp"     // BaseCDRReader

namespace rmw_cyclonedds_cpp
{

// ---------------------------------------------------------------------------
// Deser* type descriptors — parallel to Ser* but carry write-side accessors.
// ---------------------------------------------------------------------------

struct DeserString
{
  void (*assign)(void * str, const char * src, size_t n);
};

struct DeserWString
{
  void (*assign)(void * str, const char16_t * src, size_t n);
};

struct DeserArray;
struct DeserCSequence;
struct DeserCppSequence;
struct DeserCBoolVector;
struct DeserCppBoolVector;
struct DeserStruct;

using DeserAnyType = std::variant<
  SerPrimitive,    // reuse: cdr_size/cdr_align/native_size/needs_bswap/trivial_at_align
  DeserString,
  DeserWString,
  DeserArray,
  DeserCSequence,
  DeserCppSequence,
  DeserCBoolVector,
  DeserCppBoolVector,
  DeserStruct
>;

struct DeserMember
{
  const DeserAnyType * type;   // non-owning; points into DeserTypeStorage::nodes
  bool is_key;
  uint32_t native_offset;
};

struct DeserStruct
{
  bool has_keys;
  size_t native_size = 0;
  std::span<const DeserMember> members;
  TrivialArray trivial_at_align = {};
};

// Flat arena owning all DeserAnyType nodes and DeserMember entries for an entire
// message type tree.  Both vectors are pre-reserved so that pointers / spans
// into them remain stable after construction.
struct DeserTypeStorage
{
  std::vector<DeserAnyType> nodes;
  std::vector<DeserMember> all_members;
};

struct DeserArray
{
  const DeserAnyType * element;  // non-owning
  size_t count;
  size_t native_elem_stride;
  size_t fixed_cdr_stride = 0;
  TrivialArray trivial_at_align = {};
};

// rosidl C sequence: { T* data; size_t size; size_t capacity }
struct DeserCSequence
{
  const DeserAnyType * element;  // non-owning
  size_t native_elem_stride;
  size_t fixed_cdr_stride = 0;
  TrivialArray trivial_at_align = {};
  bool (* resize)(void * seq, size_t n);   // returns false on alloc failure
  void * (* mut_contents)(void * seq);     // returns data pointer
};

// C++ std::vector<T> (T != bool): { T* begin; T* end; T* cap }
struct DeserCppSequence
{
  const DeserAnyType * element;  // non-owning
  size_t native_elem_stride;
  size_t fixed_cdr_stride = 0;
  TrivialArray trivial_at_align = {};
  void (* resize)(void * seq, size_t n);   // calls resize_function (no return)
  void * (* mut_contents)(void * seq);     // returns begin pointer
};

// rosidl C bool sequence: { bool* data; size_t size; size_t capacity }
struct DeserCBoolVector
{
  bool (* resize)(void * seq, size_t n);   // rosidl resize_function; returns false on failure
  void (* assign_byte)(void * seq, size_t idx, uint8_t val);  // writes data[idx]
};

// C++ std::vector<bool>: bit-packed; use introspection callbacks
struct DeserCppBoolVector
{
  void (* resize)(void * seq, size_t n);   // rosidl resize_function (no return)
  void (* assign_fn)(void * seq, size_t idx, const void * val);  // rosidl assign_function
};

// Build a DeserStruct from a MessageMembersVariant.
std::pair<DeserStruct, DeserTypeStorage> make_deser_struct(MessageMembersVariant members);

// ---------------------------------------------------------------------------
// CDRDeserializer — implements BaseCDRReader using the Deser* type system.
// ---------------------------------------------------------------------------
class CDRDeserializer final : public BaseCDRReader
{
public:
  explicit CDRDeserializer(MessageMembersVariant members, SampleOrRequest variant);

  void deserialize(
    void * dst, const void * cdr, size_t cdrsize, SampleOrKey what) const override;

  void extractkey(
    std::vector<std::byte> & dst, const void * cdr, size_t cdrsize,
    SampleOrKey what) const override;

  void extractkey_be(
    std::vector<std::byte> & dst, const void * cdr, size_t cdrsize,
    SampleOrKey what) const override;

  size_t print(
    char * dst, size_t dstsize, const void * cdr, size_t cdrsize,
    SampleOrKey what) const override;

private:
  void extractkey_impl(
    std::vector<std::byte> & dst, const void * cdr, size_t cdrsize,
    SampleOrKey what, bool output_be) const;

  DeserTypeStorage m_storage;
  DeserStruct m_root;
  SampleOrRequest m_variant;
};

}  // namespace rmw_cyclonedds_cpp
#endif  // DESER_TYPE_SUPPORT_HPP_
