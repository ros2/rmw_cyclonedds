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

#include "TypeSupport2.hpp"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "serdata.hpp"

namespace rmw_cyclonedds_cpp
{

class BaseCDRWriter
{
public:
  virtual size_t get_serialized_size(const void * data) const = 0;
  virtual void serialize(void * dest, const void * data) const = 0;
  virtual size_t get_serialized_size(const cdds_request_wrapper_t & request) const = 0;
  virtual void serialize(void * dest, const cdds_request_wrapper_t & request) const = 0;
  virtual ~BaseCDRWriter() = default;
};

std::unique_ptr<BaseCDRWriter> make_cdr_writer(std::unique_ptr<StructValueType> value_type);
}  // namespace rmw_cyclonedds_cpp

#endif  // SERIALIZATION_HPP_
