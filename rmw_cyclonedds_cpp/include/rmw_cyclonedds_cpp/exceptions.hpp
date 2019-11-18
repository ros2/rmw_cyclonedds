// Copyright 2018 to 2019 ADLINK Technology
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

#ifndef RMW_CYCLONEDDS_CPP__EXCEPTIONS_HPP_
#define RMW_CYCLONEDDS_CPP__EXCEPTIONS_HPP_

#include <exception>
#include <string>

namespace rmw_cyclonedds_cpp
{
class Exception : public std::runtime_error
{
public:
  explicit Exception(const std::string & what_arg)
  : std::runtime_error(what_arg) {}
  explicit Exception(const char * what_arg)
  : std::runtime_error(what_arg) {}
};

class DeserializationException : public Exception
{
public:
  explicit DeserializationException(const std::string & what_arg)
  : Exception(what_arg) {}
  explicit DeserializationException(const char * what_arg)
  : Exception(what_arg) {}
};

}  // namespace rmw_cyclonedds_cpp

#endif  // RMW_CYCLONEDDS_CPP__EXCEPTIONS_HPP_
