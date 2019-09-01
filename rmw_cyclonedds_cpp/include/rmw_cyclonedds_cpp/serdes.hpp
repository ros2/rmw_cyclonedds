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

#ifndef RMW_CYCLONEDDS_CPP__SERDES_HPP_
#define RMW_CYCLONEDDS_CPP__SERDES_HPP_

#include <string.h>
#include <inttypes.h>
#include <stdarg.h>

#include <array>
#include <string>
#include <vector>
#include <type_traits>

class cycser
{
public:
  explicit cycser(std::vector<unsigned char> & dst_);
  cycser() = delete;

  inline cycser & operator<<(bool x) {serialize(x); return *this;}
  inline cycser & operator<<(char x) {serialize(x); return *this;}
  inline cycser & operator<<(int8_t x) {serialize(x); return *this;}
  inline cycser & operator<<(uint8_t x) {serialize(x); return *this;}
  inline cycser & operator<<(int16_t x) {serialize(x); return *this;}
  inline cycser & operator<<(uint16_t x) {serialize(x); return *this;}
  inline cycser & operator<<(int32_t x) {serialize(x); return *this;}
  inline cycser & operator<<(uint32_t x) {serialize(x); return *this;}
  inline cycser & operator<<(int64_t x) {serialize(x); return *this;}
  inline cycser & operator<<(uint64_t x) {serialize(x); return *this;}
  inline cycser & operator<<(float x) {serialize(x); return *this;}
  inline cycser & operator<<(double x) {serialize(x); return *this;}
  inline cycser & operator<<(const char * x) {serialize(x); return *this;}
  inline cycser & operator<<(const std::string & x) {serialize(x); return *this;}
  inline cycser & operator<<(const std::wstring & x) {serialize(x); return *this;}
  template<class T>
  inline cycser & operator<<(const std::vector<T> & x) {serialize(x); return *this;}
  template<class T, size_t S>
  inline cycser & operator<<(const std::array<T, S> & x) {serialize(x); return *this;}

#define SIMPLE(T) inline void serialize(T x) { \
    if ((off % sizeof(T)) != 0) { \
      off += sizeof(T) - (off % sizeof(T)); \
    } \
    resize(off + sizeof(T)); \
    *(reinterpret_cast<T *>(data() + off)) = x; \
    off += sizeof(T); \
}
  SIMPLE(char);
  SIMPLE(int8_t);
  SIMPLE(uint8_t);
  SIMPLE(int16_t);
  SIMPLE(uint16_t);
  SIMPLE(int32_t);
  SIMPLE(uint32_t);
  SIMPLE(int64_t);
  SIMPLE(uint64_t);
  SIMPLE(float);
  SIMPLE(double);
#undef SIMPLE

  inline void serialize(bool x)
  {
    serialize(static_cast<unsigned char>(x));
  }
  inline void serialize(const char * x)
  {
    size_t sz = strlen(x) + 1;
    serialize(static_cast<uint32_t>(sz));
    resize(off + sz);
    memcpy(data() + off, x, sz);
    off += sz;
  }
  inline void serialize(const std::string & x)
  {
    size_t sz = x.size() + 1;
    serialize(static_cast<uint32_t>(sz));
    resize(off + sz);
    memcpy(data() + off, x.c_str(), sz);
    off += sz;
  }
  inline void serialize(const std::wstring & x)
  {
    size_t sz = x.size();
    serialize(static_cast<uint32_t>(sz));
    resize(off + sz * sizeof(wchar_t));
    memcpy(data() + off, reinterpret_cast<const char *>(x.c_str()), sz * sizeof(wchar_t));
    off += sz * sizeof(wchar_t);
  }

#define SIMPLEA(T) inline void serializeA(const T * x, size_t cnt) { \
    if (cnt > 0) { \
      if ((off % sizeof(T)) != 0) { \
        off += sizeof(T) - (off % sizeof(T)); \
      } \
      resize(off + cnt * sizeof(T)); \
      memcpy(data() + off, reinterpret_cast<const void *>(x), cnt * sizeof(T)); \
      off += cnt * sizeof(T); \
    } \
}
  SIMPLEA(char);
  SIMPLEA(int8_t);
  SIMPLEA(uint8_t);
  SIMPLEA(int16_t);
  SIMPLEA(uint16_t);
  SIMPLEA(int32_t);
  SIMPLEA(uint32_t);
  SIMPLEA(int64_t);
  SIMPLEA(uint64_t);
  SIMPLEA(float);
  SIMPLEA(double);
