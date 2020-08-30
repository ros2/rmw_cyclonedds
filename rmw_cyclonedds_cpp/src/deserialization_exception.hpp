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

#ifndef DESERIALIZATION_EXCEPTION_HPP_
#define DESERIALIZATION_EXCEPTION_HPP_

#include "exception.hpp"

namespace rmw_cyclonedds_cpp
{

class DeserializationException : public Exception
{
public:
  explicit DeserializationException(const char * const & message);
  DeserializationException(const DeserializationException & ex);
  DeserializationException & operator=(const DeserializationException & ex);

  virtual ~DeserializationException() throw();

  static const char * const DEFAULT_MESSAGE;
};

}  // namespace rmw_cyclonedds_cpp

#endif  // DESERIALIZATION_EXCEPTION_HPP_
