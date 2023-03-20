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

#ifndef MESSAGETYPESUPPORT_HPP_
#define MESSAGETYPESUPPORT_HPP_

#include <cassert>
#include <memory>

#include "TypeSupport.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"

namespace rmw_cyclonedds_cpp
{

template<typename MembersType>
class MessageTypeSupport : public TypeSupport<MembersType>
{
public:
  explicit MessageTypeSupport(const MembersType * members);
  const MembersType * get_members();
};

}  // namespace rmw_cyclonedds_cpp

#include "MessageTypeSupport_impl.hpp"

#endif  // MESSAGETYPESUPPORT_HPP_