#undef SIMPLEA
  template<class T>
  inline void serializeA(const T * x, size_t cnt)
  {
    for (size_t i = 0; i < cnt; i++) {serialize(x[i]);}
  }

  template<class T>
  inline void serialize(const std::vector<T> & x)
  {
    serialize(static_cast<uint32_t>(x.size()));
    serializeA(x.data(), x.size());
  }
  inline void serialize(const std::vector<bool> & x)
  {
    serialize(static_cast<uint32_t>(x.size()));
    for (auto && i : x) {serialize(i);}
  }

  template<class T>
  inline void serializeS(const T * x, size_t cnt)
  {
    serialize(static_cast<uint32_t>(cnt));
    serializeA(x, cnt);
  }

private:
  inline void resize(size_t n) {dst.resize(n + 4);}
  inline unsigned char * data() {return dst.data() + 4;}

  std::vector<unsigned char> & dst;
  size_t off;
};

class cycdeser
{
public:
  // FIXME: byteswapping
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
  inline cycdeser & operator>>(char * & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(std::string & x) {deserialize(x); return *this;}
  inline cycdeser & operator>>(std::wstring & x) {deserialize(x); return *this;}
  template<class T>
  inline cycdeser & operator>>(std::vector<T> & x) {deserialize(x); return *this;}
  template<class T, size_t S>
  inline cycdeser & operator>>(std::array<T, S> & x) {deserialize(x); return *this;}

#define SIMPLE(T) inline void deserialize(T & x) { \
    align(sizeof(x)); \
    x = *reinterpret_cast<const T *>(data + pos); \
    pos += sizeof(x); \
}
  SIMPLE(char);
  SIMPLE(int8_t);
  SIMPLE(uint8_t);
  SIMPLE(int16_t);
  SIMPLE(uint16_t);
  SIMPLE(int32_t);
  SIMPLE(uint32_t);
  SIMPLE(int64_t);
  SIMPLE(uint64_t);
  SIMPLE(float);
  SIMPLE(double);
#undef SIMPLE

  inline void deserialize(bool & x)
  {
    unsigned char z; deserialize(z); x = (z != 0);
  }
  inline uint32_t deserialize32()
  {
    uint32_t sz; deserialize(sz); return sz;
  }
  inline void deserialize(char * & x)
  {
    const uint32_t sz = deserialize32();
    x = reinterpret_cast<char *>(malloc(sz));
    memcpy(x, data + pos, sz);
    pos += sz;
  }
  inline void deserialize(std::string & x)
  {
    const uint32_t sz = deserialize32();
    if (sz == 0) {
      x = std::string("");
    } else {
      x = std::string(data + pos, sz - 1);
    }
    pos += sz;
  }
  inline void deserialize(std::wstring & x)
  {
    const uint32_t sz = deserialize32();
    x = std::wstring(reinterpret_cast<const wchar_t *>(data + pos), sz);
    pos += sz * sizeof(wchar_t);
  }

#define SIMPLEA(T) inline void deserializeA(T * x, size_t cnt) { \
    if (cnt > 0) { \
      align(sizeof(T)); \
      memcpy(reinterpret_cast<void *>(x), reinterpret_cast<const void *>(data + pos), \
        (cnt) * sizeof(T)); \
      pos += (cnt) * sizeof(T); \
    } \
}
  SIMPLEA(char);
  SIMPLEA(int8_t);
  SIMPLEA(uint8_t);
  SIMPLEA(int16_t);
  SIMPLEA(uint16_t);
  SIMPLEA(int32_t);
  SIMPLEA(uint32_t);
  SIMPLEA(int64_t);
  SIMPLEA(uint64_t);
  SIMPLEA(float);
  SIMPLEA(double);
