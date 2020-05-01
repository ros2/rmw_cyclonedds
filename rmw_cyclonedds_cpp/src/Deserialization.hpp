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

#include "CDR.hpp"
#include "TypeSupport2.hpp"
#include "serdata.hpp"
namespace rmw_cyclonedds_cpp
{

void deserialize_top_level(void * destination_object, const void * data, const StructValueType * ts)
{
  EncapsulationHeader enc_hdr;
  auto bdata = static_cast<const byte *>(data);
  uint16_t benc_hdr = (((uint16_t)data[0])<<8) | data[1];
  enc_hdr.m_endian = (enc_hdr & 1) ? little_endian : big_endian;
  switch(benc_hdr ) {
    case 0x0000:
      enc_hdr.m_endian
  }
  if (bdata[0] == 1)
  enc_hdr.m_endian =
  // todo: create deserialization stream using the encapsulation header (endian, eversion)
  // todo: pull data out of that stream, structurally recursing on data and types
}

class AbstractCDRReader
{
public:
  virtual size_t get_serialized_size(const void * data) const = 0;
  virtual void serialize(void * dest, const void * data) const = 0;
  virtual size_t get_serialized_size(const cdds_request_wrapper_t & request) const = 0;
  virtual void serialize(void * dest, const cdds_request_wrapper_t & request) const = 0;
  virtual ~AbstractCDRReader() = default;
};

std::unique_ptr<AbstractCDRReader> make_cdr_reader(std::unique_ptr<StructValueType> value_type);

}
#endif  //ROS2_MASTER_DESERIALIZATION_HPP
