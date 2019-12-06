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
#ifndef ROS2_MASTER_DESERIALIZATION_HPP
#define ROS2_MASTER_DESERIALIZATION_HPP

#include "TypeSupport2.hpp"
namespace rmw_cyclonedds_cpp
{

void deserialize_top_level(
  void * destination_object, const void * data, const StructValueType * ts);

}
#endif  //ROS2_MASTER_DESERIALIZATION_HPP