#undef SIMPLEA
  template<class T>
  inline void deserializeA(T * x, size_t cnt)
  {
    for (size_t i = 0; i < cnt; i++) {deserialize(x[i]);}
  }

  template<class T>
  inline void deserialize(std::vector<T> & x)
  {
    const uint32_t sz = deserialize32();
    x.resize(sz);
    deserializeA(x.data(), sz);
  }
  inline void deserialize(std::vector<bool> & x)
  {
    const uint32_t sz = deserialize32();
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

private:
  inline void align(size_t a)
  {
    if ((pos % a) != 0) {
      pos += a - (pos % a);
    }
  }

  const char * data;
  size_t pos;
  size_t lim;   // ignored for now ... better provide correct input
};

class cycprint
{
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
    } else if ((size_t) n <= *bufsize) {
      *buf += (size_t) n;
      *bufsize -= (size_t) n;
      return *bufsize > 0;
    } else {
      *buf += *bufsize;
      *bufsize = 0;
      return false;
    }
  }

public:
  // FIXME: byteswapping
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
  inline cycprint & operator>>(char * & x) {print(x); return *this;}
  inline cycprint & operator>>(std::string & x) {print(x); return *this;}
  inline cycprint & operator>>(std::wstring & x) {print(x); return *this;}
  template<class T>
  inline cycprint & operator>>(std::vector<T> & x) {print(x); return *this;}
  template<class T, size_t S>
  inline cycprint & operator>>(std::array<T, S> & x) {print(x); return *this;}

#define SIMPLE(T, F) inline void print(T & x) { \
    align(sizeof(x)); \
    x = *reinterpret_cast<const T *>(data + pos); \
    prtf(&buf, &bufsize, F, x); \
    pos += sizeof(x); \
}
  SIMPLE(char, "'%c'");
  SIMPLE(int8_t, "%" PRId8);
  SIMPLE(uint8_t, "%" PRIu8);
  SIMPLE(int16_t, "%" PRId16);
  SIMPLE(uint16_t, "%" PRIu16);
  SIMPLE(int32_t, "%" PRId32);
  SIMPLE(uint32_t, "%" PRIu32);
  SIMPLE(int64_t, "%" PRId64);
  SIMPLE(uint64_t, "%" PRIu64);
  SIMPLE(float, "%f");
  SIMPLE(double, "%f");
#undef SIMPLE

  inline void print(bool & x)
  {
    static_cast<void>(x);
    unsigned char z; print(z);
  }
  inline uint32_t get32()
  {
    uint32_t sz;
    align(sizeof(sz));
    sz = *reinterpret_cast<const uint32_t *>(data + pos);
    pos += sizeof(sz);
    return sz;
  }
  inline void print(char * & x)
  {
    const uint32_t sz = get32();
    const int len = (sz == 0) ? 0 : (sz > INT32_MAX) ? INT32_MAX : static_cast<int>(sz - 1);
    static_cast<void>(x);
    prtf(&buf, &bufsize, "\"%*.*s\"", len, len, static_cast<const char *>(data + pos));
    pos += sz;
  }
  inline void print(std::string & x)
  {
    const uint32_t sz = get32();
    const int len = (sz == 0) ? 0 : (sz > INT32_MAX) ? INT32_MAX : static_cast<int>(sz - 1);
    static_cast<void>(x);
    prtf(&buf, &bufsize, "\"%*.*s\"", len, len, static_cast<const char *>(data + pos));
    pos += sz;
  }
  inline void print(std::wstring & x)
  {
    const uint32_t sz = get32();
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
    const uint32_t sz = get32();
    printA(x.data(), sz);
  }
  template<class T, size_t S>
  inline void print(std::array<T, S> & x)
  {
    printA(x.data(), x.size());
  }

private:
  inline void align(size_t a)
  {
    if ((pos % a) != 0) {
      pos += a - (pos % a);
    }
  }

  const char * data;
  size_t pos;
  char * buf;
  size_t bufsize;
};

#endif  // RMW_CYCLONEDDS_CPP__SERDES_HPP_
