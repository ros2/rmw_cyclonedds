// Copyright 2019 ADLINK Technology via Rover Robotics and Dan Rose
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

#ifndef RMW_CYCLONEDDS_CPP__FALLTHROUGH_MACRO_HPP_
#define RMW_CYCLONEDDS_CPP__FALLTHROUGH_MACRO_HPP_

#if __has_cpp_attribute(fallthrough) || (__cplusplus >= 201603L)
// C++17
#define FALLTHROUGH [[fallthrough]]
#elif __has_cpp_attribute(clang::fallthrough)
// Clang
#define FALLTHROUGH [[clang::fallthrough]]
#else
// gcc
#define FALLTHROUGH /* fallthrough */
#endif

#endif  //RMW_CYCLONEDDS_CPP__FALLTHROUGH_MACRO_HPP_
