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

struct ByteOffset
{
  ptrdiff_t n;

  explicit ByteOffset(intmax_t n_bytes)
  : n(n_bytes)
  {
    if (n_bytes < PTRDIFF_MIN || PTRDIFF_MAX < n_bytes) {
      throw std::invalid_argument("value is too large to be a byte offset " +
              std::to_string(n_bytes));
    }
  }

  ByteOffset operator*(intmax_t multiple) const
  {
    if (INTPTR_MAX / n < multiple) {
      throw std::invalid_argument("value is too large to be a byte offset " +
              std::to_string(n) + " * " +
              std::to_string(multiple));
    }
    return ByteOffset(n * multiple);
  }

  ByteOffset operator+(const ByteOffset & other) const
  {
    return ByteOffset(n + other.n);
  }
  ByteOffset operator-(const ByteOffset & other) const
  {
    return ByteOffset(n - other.n);
  }

  ByteOffset operator-() const {return ByteOffset(-n);}
  explicit operator ptrdiff_t() {return n;}

  template<typename Ptr>
  Ptr operator+(Ptr p) const
  {
    auto p2 = const_cast<void *>(p);
    // offset any (possibly cv-qualified) null pointer
    auto p3 = reinterpret_cast<char *>(p2) + n;
    return Ptr(p3);
  }

  template<typename Ptr, size_t stride = sizeof(*Ptr(nullptr))>
  Ptr operator+(Ptr p) const
  {
    if (n % stride != 0) {
      throw std::invalid_argument(
              "offset is not a multiple of the pointed-to object size");
    }
    return p + n / stride;
  }
};

template<typename T>
static auto operator-(T t, ByteOffset b)
{
  return (-b).operator+(t);
}

template<typename T>
static auto operator+(T t, ByteOffset b)
{
  return b.operator+(t);
}

#endif  // BYTEWISE_HPP_
