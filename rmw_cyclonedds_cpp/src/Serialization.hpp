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
#ifndef SERIALIZATION_HPP_
#define SERIALIZATION_HPP_

#include <memory>
#include <vector>

#include "TypeSupport2.hpp"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "serdata.hpp"

namespace rmw_cyclonedds_cpp
{
enum class SampleOrKey
{
  Sample,
  Key
};

enum class SampleOrRequest
{
  Sample,
  Request
};

class BaseCDRWriter
{
public:
  virtual size_t get_serialized_size(const void * data, SampleOrKey what) const = 0;
  virtual size_t get_serialized_size_estimate(const void * data, SampleOrKey what) const = 0;

  // includes 4 bytes encoding header
  virtual size_t get_min_serialized_size(SampleOrKey what) const = 0;
  // includes 4 bytes encoding header, SIZE_MAX if unbounded
  virtual size_t get_max_serialized_size(SampleOrKey what) const = 0;

  virtual void serialize(void * dest, const void * data, SampleOrKey what) const = 0;
  virtual ~BaseCDRWriter() = default;
};

std::unique_ptr<BaseCDRWriter> make_cdr_writer(
  const StructValueType * value_type,
  SampleOrRequest variant);

class BaseCDRReader
{
public:
  virtual void deserialize(
    void * dest, const void * cdr, size_t cdrsize,
    SampleOrKey what) const = 0;
  virtual void extractkey(
    std::vector<byte> & dest, const void * cdr, size_t cdrsize,
    SampleOrKey what) const = 0;
  virtual void extractkey_be(
    std::vector<byte> & dst, const void * cdr, size_t cdrsize,
    SampleOrKey what) const = 0;
  virtual size_t print(
    char * dst, size_t dstsize, const void * cdr, size_t cdrsize,
    SampleOrKey what) const =  0;

  virtual ~BaseCDRReader() = default;
};

std::unique_ptr<BaseCDRReader> make_cdr_reader(
  const StructValueType * value_type,
  SampleOrRequest variant);
}  // namespace rmw_cyclonedds_cpp

#endif  // SERIALIZATION_HPP_
