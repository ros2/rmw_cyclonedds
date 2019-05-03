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

class cycser {
public:
    cycser (std::vector<unsigned char>& dst_);
    cycser () = delete;

    inline cycser& operator<< (bool x) { serialize (x); return *this; }
    inline cycser& operator<< (char x) { serialize (x); return *this; }
    inline cycser& operator<< (uint8_t x) { serialize (x); return *this; }
    inline cycser& operator<< (int16_t x) { serialize (x); return *this; }
    inline cycser& operator<< (uint16_t x) { serialize (x); return *this; }
    inline cycser& operator<< (int32_t x) { serialize (x); return *this; }
    inline cycser& operator<< (uint32_t x) { serialize (x); return *this; }
    inline cycser& operator<< (int64_t x) { serialize (x); return *this; }
    inline cycser& operator<< (uint64_t x) { serialize (x); return *this; }
    inline cycser& operator<< (float x) { serialize (x); return *this; }
    inline cycser& operator<< (double x) { serialize (x); return *this; }
    inline cycser& operator<< (const char *x) { serialize (x); return *this; }
    inline cycser& operator<< (const std::string& x) { serialize (x); return *this; }
    template<class T> inline cycser& operator<< (const std::vector<T>& x) { serialize (x); return *this; }
    template<class T, size_t S> inline cycser& operator<< (const std::array<T, S>& x) { serialize (x); return *this; }

#define SIMPLE(T) inline void serialize (T x) {         \
        if ((off % sizeof (T)) != 0) {                  \
            off += sizeof (T) - (off % sizeof (T));     \
        }                                               \
        resize (off + sizeof (T));                      \
        *(T *) (data () + off) = x;                     \
        off += sizeof (T);                              \
    }
    SIMPLE (char);
    SIMPLE (unsigned char);
    SIMPLE (int16_t);
    SIMPLE (uint16_t);
    SIMPLE (int32_t);
    SIMPLE (uint32_t);
    SIMPLE (int64_t);
    SIMPLE (uint64_t);
    SIMPLE (float);
    SIMPLE (double);
#undef SIMPLE
  
    inline void serialize (bool x) {
        serialize (static_cast<unsigned char> (x));
    }
    inline void serialize (const char *x)
    {
        size_t sz = strlen (x) + 1;
        serialize (static_cast<uint32_t> (sz));
        resize (off + sz);
        memcpy (data () + off, x, sz);
        off += sz;
    }
    inline void serialize (const std::string& x)
    {
        size_t sz = x.size () + 1;
        serialize (static_cast<uint32_t> (sz));
        resize (off + sz);
        memcpy (data () + off, x.data (), sz - 1);
        *(data () + off + sz - 1) = 0;
    }

#define SIMPLEA(T) inline void serializeA (const T *x, size_t cnt) {    \
        if ((off % sizeof (T)) != 0) {                                  \
            off += sizeof (T) - (off % sizeof (T));                     \
        }                                                               \
        resize (off + cnt * sizeof (T));                                \
        memcpy (data () + off, (void *) x, cnt * sizeof (T));           \
        off += cnt * sizeof (T);                                        \
    }
    SIMPLEA (char);
    SIMPLEA (unsigned char);
    SIMPLEA (int16_t);
    SIMPLEA (uint16_t);
    SIMPLEA (int32_t);
    SIMPLEA (uint32_t);
    SIMPLEA (int64_t);
    SIMPLEA (uint64_t);
    SIMPLEA (float);
    SIMPLEA (double);
#undef SIMPLEA
    template<class T> inline void serializeA (const T *x, size_t cnt) {
        for (size_t i = 0; i < cnt; i++) serialize (x[i]);
    }
  
    template<class T> inline void serialize (const std::vector<T>& x)
    {
        serialize (static_cast<uint32_t> (x.size ()));
        if (x.size () > 0) serializeA (x.data (), x.size ());
    }
    inline void serialize (const std::vector<bool>& x) {
        serialize (static_cast<uint32_t> (x.size ()));
        for (auto&& i : x) serialize (i);
    }

    template<class T> inline void serializeS (const T *x, size_t cnt)
    {
        serialize (static_cast<uint32_t> (cnt));
        if (cnt > 0) serializeA (x, cnt);
    }

