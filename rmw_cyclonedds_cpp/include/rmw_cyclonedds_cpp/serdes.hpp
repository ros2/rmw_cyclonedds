// Copyright 2018 ADLINK Technology
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

#ifndef SERDES_HPP
#define SERDES_HPP

#include <array>
#include <string>
#include <vector>
#include <type_traits>

#include "ddsc/dds.h"

extern "C" {
  struct serstate;
  struct serdata;
  struct sertopic;
  
  struct serdata *ddsi_serdata_ref(struct serdata *serdata);
  void ddsi_serdata_unref(struct serdata *serdata);
  struct serstate *ddsi_serstate_new(const struct sertopic *topic);
  struct serdata *ddsi_serstate_fix(struct serstate *st);
  void ddsi_serstate_release(struct serstate *st);
  void *ddsi_serstate_append(struct serstate *st, size_t n);
  void *ddsi_serstate_append_align(struct serstate *st, size_t a);
  void *ddsi_serstate_append_aligned(struct serstate *st, size_t n, size_t a);
  void ddsi_serdata_getblob(void **raw, size_t *sz, struct serdata *serdata);
}

template<class T>
struct simpletype : std::integral_constant< bool, std::is_arithmetic<T>::value && !std::is_same<bool, T>::value > {};

template<class T>
struct complextype : std::integral_constant< bool, !(std::is_arithmetic<T>::value && !std::is_same<bool, T>::value) > {};

template<unsigned N> struct rank : rank<N-1> {};
template<> struct rank<0> {};

class cycser {
public:
  cycser(struct sertopic *topic);
  cycser() = delete;
  ~cycser();
  cycser& fix();
  cycser& ref();
  void unref();
  struct serdata *data();

  inline cycser& operator<<(bool x) { serialize(x); return *this; }
  inline cycser& operator<<(char x) { serialize(x); return *this; }
  inline cycser& operator<<(uint8_t x) { serialize(x); return *this; }
  inline cycser& operator<<(int16_t x) { serialize(x); return *this; }
  inline cycser& operator<<(uint16_t x) { serialize(x); return *this; }
  inline cycser& operator<<(int32_t x) { serialize(x); return *this; }
  inline cycser& operator<<(uint32_t x) { serialize(x); return *this; }
  inline cycser& operator<<(int64_t x) { serialize(x); return *this; }
  inline cycser& operator<<(uint64_t x) { serialize(x); return *this; }
  inline cycser& operator<<(float x) { serialize(x); return *this; }
  inline cycser& operator<<(double x) { serialize(x); return *this; }
  inline cycser& operator<<(const char *x) { serialize(x); return *this; }
  inline cycser& operator<<(const std::string& x) { serialize(x); return *this; }
  template<class T> inline cycser& operator<<(const std::vector<T>& x) { serialize(x); return *this; }
  template<class T, size_t S> inline cycser& operator<<(const std::array<T, S>& x) { serialize(x); return *this; }

#define SIMPLE(T) inline void serialize(T x) {                          \
    T *p = (T *)ddsi_serstate_append_aligned(st, sizeof(T), sizeof(T)); \
    *p = x;                                                             \
  }
  SIMPLE(char);
  SIMPLE(unsigned char);
  SIMPLE(int16_t);
  SIMPLE(uint16_t);
  SIMPLE(int32_t);
  SIMPLE(uint32_t);
  SIMPLE(int64_t);
  SIMPLE(uint64_t);
  SIMPLE(float);
  SIMPLE(double);
#undef SIMPLE
  
  inline void serialize(bool x) {
    serialize(static_cast<unsigned char>(x));
  }
  inline void serialize(const char *x)
  {
    size_t sz = strlen(x) + 1;
    serialize(static_cast<uint32_t>(sz));
    char *p = (char *)ddsi_serstate_append(st, sz);
    memcpy(p, x, sz);
  }
  inline void serialize(const std::string& x)
  {
    size_t sz = x.size();
    serialize(static_cast<uint32_t>(sz+1));
    char *p = (char *)ddsi_serstate_append(st, sz+1);
    memcpy(p, x.data(), sz);
    p[sz] = 0;
  }

#define SIMPLEA(T) inline void serializeA(const T *x, size_t cnt) {     \
    void *p = (void *)ddsi_serstate_append_aligned(st, (cnt) * sizeof(T), sizeof(T)); \
    memcpy(p, (void *)x, (cnt) * sizeof(T));                            \
  }
  SIMPLEA(char);
  SIMPLEA(unsigned char);
  SIMPLEA(int16_t);
  SIMPLEA(uint16_t);
  SIMPLEA(int32_t);
  SIMPLEA(uint32_t);
  SIMPLEA(int64_t);
  SIMPLEA(uint64_t);
  SIMPLEA(float);
  SIMPLEA(double);
