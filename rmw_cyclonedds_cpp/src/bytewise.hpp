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
#ifndef BYTEWISE_HPP_
#define BYTEWISE_HPP_

#include <cstdint>

#include "dds/ddsrt/endian.h"

using std::size_t;
using std::ptrdiff_t;

enum class endian
{
  little = DDSRT_LITTLE_ENDIAN,
  big = DDSRT_BIG_ENDIAN,
};

constexpr endian native_endian() {return endian(DDSRT_ENDIAN);}

enum class byte : unsigned char {};

static inline auto byte_offset(void * ptr, ptrdiff_t n)
{
  return static_cast<void *>(static_cast<byte *>(ptr) + n);
}

static inline auto byte_offset(const void * ptr, ptrdiff_t n)
{
  return static_cast<const void *>(static_cast<const byte *>(ptr) + n);
}

#endif  // BYTEWISE_HPP_