private:
    inline void resize (size_t n) { dst.resize (n + 4); }
    inline unsigned char *data () { return dst.data () + 4; }
    
    std::vector<unsigned char>& dst;
    size_t off;
};

class cycdeser {
public:
    // FIXME: byteswapping
    cycdeser (const void *data, size_t size);
    cycdeser () = delete;

    inline cycdeser& operator>> (bool& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (char& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (uint8_t& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (int16_t& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (uint16_t& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (int32_t& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (uint32_t& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (int64_t& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (uint64_t& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (float& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (double& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (char *& x) { deserialize (x); return *this; }
    inline cycdeser& operator>> (std::string& x) { deserialize (x); return *this; }
    template<class T> inline cycdeser& operator>> (std::vector<T>& x) { deserialize (x); return *this; }
    template<class T, size_t S> inline cycdeser& operator>> (std::array<T, S>& x) { deserialize (x); return *this; }

#define SIMPLE(T) inline void deserialize (T& x) {      \
        align (sizeof (x));                             \
        x = *reinterpret_cast<const T *> (data + pos);  \
        pos += sizeof (x);                              \
    }
    SIMPLE (char);
    SIMPLE (unsigned char);
    SIMPLE (int16_t);
    SIMPLE (uint16_t);
    SIMPLE (int32_t);
    SIMPLE (uint32_t);
    SIMPLE (int64_t);
    SIMPLE (uint64_t);
    SIMPLE (float);
    SIMPLE (double);
#undef SIMPLE
  
    inline void deserialize (bool& x) {
        unsigned char z; deserialize (z); x = (z != 0);
    }
    inline uint32_t deserialize32 () {
        uint32_t sz; deserialize (sz); return sz;
    }
    inline void deserialize (char *& x) {
        const uint32_t sz = deserialize32 ();
        x = (char *) malloc (sz);
        memcpy (x, data + pos, sz);
        pos += sz;
    }
    inline void deserialize (std::string& x) {
        const uint32_t sz = deserialize32 ();
        x = std::string (data + pos, sz-1);
        pos += sz;
    }

#define SIMPLEA(T) inline void deserializeA (T *x, size_t cnt) {        \
        align (sizeof (T));                                             \
        memcpy ((void *) x, (void *) (data + pos), (cnt) * sizeof (T)); \
        pos += (cnt) * sizeof (T);                                      \
    }
    SIMPLEA (char);
    SIMPLEA (unsigned char);
    SIMPLEA (int16_t);
    SIMPLEA (uint16_t);
    SIMPLEA (int32_t);
    SIMPLEA (uint32_t);
    SIMPLEA (int64_t);
    SIMPLEA (uint64_t);
    SIMPLEA (float);
    SIMPLEA (double);
#undef SIMPLEA
    template<class T> inline void deserializeA (T *x, size_t cnt) {
        for (size_t i = 0; i < cnt; i++) deserialize (x[i]);
    }
  
    template<class T> inline void deserialize (std::vector<T>& x) {
        const uint32_t sz = deserialize32 ();
        x.resize (sz);
        deserializeA (x.data (), sz);
    }
    inline void deserialize (std::vector<bool>& x) {
        const uint32_t sz = deserialize32 ();
        x.resize (sz);
        for (auto&& i : x) i = ((data + pos)[i] != 0);
        pos += sz;
    }
    template<class T, size_t S> inline void deserialize (std::array<T, S>& x) {
        deserializeA (x.data (), x.size ());
    }
  
private:
    inline void align (size_t a)
    {
        if ((pos % a) != 0) {
            pos += a - (pos % a);
        }
    }

    const char *data;
    size_t pos;
    size_t lim; // ignored for now ... better provide correct input
};

#endif
