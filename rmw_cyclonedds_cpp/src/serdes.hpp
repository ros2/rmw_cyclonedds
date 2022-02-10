// Copyright 2018 to 2019 ADLINK Technology
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

#ifndef SERDES_HPP_
#define SERDES_HPP_

#include <string.h>
#include <inttypes.h>
#include <stdarg.h>

#include <array>
#include <cassert>
#include <string>
#include <vector>
#include <type_traits>

#include "deserialization_exception.hpp"

using rmw_cyclonedds_cpp::DeserializationException;

class cycdeserbase
{
public:
  explicit cycdeserbase(const char * data_, size_t lim_);
  cycdeserbase() = delete;

protected:
  inline uint16_t bswap2u(uint16_t x)
  {
    return (uint16_t) ((x >> 8) | (x << 8));
  }
  inline int16_t bswap2(int16_t x)
  {
    return (int16_t) bswap2u((uint16_t) x);
  }
  inline uint32_t bswap4u(uint32_t x)
  {
    return (x >> 24) | ((x >> 8) & 0xff00) | ((x << 8) & 0xff0000) | (x << 24);
  }
  inline int32_t bswap4(int32_t x)
  {
    return (int32_t) bswap4u((uint32_t) x);
  }
  inline uint64_t bswap8u(uint64_t x)
  {
    const uint32_t newhi = bswap4u((uint32_t) x);
    const uint32_t newlo = bswap4u((uint32_t) (x >> 32));
    return ((uint64_t) newhi << 32) | (uint64_t) newlo;
  }
  inline int64_t bswap8(int64_t x)
  {
    return (int64_t) bswap8u((uint64_t) x);
  }

  inline void align(size_t a)
  {
    if ((pos % a) != 0) {
      pos += a - (pos % a);
      if (pos > lim) {
        throw DeserializationException("invalid data size");
      }
    }
  }
  inline void validate_size(size_t count, size_t sz)
  {
    assert(sz == 1 || sz == 2 || sz == 4 || sz == 8);
    if (count > (lim - pos) / sz) {
      throw DeserializationException("invalid data size");
    }
  }
  inline void validate_str(size_t sz)
  {
    if (sz > 0 && data[pos + sz - 1] != '\0') {
      throw DeserializationException("string data is not null-terminated");
    }
  }

  const char * data;
  size_t pos;
  size_t lim;
  bool swap_bytes;
};

class cycdeser : cycdeserbase
{
public:
  cycdeser(const void * data, size_t size);
  cycdeser() = delete;

