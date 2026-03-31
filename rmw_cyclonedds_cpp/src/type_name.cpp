// Copyright 2019 ADLINK Technology Limited.
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

#include <cassert>
#include <cstring>
#include <string>
#include <regex>
#include <sstream>

#include "rmw/error_handling.h"

#include "rosidl_typesupport_introspection_c/identifier.h"
#include "rosidl_typesupport_introspection_c/message_introspection.h"
#include "rosidl_typesupport_introspection_c/service_introspection.h"
#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"

#include "type_name.hpp"

static bool using_introspection_c_typesupport(const char * typesupport_identifier)
{
  return strcmp(
    typesupport_identifier,
    rosidl_typesupport_introspection_c__identifier) == 0;
}

static bool using_introspection_cpp_typesupport(const char * typesupport_identifier)
{
  return strcmp(
    typesupport_identifier,
    rosidl_typesupport_introspection_cpp::typesupport_identifier) == 0;
}

std::string get_type_name_impl(
  const std::string & message_namespace, const std::string & message_name,
  const std::string & suffix)
{
  // "::" + "dds_::" + "_"
  const size_t fixed_reserve = 5 + message_name.size() + 1 + suffix.size();
  std::string name;
  if (message_namespace.size() != 0) {
    std::string mangled_message_namespace(message_namespace);
    // Find and replace C namespace separator with C++, in case this is using C typesupport
    std::string::size_type pos = 0;
    while ((pos = mangled_message_namespace.find("__", pos)) != std::string::npos) {
      mangled_message_namespace.replace(pos, 2, "::");
      pos += 2;
    }
    name.reserve(
      mangled_message_namespace.size() + 2 + fixed_reserve);
    name += mangled_message_namespace;
    name += "::";
  } else {
    name.reserve(fixed_reserve);
  }
  name += "dds_::";
  name += message_name;
  name += '_';
  name += suffix;
  return name;
}

template<typename MembersType>
static std::string get_message_type_name_impl(MembersType * members)
{
  return get_type_name_impl(members->message_namespace_, members->message_name_, std::string(""));
}
template<typename MembersType>
static std::string get_request_type_name_impl(MembersType * members)
{
  return get_type_name_impl(
    members->service_namespace_, members->service_name_,
    std::string("Request_"));
}
template<typename MembersType>
static std::string get_response_type_name_impl(MembersType * members)
{
  return get_type_name_impl(
    members->service_namespace_, members->service_name_,
    std::string("Response_"));
}

std::string get_message_type_name(const rosidl_message_type_support_t * type_support)
{
  if (using_introspection_c_typesupport(type_support->typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(type_support->data);
    return get_message_type_name_impl(members);
  } else if (using_introspection_cpp_typesupport(type_support->typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(type_support->data);
    return get_message_type_name_impl(members);
  }
  RMW_SET_ERROR_MSG("Unknown typesupport identifier");
  return nullptr;
}

std::string get_request_type_name(const rosidl_service_type_support_t * type_support)
{
  if (using_introspection_c_typesupport(type_support->typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(type_support->data);
    return get_request_type_name_impl(members);
  } else if (using_introspection_cpp_typesupport(type_support->typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *>(type_support->data);
    return get_request_type_name_impl(members);
  }
  RMW_SET_ERROR_MSG("Unknown typesupport identifier");
  return nullptr;
}

std::string get_response_type_name(const rosidl_service_type_support_t * type_support)
{
  if (using_introspection_c_typesupport(type_support->typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(type_support->data);
    return get_response_type_name_impl(members);
  } else if (using_introspection_cpp_typesupport(type_support->typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *>(type_support->data);
    return get_response_type_name_impl(members);
  }
  RMW_SET_ERROR_MSG("Unknown typesupport identifier");
  return nullptr;
}
