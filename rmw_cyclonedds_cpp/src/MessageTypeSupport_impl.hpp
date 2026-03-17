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

  std::string message_name(this->members_->message_name_);

  std::string name;
  if (this->members_->message_namespace_ != nullptr &&
    this->members_->message_namespace_[0] != '\0')
  {
    std::string message_namespace(this->members_->message_namespace_);
    // Find and replace C namespace separator with C++, in case this is using C typesupport
    std::string::size_type pos = 0;
    while ((pos = message_namespace.find("__", pos)) != std::string::npos) {
      message_namespace.replace(pos, 2, "::");
      pos += 2;
    }
    name.reserve(
      message_namespace.size() + 2 + 5 + message_name.size() + 1);  // "::" + "dds_::" + "_"
    name += message_namespace;
    name += "::";
  } else {
    name.reserve(5 + message_name.size() + 1);  // "dds_::" + "_"
  }
  name += "dds_::";
  name += message_name;
  name += '_';

  this->setName(name);
}

}  // namespace rmw_cyclonedds_cpp

#endif  // MESSAGETYPESUPPORT_IMPL_HPP_
