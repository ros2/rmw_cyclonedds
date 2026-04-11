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
#ifndef SER_DES_INTERNALS_HPP_
#define SER_DES_INTERNALS_HPP_

// Private implementation utilities shared between SerTypeSupport.cpp and
// DeserTypeSupport.cpp. Not part of the public API.

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

#include "SizeTypeSupport.hpp"  // for ROSIDL_TypeKind

namespace rmw_cyclonedds_cpp
{

// ---------------------------------------------------------------------------
// Trivial-at-align helpers (shared by Ser* and Deser* type descriptors)
// ---------------------------------------------------------------------------

static constexpr size_t kSerMaxAlign = 8;
using TrivialArray = std::array<bool, kSerMaxAlign>;

struct SerPrimitive
{
  uint8_t cdr_size;    // bytes in CDR (1/2/4/8)
  uint8_t cdr_align;   // CDR alignment (same as cdr_size for XCDR1 primitives)
  uint8_t native_size; // sizeof() on this host (may differ from cdr_size, e.g. bool)
  bool needs_bswap;    // true if byte-swap is required (BE host or native != CDR width)
  TrivialArray trivial_at_align = {};
  // trivial_at_align[a] = (a % cdr_align == 0) && !needs_bswap
};

static constexpr bool host_needs_bswap()
{
  return std::endian::native != std::endian::little;
}

// ---------------------------------------------------------------------------
// Byte-swap helpers
// ---------------------------------------------------------------------------

template<size_t N>
static void bswap_n(void * dst, const void * src);

template<>
inline void bswap_n<1>(void * dst, const void * src)
{
  std::memcpy(dst, src, 1);
}
template<>
inline void bswap_n<2>(void * dst, const void * src)
{
  uint16_t v; std::memcpy(&v, src, 2);
  v = static_cast<uint16_t>((v >> 8) | (v << 8));
  std::memcpy(dst, &v, 2);
}
template<>
inline void bswap_n<4>(void * dst, const void * src)
{
  uint32_t v; std::memcpy(&v, src, 4);
  v = (v >> 24) | ((v & 0x00ff0000u) >> 8) | ((v & 0x0000ff00u) << 8) | (v << 24);
  std::memcpy(dst, &v, 4);
}
template<>
inline void bswap_n<8>(void * dst, const void * src)
{
  uint64_t v; std::memcpy(&v, src, 8);
  v = (v >> 56) |
    ((v & 0x00ff000000000000ull) >> 40) |
    ((v & 0x0000ff0000000000ull) >> 24) |
    ((v & 0x000000ff00000000ull) >> 8) |
    ((v & 0x00000000ff000000ull) << 8) |
    ((v & 0x0000000000ff0000ull) << 24) |
    ((v & 0x000000000000ff00ull) << 40) |
    (v << 56);
  std::memcpy(dst, &v, 8);
}

// ---------------------------------------------------------------------------
// Primitive kind → sizes
// ---------------------------------------------------------------------------

static inline uint8_t native_size_of_kind(ROSIDL_TypeKind kind)
{
  switch (kind) {
    case ROSIDL_TypeKind::FLOAT:    return sizeof(float);
    case ROSIDL_TypeKind::DOUBLE:   return sizeof(double);
    case ROSIDL_TypeKind::CHAR:     return sizeof(char);
    case ROSIDL_TypeKind::WCHAR:    return sizeof(char16_t);
    case ROSIDL_TypeKind::BOOLEAN:  return sizeof(bool);
    case ROSIDL_TypeKind::OCTET:
    case ROSIDL_TypeKind::UINT8:
    case ROSIDL_TypeKind::INT8:     return 1;
    case ROSIDL_TypeKind::UINT16:
    case ROSIDL_TypeKind::INT16:    return 2;
    case ROSIDL_TypeKind::UINT32:
    case ROSIDL_TypeKind::INT32:    return 4;
    case ROSIDL_TypeKind::UINT64:
    case ROSIDL_TypeKind::INT64:    return 8;
    default:                        return 0;
  }
}

static inline uint8_t cdr_size_of_kind(ROSIDL_TypeKind kind)
{
  switch (kind) {
    case ROSIDL_TypeKind::FLOAT:    return 4;
    case ROSIDL_TypeKind::DOUBLE:   return 8;
    case ROSIDL_TypeKind::CHAR:
    case ROSIDL_TypeKind::WCHAR:    return 2;
    case ROSIDL_TypeKind::BOOLEAN:
    case ROSIDL_TypeKind::OCTET:
    case ROSIDL_TypeKind::UINT8:
    case ROSIDL_TypeKind::INT8:     return 1;
    case ROSIDL_TypeKind::UINT16:
    case ROSIDL_TypeKind::INT16:    return 2;
    case ROSIDL_TypeKind::UINT32:
    case ROSIDL_TypeKind::INT32:    return 4;
    case ROSIDL_TypeKind::UINT64:
    case ROSIDL_TypeKind::INT64:    return 8;
    default:                        return 0;
  }
}

// Native stride for a C primitive element in a fixed array.
static inline size_t c_elem_native_stride(ROSIDL_TypeKind kind)
{
  return native_size_of_kind(kind);
}

static inline bool prim_trivial(uint8_t cdr_align, bool needs_bswap, size_t align_offset)
{
  if (needs_bswap) { return false; }
  return (align_offset % cdr_align) == 0;
}

static inline TrivialArray make_prim_trivial_array(uint8_t cdr_align, bool needs_bswap)
{
  TrivialArray arr{};
  for (size_t a = 0; a < kSerMaxAlign; ++a) {
    arr[a] = prim_trivial(cdr_align, needs_bswap, a);
  }
  return arr;
}

static inline SerPrimitive make_ser_primitive(ROSIDL_TypeKind kind)
{
  uint8_t cdr_size = cdr_size_of_kind(kind);
  uint8_t nat_size = native_size_of_kind(kind);
  bool needs_bswap = host_needs_bswap() || (nat_size != cdr_size);
  return SerPrimitive{
    cdr_size, cdr_size,
    nat_size, needs_bswap,
    make_prim_trivial_array(cdr_size, needs_bswap)
  };
}

}  // namespace rmw_cyclonedds_cpp
#endif  // SER_DES_INTERNALS_HPP_