#undef SIMPLEA
  template<class T> inline void serializeA(const T *x, size_t cnt) {
    for (size_t i = 0; i < cnt; i++) serialize(x[i]);
  }
  
  template<class T> inline void serialize(const std::vector<T>& x)
  {
    serialize(static_cast<uint32_t>(x.size()));
    if (x.size() > 0) serializeA(x.data(), x.size());
  }
  inline void serialize(const std::vector<bool>& x) {
    serialize(static_cast<uint32_t>(x.size()));
    for (auto&& i : x) serialize(i);
  }

  template<class T> inline void serializeS(const T *x, size_t cnt)
  {
    serialize(static_cast<uint32_t>(cnt));
    if (cnt > 0) serializeA(x, cnt);
  }

private:
  struct serstate *st;
  struct serdata *sd;
};

class cycdeser {
public:
  // FIXME: byteswapping
  cycdeser(const void *data, size_t size);
  cycdeser() = delete;
  ~cycdeser();

  inline cycdeser& operator>>(bool& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(char& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(uint8_t& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(int16_t& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(uint16_t& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(int32_t& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(uint32_t& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(int64_t& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(uint64_t& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(float& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(double& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(char *& x) { deserialize(x); return *this; }
  inline cycdeser& operator>>(std::string& x) { deserialize(x); return *this; }
  template<class T> inline cycdeser& operator>>(std::vector<T>& x) { deserialize(x); return *this; }
  template<class T, size_t S> inline cycdeser& operator>>(std::array<T, S>& x) { deserialize(x); return *this; }

#define SIMPLE(T) inline void deserialize(T& x) {       \
    align(sizeof(x));                                   \
    x = *reinterpret_cast<const T *>(data);             \
    data += sizeof(x);                                  \
  }
  SIMPLE(char);
  SIMPLE(unsigned char);
  SIMPLE(int16_t);
  SIMPLE(uint16_t);
  SIMPLE(int32_t);
  SIMPLE(uint32_t);
  SIMPLE(int64_t);
  SIMPLE(uint64_t);
  SIMPLE(float);
  SIMPLE(double);
#undef SIMPLE
  
  inline void deserialize(bool& x) {
    unsigned char z; deserialize(z); x = (z != 0);
  }
  inline uint32_t deserialize32() {
    uint32_t sz; deserialize(sz); return sz;
  }
  inline void deserialize(char *& x) {
    const uint32_t sz = deserialize32(); x = (char *)malloc(sz); memcpy(x, data, sz); data += sz;
  }
  inline void deserialize(std::string& x) {
    const uint32_t sz = deserialize32(); x = std::string(data, sz-1); data += sz;
  }

#define SIMPLEA(T) inline void deserializeA(T *x, size_t cnt) {         \
    align(sizeof(T));                                                   \
    memcpy((void *)x, (void *)data, (cnt) * sizeof(T));                 \
    data += (cnt) * sizeof(T);                                          \
  }
  SIMPLEA(char);
  SIMPLEA(unsigned char);
  SIMPLEA(int16_t);
  SIMPLEA(uint16_t);
  SIMPLEA(int32_t);
  SIMPLEA(uint32_t);
  SIMPLEA(int64_t);
  SIMPLEA(uint64_t);
  SIMPLEA(float);
  SIMPLEA(double);
#undef SIMPLEA
  template<class T> inline void deserializeA(T *x, size_t cnt) {
    for (size_t i = 0; i < cnt; i++) deserialize(x[i]);
  }
  
  template<class T> inline void deserialize(std::vector<T>& x) {
    const uint32_t sz = deserialize32();
    x.resize(sz);
    deserializeA(x.data(), sz);
  }
  inline void deserialize(std::vector<bool>& x) {
    const uint32_t sz = deserialize32();
    x.resize(sz);
    for (auto&& i : x) i = (data[i] != 0);
    data += sz;
  }
  template<class T, size_t S> inline void deserialize(std::array<T, S>& x) {
    deserializeA(x.data(), x.size());
  }
  
private:
  inline void align(size_t a)
  {
    const uintptr_t x = reinterpret_cast<uintptr_t>(data);
    if ((x % a) != 0) {
      const uintptr_t pad = a - (x % a);
      data = reinterpret_cast<const char *>(x + pad);
    }
  }

  const char *data;
  size_t pos;
  size_t lim; // ignored for now ... better provide correct input
};

#endif
