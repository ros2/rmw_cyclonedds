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
#ifndef SERDATA_HPP
#define SERDATA_HPP

#include "dds/ddsi/ddsi_sertopic.h"
#include "dds/ddsi/ddsi_serdata.h"

struct CddsTypeSupport {
    void *type_support_;
    const char *typesupport_identifier_;
};

struct sertopic_rmw : ddsi_sertopic {
    CddsTypeSupport type_support;
    bool is_request_header;
    std::string cpp_name;
    std::string cpp_type_name;
    std::string cpp_name_type_name;
};

struct serdata_rmw : ddsi_serdata {
    /* first two bytes of data is CDR encoding
       second two bytes are encoding options */
    std::vector<unsigned char> data;
};

typedef struct cdds_request_header {
    uint64_t guid;
    int64_t seq;
} cdds_request_header_t;

typedef struct cdds_request_wrapper {
    cdds_request_header_t header;
    void *data;
} cdds_request_wrapper_t;

void *create_message_type_support (const void *untyped_members, const char *typesupport_identifier);
void *create_request_type_support (const void *untyped_members, const char *typesupport_identifier);
void *create_response_type_support (const void *untyped_members, const char *typesupport_identifier);

struct sertopic_rmw *create_sertopic (const char *topicname, const char *type_support_identifier, void *type_support, bool is_request_header);

#endif
