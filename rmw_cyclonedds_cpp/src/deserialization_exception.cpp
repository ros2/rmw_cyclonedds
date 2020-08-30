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

#include "deserialization_exception.hpp"

using rmw_cyclonedds_cpp::DeserializationException;

DeserializationException::DeserializationException(const char * const & message)
: Exception(message)
{
}

DeserializationException::DeserializationException(const DeserializationException & ex)
: Exception(ex)
{
}

DeserializationException & DeserializationException::operator=(const DeserializationException & ex)
{
  if (this != &ex) {
    Exception::operator=(ex);
  }
  return *this;
}

DeserializationException::~DeserializationException() throw()
{
}

const char * const DeserializationException::DEFAULT_MESSAGE = "Invalid data";
