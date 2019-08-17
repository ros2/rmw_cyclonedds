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
#include <vector>

#include "rmw_cyclonedds_cpp/serdes.hpp"

cycser::cycser(std::vector<unsigned char> & dst_)
: dst(dst_),
  off(0)
{
  dst.reserve(4);
  /* FIXME: hard code to little endian ... and ignoring endianness in deser */
  dst.push_back(0);
  dst.push_back(3);
  /* options: */
  dst.push_back(0);
  dst.push_back(0);
}

cycdeser::cycdeser(const void * data_, size_t size_)
: data(static_cast<const char *>(data_) + 4),
  pos(0),
  lim(size_ - 4)
{
}
