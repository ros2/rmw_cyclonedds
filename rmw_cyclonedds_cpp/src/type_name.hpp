// Copyright 2019 ADLINK Technology
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
#ifndef TYPE_NAME_HPP_
#define TYPE_NAME_HPP_

#include <string>

#include "rosidl_runtime_c/message_type_support_struct.h"
#include "rosidl_runtime_c/service_type_support_struct.h"

std::string get_type_name_impl(
  const std::string & message_namespace, const std::string & message_name,
  const std::string & suffix);

std::string get_message_type_name(const rosidl_message_type_support_t * type_support);
std::string get_request_type_name(const rosidl_service_type_support_t * type_support);
std::string get_response_type_name(const rosidl_service_type_support_t * type_support);

#endif  // TYPE_NAME_HPP_
