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

// suppress definition of min/max macros on Windows.
// TODO(dan@digilabs.io): Move this closer to where Windows.h/Windef.h is included
#ifndef TRIVSERCACHE_HPP_
#define TRIVSERCACHE_HPP_

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dds/ddsi/ddsi_xqos.h"

#include "TypeSupport2.hpp"
#include "Serialization.hpp"
#include "bytewise.hpp"


namespace rmw_cyclonedds_cpp
{
class TriviallySerializedCache
{
  const size_t max_align = 8;

private:
  struct CacheKey
  {
    size_t align;
    const AnyValueType * value_type;
    bool operator==(const CacheKey & other) const
    {
      return align == other.align && value_type == other.value_type;
    }

    struct Hash
    {
      size_t operator()(const CacheKey & k) const
      {
        return std::hash<decltype(align)>{}(k.align) ^
               ((std::hash<decltype(value_type)>{}(k.value_type)) << 1U);
      }
    };
  };

public:
  explicit TriviallySerializedCache(const AnyValueType * t)
  {
    register_serializable_type(t);
  }

private:
  void register_serializable_type(const AnyValueType * t)
  {
    for (size_t align = 0; align < max_align; align++) {
      CacheKey key{align, t};
      if (cache_.find(key) != cache_.end()) {
        continue;
      }

      bool & result = cache_[key];

      switch (t->e_value_type()) {
        case EValueType::PrimitiveValueType: {
            auto tt = static_cast<const PrimitiveValueType *>(t);
            result = is_trivially_serialized(align, *tt);
          }
          break;
        case EValueType::ArrayValueType: {
            auto tt = static_cast<const ArrayValueType *>(t);
            result = compute_trivially_serialized(align, *tt);
            register_serializable_type(tt->element_value_type());
          }
          break;
        case EValueType::StructValueType: {
            auto tt = static_cast<const StructValueType *>(t);
            for (size_t i = 0; i < tt->n_members(); i++) {
              register_serializable_type(tt->get_member(i)->value_type);
            }
            result = is_trivially_serialized(align, *tt);
          }
          break;
        case EValueType::SpanSequenceValueType: {
            auto tt = static_cast<const SpanSequenceValueType *>(t);
            register_serializable_type(tt->element_value_type());
          }
          result = false;
          break;
        case EValueType::U8StringValueType:
        case EValueType::U16StringValueType:
        case EValueType::BoolVectorValueType:
          result = false;
          break;
        default:
          unreachable();
      }
    }
  }

private:
  bool is_trivially_serialized(size_t align, const StructValueType & p) const
  {
    align %= max_align;

    size_t offset = align;
    for (size_t i = 0; i < p.n_members(); i++) {
      auto m = p.get_member(i);
      if (m->member_offset != offset - align) {
        return false;
      }
      if (!compute_trivially_serialized(offset % max_align, m->value_type)) {
        return false;
      }
      offset += m->value_type->sizeof_type();
    }

    return offset == align + p.sizeof_struct();
  }

  bool is_trivially_serialized(size_t align, const PrimitiveValueType & v) const
  {
    align %= max_align;

    // Value of 0 implies it is not a primitive, which should not happen and is checked elsewhere
    const size_t cdr_alignof = v.cdralignof_type();
    assert(0 != cdr_alignof);
    if (align % cdr_alignof != 0) {
      return false;
    }
    return v.sizeof_type() == v.cdrsizeof_type();
  }

public:
  bool lookup_many_trivially_serialized(size_t align, const AnyValueType * evt) const
  {
    align %= max_align;
    // CLEVERNESS ALERT
    // we take advantage of the fact that if something is aligned at offset A and at offset A+N
    // then the alignment requirement of its elements divides A+k*N for all k
    return lookup_trivially_serialized(align, evt) &&
           lookup_trivially_serialized((align + evt->sizeof_type()) % max_align, evt);
  }

private:
  bool compute_trivially_serialized(size_t align, const ArrayValueType & v) const
  {
    auto evt = v.element_value_type();
    align %= max_align;
    // CLEVERNESS ALERT
    // we take advantage of the fact that if something is aligned at offset A and at offset A+N
    // then the alignment requirement of its elements divides A+k*N for all k
    return compute_trivially_serialized(align, evt) &&
           compute_trivially_serialized((align + evt->sizeof_type()) % max_align, evt);
  }

public:
  /// Returns true if a memcpy is all it takes to serialize this value
  bool lookup_trivially_serialized(size_t align, const AnyValueType * p) const
  {
    CacheKey key{align % max_align, p};
    return cache_.at(key);
  }

private:
  /// Returns true if a memcpy is all it takes to serialize this value
  bool compute_trivially_serialized(size_t align, const AnyValueType * p) const
  {
    align %= max_align;

    bool result;
    switch (p->e_value_type()) {
      case EValueType::PrimitiveValueType:
        result = is_trivially_serialized(align, *static_cast<const PrimitiveValueType *>(p));
        break;
      case EValueType::StructValueType:
        result = is_trivially_serialized(align, *static_cast<const StructValueType *>(p));
        break;
      case EValueType::ArrayValueType:
        result = compute_trivially_serialized(align, *static_cast<const ArrayValueType *>(p));
        break;
      case EValueType::U8StringValueType:
      case EValueType::U16StringValueType:
      case EValueType::SpanSequenceValueType:
      case EValueType::BoolVectorValueType:
        result = false;
        break;
      default:
        unreachable();
    }
    return result;
  }

  std::unordered_map<CacheKey, bool, CacheKey::Hash> cache_;
};
}  // namespace rmw_cyclonedds_cpp

#endif  // TRIVSERCACHE_HPP_
