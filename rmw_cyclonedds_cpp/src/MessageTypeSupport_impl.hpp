// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
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

#ifndef MESSAGETYPESUPPORT_IMPL_HPP_
#define MESSAGETYPESUPPORT_IMPL_HPP_

#include <cassert>
#include <memory>
#include <regex>
#include <sstream>
#include <string>

#include "MessageTypeSupport.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"

namespace rmw_cyclonedds_cpp
{

template<typename MembersType>
MessageTypeSupport<MembersType>::MessageTypeSupport(const MembersType * members)
{
  assert(members);
  this->members_ = members;

  std::ostringstream ss;
  std::string message_namespace(this->members_->message_namespace_);
  std::string message_name(this->members_->message_name_);
  if (!message_namespace.empty()) {
    // Find and replace C namespace separator with C++, in case this is using C typesupport
    message_namespace = std::regex_replace(message_namespace, std::regex("__"), "::");
    ss << message_namespace << "::";
  }
  ss << "dds_::" << message_name << "_";
  this->setName(ss.str().c_str());
}

}  // namespace rmw_cyclonedds_cpp

#endif  // MESSAGETYPESUPPORT_IMPL_HPP_
