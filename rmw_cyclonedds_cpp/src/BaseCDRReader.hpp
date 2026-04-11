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
#ifndef BASE_CDR_READER_HPP_
#define BASE_CDR_READER_HPP_

#include <cstddef>
#include <memory>
#include <vector>

#include "SizeTypeSupport.hpp"  // for MessageMembersVariant, SampleOrKey, SampleOrRequest

namespace rmw_cyclonedds_cpp
{

class BaseCDRReader
{
public:
  virtual void deserialize(
    void * dest, const void * cdr, size_t cdrsize,
    SampleOrKey what) const = 0;
  virtual void extractkey(
    std::vector<std::byte> & dest, const void * cdr, size_t cdrsize,
    SampleOrKey what) const = 0;
  virtual void extractkey_be(
    std::vector<std::byte> & dst, const void * cdr, size_t cdrsize,
    SampleOrKey what) const = 0;
  virtual size_t print(
    char * dst, size_t dstsize, const void * cdr, size_t cdrsize,
    SampleOrKey what) const = 0;

  virtual ~BaseCDRReader() = default;
};

std::unique_ptr<BaseCDRReader> make_cdr_reader(
  MessageMembersVariant members,
  SampleOrRequest variant);

}  // namespace rmw_cyclonedds_cpp
#endif  // BASE_CDR_READER_HPP_
