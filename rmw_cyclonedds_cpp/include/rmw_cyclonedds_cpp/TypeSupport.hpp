// Copyright 2016 Proyectos y Sistemas de Mantenimiento SL (eProsima).
// Copyright 2018 ADLINK Technology
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

#ifndef RMW_CYCLONEDDS_CPP__TYPESUPPORT_HPP_
#define RMW_CYCLONEDDS_CPP__TYPESUPPORT_HPP_

#include <cassert>
#include <string>
#include <functional>

#include "rcutils/logging_macros.h"

#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "rosidl_typesupport_introspection_cpp/identifier.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/service_introspection.hpp"
#include "rosidl_typesupport_introspection_cpp/visibility_control.h"

#include "serdes.hpp"

namespace rmw_cyclonedds_cpp
{

// Helper class to read/write string types to/from a cycser/cycdeser
struct StringHelper
{
  using type = std::string;

  static std::string & convert_to_std_string(void * data)
  {
    return *(static_cast<std::string *>(data));
  }

  static void assign(cycdeser & deser, void * field, bool call_new)
  {
    std::string & str = *(std::string *)field;
    if (call_new) {
      new(&str) std::string;
    }
    deser >> str;
  }
};

template<typename MembersType>
class TypeSupport
{
public:
  bool serializeROSmessage(
    const void * ros_message, cycser & ser,
    std::function<void(cycser &)> prefix = nullptr);
  bool deserializeROSmessage(
    cycdeser & deser, void * ros_message,
    std::function<void(cycdeser &)> prefix = nullptr);
  bool printROSmessage(
    cycprint & deser,
    std::function<void(cycprint &)> prefix = nullptr);
  std::string getName();

protected:
  TypeSupport();

  void setName(const std::string & name);

  const MembersType * members_;
  std::string name;

private:
  bool serializeROSmessage(cycser & ser, const MembersType * members, const void * ros_message);
  bool deserializeROSmessage(
    cycdeser & deser, const MembersType * members, void * ros_message,
    bool call_new);
  bool printROSmessage(
    cycprint & deser, const MembersType * members);
};

}  // namespace rmw_cyclonedds_cpp

#include "TypeSupport_impl.hpp"

#endif  // RMW_CYCLONEDDS_CPP__TYPESUPPORT_HPP_
