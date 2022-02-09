// Copyright 2021 Apex.AI, Inc.
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

#include "TypeSupport.hpp"

#include "rosidl_runtime_c/string.h"
#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_c/identifier.h"
#include "rosidl_typesupport_introspection_c/message_introspection.h"

#include "serdes.hpp"

namespace rmw_cyclonedds_cpp
{

size_t get_message_size(
  const rosidl_message_type_support_t * type_supports)
{
  // handling C++ typesupport
  const rosidl_message_type_support_t * ts = get_message_typesupport_handle(
    type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier);
  if (ts != nullptr) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(ts->data);
    return members->size_of_;
  } else {
    // handle C Typesupport
    const rosidl_message_type_support_t * ts_c = get_message_typesupport_handle(
      type_supports, rosidl_typesupport_introspection_c__identifier);
    if (ts_c != nullptr) {
      auto members =
        static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(ts_c->data);
      return members->size_of_;
    } else {
      throw std::runtime_error("get_message_size, unsupported typesupport");
    }
  }
}

void init_message(
  const rosidl_message_type_support_t * type_supports,
  void * message)
{
  // handling C++ typesupport
  const rosidl_message_type_support_t * ts = get_message_typesupport_handle(
    type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier);
  if (ts != nullptr) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(ts->data);
    members->init_function(message, rosidl_runtime_cpp::MessageInitialization::ALL);
  } else {
    // handle C Typesupport
    const rosidl_message_type_support_t * ts_c = get_message_typesupport_handle(
      type_supports, rosidl_typesupport_introspection_c__identifier);
    if (ts_c != nullptr) {
      auto members =
        static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(ts_c->data);
      members->init_function(message, ROSIDL_RUNTIME_C_MSG_INIT_ALL);
    } else {
      throw std::runtime_error("get_message_size, unsupported typesupport");
    }
  }
}

void fini_message(
  const rosidl_message_type_support_t * type_supports,
  void * message)
{
  // handling C++ typesupport
  const rosidl_message_type_support_t * ts = get_message_typesupport_handle(
    type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier);
  if (ts != nullptr) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(ts->data);
    members->fini_function(message);
  } else {
    // handle C Typesupport
    const rosidl_message_type_support_t * ts_c = get_message_typesupport_handle(
      type_supports, rosidl_typesupport_introspection_c__identifier);
    if (ts_c != nullptr) {
      auto members =
        static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(ts_c->data);
      members->fini_function(message);
    } else {
      throw std::runtime_error("get_message_size, unsupported typesupport");
    }
  }
}

}  // namespace rmw_cyclonedds_cpp
