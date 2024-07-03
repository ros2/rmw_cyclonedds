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
#include <mutex>

#include "TypeSupport2.hpp"
#include "bytewise.hpp"
#include "dds/dds.h"
#include "dds/ddsi/ddsi_serdata.h"
#include "cdds_version.hpp"

#if CDDS_VERSION == CDDS_VERSION_0_10 && defined DDS_HAS_SHM
extern "C" {
#include "dds/ddsi/ddsi_shm_transport.h"
}
#endif

namespace rmw_cyclonedds_cpp
{
class BaseCDRWriter;
}

struct CddsTypeSupport
{
  void * type_support_;
  const char * typesupport_identifier_;
};

struct sertype_rmw : ddsi_sertype
{
  CddsTypeSupport type_support;
  bool is_request_header;
  std::unique_ptr<const rmw_cyclonedds_cpp::BaseCDRWriter> cdr_writer;
  bool is_fixed;
  std::mutex serialize_lock;
#if DDS_HAS_TYPELIB
  struct dds_type_meta_ser type_information;
  struct dds_type_meta_ser type_mapping;
#endif
};

class serdata_rmw : public ddsi_serdata
{
protected:
  size_t m_size {0};
  /* first two bytes of data is CDR encoding
     second two bytes are encoding options */
  std::unique_ptr<byte[]> m_data {nullptr};

public:
  serdata_rmw(const ddsi_sertype * type, ddsi_serdata_kind kind);
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

struct sertype_rmw * create_sertype(
  const char * type_support_identifier,
  void * type_support, bool is_request_header,
  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> message_type_support,
  const uint32_t sample_size = 0U,
  const bool is_fixed_type = false
);

struct ddsi_serdata * serdata_rmw_from_serialized_message(
  const struct ddsi_sertype * typecmn,
  const void * raw, size_t size);

void create_msg_dds_dynamic_type(
  const char * type_support_identifier,
  const void * untyped_members, dds_entity_t dds_ppant, struct sertype_rmw * st);

void create_req_dds_dynamic_type(
  const char * type_support_identifier,
  const void * untyped_members, dds_entity_t dds_ppant, struct sertype_rmw * st);

void create_res_dds_dynamic_type(
  const char * type_support_identifier,
  const void * untyped_members, dds_entity_t dds_ppant, struct sertype_rmw * st);

#endif  // SERDATA_HPP_