  inline cycdeser & operator>>(bool & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(char & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(int8_t & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(uint8_t & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(int16_t & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(uint16_t & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(int32_t & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(uint32_t & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(int64_t & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(uint64_t & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(float & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(double & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(std::string & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(std::wstring & x) {deserialize(x); return *this;}
  template<class T>
  inline cycdeser & operator>>(std::vector<T> & x) {deserialize(x); return *this;}
  template<class T, size_t S>
  inline cycdeser & operator>>(std::array<T, S> & x) {deserialize(x); return *this;}

#define DESER8(T) DESER(T, )
#define DESER(T, fn_swap) inline void deserialize(T & x) { \
    align(sizeof(x)); \
    validate_size(1, sizeof(x)); \
    x = *reinterpret_cast<const T *>(data + pos); \
    if (swap_bytes) {x = fn_swap(x);} \
    pos += sizeof(x); \
}
  DESER8(char);
  DESER8(int8_t);
  DESER8(uint8_t);
  DESER(int16_t, bswap2);
  DESER(uint16_t, bswap2u);
  DESER(int32_t, bswap4);
  DESER(uint32_t, bswap4u);
  DESER(int64_t, bswap8);
  DESER(uint64_t, bswap8u);
#undef DESER

  inline void deserialize(bool & x)
  {
    unsigned char z; deserialize(z); x = (z != 0);
  }
  inline void deserialize(float & x)
  {
    deserialize(*reinterpret_cast<uint32_t *>(&x));
  }
  inline void deserialize(double & x)
  {
    deserialize(*reinterpret_cast<uint64_t *>(&x));
  }
  inline uint32_t deserialize_len(size_t el_sz)
  {
    uint32_t sz;
    deserialize(sz);
    validate_size(sz, el_sz);
    return sz;
  }
  inline void deserialize(std::string & x)
  {
    const uint32_t sz = deserialize_len(sizeof(char));
    if (sz == 0) {
      x = std::string("");
    } else {
      validate_str(sz);
      x = std::string(data + pos, sz - 1);
    }
    pos += sz;
  }
  inline void deserialize(std::wstring & x)
  {
    const uint32_t sz = deserialize_len(sizeof(wchar_t));
    // wstring is not null-terminated in cdr
    x = std::wstring(reinterpret_cast<const wchar_t *>(data + pos), sz);
    pos += sz * sizeof(wchar_t);
  }

#define DESER8_A(T) DESER_A(T, )
#define DESER_A(T, fn_swap) inline void deserializeA(T * x, size_t cnt) { \
    if (cnt > 0) { \
      align(sizeof(T)); \
      validate_size(cnt, sizeof(T)); \
      if (swap_bytes) { \
        for (size_t i = 0; i < cnt; i++) { \
          x[i] = fn_swap(*reinterpret_cast<const T *>(data + pos)); \
          pos += sizeof(T); \
        } \
      } else { \
        memcpy( \
          reinterpret_cast<void *>(x), reinterpret_cast<const void *>(data + pos), \
          cnt * sizeof(T)); \
        pos += cnt * sizeof(T); \
      } \
    } \
}
  DESER8_A(char);
  DESER8_A(int8_t);
  DESER8_A(uint8_t);
  DESER_A(int16_t, bswap2);
  DESER_A(uint16_t, bswap2u);
  DESER_A(int32_t, bswap4);
  DESER_A(uint32_t, bswap4u);
  DESER_A(int64_t, bswap8);
  DESER_A(uint64_t, bswap8u);
#undef DESER_A

  inline void deserializeA(float * x, size_t cnt)
  {
    deserializeA(reinterpret_cast<uint32_t *>(x), cnt);
  }
  inline void deserializeA(double * x, size_t cnt)
  {
    deserializeA(reinterpret_cast<uint64_t *>(x), cnt);
  }

  template<class T>
  inline void deserializeA(T * x, size_t cnt)
  {
    for (size_t i = 0; i < cnt; i++) {deserialize(x[i]);}
  }

  template<class T>
  inline void deserialize(std::vector<T> & x)
  {
    const uint32_t sz = deserialize_len(1);
    x.resize(sz);
    deserializeA(x.data(), sz);
  }
  inline void deserialize(std::vector<bool> & x)
  {
    const uint32_t sz = deserialize_len(sizeof(unsigned char));
    x.resize(sz);
    for (size_t i = 0; i < sz; i++) {
      x[i] = ((data + pos)[i] != 0);
    }
    pos += sz;
  }
  template<class T, size_t S>
  inline void deserialize(std::array<T, S> & x)
  {
    deserializeA(x.data(), x.size());
  }
};

class cycprint : cycdeserbase
{
public:
  cycprint(char * buf, size_t bufsize, const void * data, size_t size);
  cycprint() = delete;

  void print_constant(const char * x)
  {
    prtf(&buf, &bufsize, "%s", x);
  }

  inline cycprint & operator>>(bool & x) {print(x); return *this;}
  inline cycprint & operator>>(char & x) {print(x); return *this;}
  inline cycprint & operator>>(int8_t & x) {print(x); return *this;}
  inline cycprint & operator>>(uint8_t & x) {print(x); return *this;}
  inline cycprint & operator>>(int16_t & x) {print(x); return *this;}
  inline cycprint & operator>>(uint16_t & x) {print(x); return *this;}
  inline cycprint & operator>>(int32_t & x) {print(x); return *this;}
  inline cycprint & operator>>(uint32_t & x) {print(x); return *this;}
  inline cycprint & operator>>(int64_t & x) {print(x); return *this;}
  inline cycprint & operator>>(uint64_t & x) {print(x); return *this;}
  inline cycprint & operator>>(float & x) {print(x); return *this;}
  inline cycprint & operator>>(double & x) {print(x); return *this;}
  inline cycprint & operator>>(std::string & x) {print(x); return *this;}
  inline cycprint & operator>>(std::wstring & x) {print(x); return *this;}
  template<class T>
  inline cycprint & operator>>(std::vector<T> & x) {print(x); return *this;}
  template<class T, size_t S>
  inline cycprint & operator>>(std::array<T, S> & x) {print(x); return *this;}

#define PRNT8(T, F) PRNT(T, F, )
#define PRNT(T, F, fn_swap) inline void print(T & x) { \
    align(sizeof(x)); \
    validate_size(1, sizeof(x)); \
    x = *reinterpret_cast<const T *>(data + pos); \
    if (swap_bytes) {x = fn_swap(x);} \
    prtf(&buf, &bufsize, F, x); \
    pos += sizeof(x); \
}
  PRNT8(char, "'%c'");
  PRNT8(int8_t, "%" PRId8);
  PRNT8(uint8_t, "%" PRIu8);
  PRNT(int16_t, "%" PRId16, bswap2);
  PRNT(uint16_t, "%" PRIu16, bswap2u);
  PRNT(int32_t, "%" PRId32, bswap4);
  PRNT(uint32_t, "%" PRIu32, bswap4u);
  PRNT(int64_t, "%" PRId64, bswap8);
  PRNT(uint64_t, "%" PRIu64, bswap8u);
#undef PRNT

  inline void print(bool & x)
  {
    static_cast<void>(x);
    unsigned char z; print(z);
  }
  inline void print(float & x)
  {
    union { uint32_t u; float f; } tmp;
    align(sizeof(x));
    validate_size(1, sizeof(x));
    tmp.u = *reinterpret_cast<const uint32_t *>(data + pos);
    if (swap_bytes) {tmp.u = bswap4u(tmp.u);}
    static_cast<void>(tmp.u);
    prtf(&buf, &bufsize, "%f", tmp.f);
    pos += sizeof(x);
  }
  inline void print(double & x)
  {
    union { uint64_t u; double f; } tmp;
    align(sizeof(x));
    validate_size(1, sizeof(x));
    tmp.u = *reinterpret_cast<const uint64_t *>(data + pos);
    if (swap_bytes) {tmp.u = bswap8u(tmp.u);}
    static_cast<void>(tmp.u);
    prtf(&buf, &bufsize, "%f", tmp.f);
    pos += sizeof(x);
  }
  inline uint32_t get_len(size_t el_sz)
  {
    uint32_t sz;
    align(sizeof(sz));
    validate_size(1, sizeof(sz));
    sz = *reinterpret_cast<const uint32_t *>(data + pos);
    if (swap_bytes) {sz = bswap4u(sz);}
    pos += sizeof(sz);
    validate_size(sz, el_sz);
    return sz;
  }
  inline void print(std::string & x)
  {
    const uint32_t sz = get_len(sizeof(char));
    validate_str(sz);
    const int len = (sz == 0) ? 0 : (sz > INT32_MAX) ? INT32_MAX : static_cast<int>(sz - 1);
    static_cast<void>(x);
    prtf(&buf, &bufsize, "\"%*.*s\"", len, len, static_cast<const char *>(data + pos));
    pos += sz;
  }
  inline void print(std::wstring & x)
  {
    const uint32_t sz = get_len(sizeof(wchar_t));
    // wstring is not null-terminated in cdr
    x = std::wstring(reinterpret_cast<const wchar_t *>(data + pos), sz);
    prtf(&buf, &bufsize, "\"%ls\"", x.c_str());
    pos += sz * sizeof(wchar_t);
  }

  template<class T>
  inline void printA(T * x, size_t cnt)
  {
    prtf(&buf, &bufsize, "{");
    for (size_t i = 0; i < cnt; i++) {
      if (i != 0) {prtf(&buf, &bufsize, ",");}
      print(*x);
    }
    prtf(&buf, &bufsize, "}");
  }

  template<class T>
  inline void print(std::vector<T> & x)
  {
    const uint32_t sz = get_len(1);
    printA(x.data(), sz);
  }
  template<class T, size_t S>
  inline void print(std::array<T, S> & x)
  {
    printA(x.data(), x.size());
  }

private:
  static bool prtf(char * __restrict * buf, size_t * __restrict bufsize, const char * fmt, ...)
  {
    va_list ap;
    if (*bufsize == 0) {
      return false;
    }
    va_start(ap, fmt);
    int n = vsnprintf(*buf, *bufsize, fmt, ap);
    va_end(ap);
    if (n < 0) {
      **buf = 0;
      return false;
    } else if (static_cast<size_t>(n) <= *bufsize) {
      *buf += static_cast<size_t>(n);
      *bufsize -= static_cast<size_t>(n);
      return *bufsize > 0;
    } else {
      *buf += *bufsize;
      *bufsize = 0;
      return false;
    }
  }

  char * buf;
  size_t bufsize;
};

#endif  // SERDES_HPP_
