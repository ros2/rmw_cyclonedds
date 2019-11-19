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

#include <utility>

#include "rmw_cyclonedds_cpp/serdata.hpp"
#include "rosidl_generator_c/service_type_support_struct.h"

namespace rmw_cyclonedds_cpp
{
std::pair<rosidl_message_type_support_t, rosidl_message_type_support_t>
get_svc_request_response_typesupports(const rosidl_service_type_support_t & svc);

size_t get_serialized_size(
  const void * data,
  const rosidl_message_type_support_t & ts);

void serialize(
  void * dest, size_t dest_size,
  const void * data,
  const rosidl_message_type_support_t & ts);

size_t get_serialized_size(
  const cdds_request_wrapper_t & request,
  const rosidl_message_type_support_t & ts);

void serialize(
  void * dest, size_t dest_size,
  const cdds_request_wrapper_t & request,
  const rosidl_message_type_support_t & ts);


}  // namespace rmw_cyclonedds_cpp

#endif  // SERIALIZATION_HPP_
