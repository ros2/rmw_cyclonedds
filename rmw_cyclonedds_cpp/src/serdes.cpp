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
#include <exception>
#include <vector>

#include "dds/ddsrt/endian.h"
#include "rmw_cyclonedds_cpp/serdes.hpp"

cycser::cycser(std::vector<unsigned char> & dst_)
: dst(dst_),
  off(0)
{
  dst.reserve(4);

  /* endianness: 0x0000 for BE, 0x0001 for LE */
  dst.push_back(0x00);
  dst.push_back((DDSRT_ENDIAN == DDSRT_LITTLE_ENDIAN) ? 0x01 : 0x00);

  /* options: defaults to 0x0000 */
  dst.push_back(0);
  dst.push_back(0);
}

cycdeserbase::cycdeserbase(const char * data_, size_t size_)
: data(data_),
  pos(0),
  lim(size_),
  swap_bytes(false)
{
  /* Get the endianness byte (skip unused first byte in data[0]) */
  uint32_t data_endianness = (data[1] == 0x01) ? DDSRT_LITTLE_ENDIAN : DDSRT_BIG_ENDIAN;

  /* If endianness of data differs from our endianness: swap bytes when deserializing */
  swap_bytes = (DDSRT_ENDIAN != data_endianness);

  /* Ignore representation options (data_[2] and data_[3]) */
  data += 4;
  lim -= 4;
}

cycdeser::cycdeser(const void * data_, size_t size_)
: cycdeserbase(static_cast<const char *>(data_), size_)
{
}

cycprint::cycprint(char * buf_, size_t bufsize_, const void * data_, size_t size_)
: cycdeserbase(static_cast<const char *>(data_), size_),
  buf(buf_),
  bufsize(bufsize_)
{
}
