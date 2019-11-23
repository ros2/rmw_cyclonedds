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

enum class endian
{
  little = DDSRT_LITTLE_ENDIAN,
  big = DDSRT_BIG_ENDIAN,
};

constexpr endian native_endian() {return endian(DDSRT_ENDIAN);}

enum class byte : unsigned char {};

template<typename Ptr, size_t N = sizeof(decltype(*Ptr(nullptr)))>
auto byte_offset(Ptr ptr, ptrdiff_t n)
{
  if (n % N != 0) {
    throw std::invalid_argument("offset is not a multiple of the pointed-to object size");
  }
  return ptr + n / N;
}

template<typename Ptr, std::enable_if_t<std::is_void<std::remove_pointer_t<Ptr>>::value, int> = 0>
auto byte_offset(Ptr ptr, ptrdiff_t n)
{
  void * p = const_cast<void *>(ptr);
  return static_cast<Ptr>(static_cast<byte *>(p) + n);
}

#endif  // BYTEWISE_HPP_
