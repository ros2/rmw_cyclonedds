// Copyright 2019 ADLINK Technology
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
#ifndef SERDATA_HPP_
#define SERDATA_HPP_

#include <memory>
#include <string>

#include "TypeSupport2.hpp"
#include "bytewise.hpp"
#include "dds/ddsi/ddsi_serdata.h"
#include "dds/ddsi/ddsi_sertopic.h"

namespace rmw_cyclonedds_cpp
{
class BaseCDRWriter;
}

struct CddsTypeSupport
{
  void * type_support_;
  const char * typesupport_identifier_;
};

struct sertopic_rmw : ddsi_sertopic
{
  CddsTypeSupport type_support;
  bool is_request_header;
#if !DDSI_SERTOPIC_HAS_TOPICKIND_NO_KEY
  std::string cpp_name;
  std::string cpp_type_name;
  std::string cpp_name_type_name;
#endif
  std::unique_ptr<const rmw_cyclonedds_cpp::BaseCDRWriter> cdr_writer;
};

class serdata_rmw : public ddsi_serdata
{
protected:
  size_t m_size {0};
  /* first two bytes of data is CDR encoding
     second two bytes are encoding options */
  std::unique_ptr<byte[]> m_data {nullptr};

public:
  serdata_rmw(const ddsi_sertopic * topic, ddsi_serdata_kind kind);
  void resize(size_t requested_size);
  size_t size() const {return m_size;}
  void * data() const {return m_data.get();}
};

typedef struct cdds_request_header
{
  uint64_t guid;
  int64_t seq;
} cdds_request_header_t;

typedef struct cdds_request_wrapper
{
  cdds_request_header_t header;
  void * data;
} cdds_request_wrapper_t;

void * create_message_type_support(
  const void * untyped_members,
  const char * typesupport_identifier);
void * create_request_type_support(
  const void * untyped_members,
  const char * typesupport_identifier);
void * create_response_type_support(
  const void * untyped_members,
  const char * typesupport_identifier);

struct sertopic_rmw * create_sertopic(
  const char * topicname, const char * type_support_identifier,
  void * type_support, bool is_request_header,
  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> message_type_support);

struct ddsi_serdata * serdata_rmw_from_serialized_message(
  const struct ddsi_sertopic * topiccmn,
  const void * raw, size_t size);

#endif  // SERDATA_HPP_
