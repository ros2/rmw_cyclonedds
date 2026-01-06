// Copyright 2019 ADLINK Technology
// Copyright 2025 ZettaScale Technology
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

#ifndef DYNTYPE_HPP_
#define DYNTYPE_HPP_

#include "dds/dds.h"
#include "serdata.hpp"

void create_msg_dds_dynamic_type(
  const char * type_support_identifier, const void * untyped_members,
  dds_entity_t dds_ppant, struct sertype_rmw * st);

void create_req_dds_dynamic_type(
  const char * type_support_identifier, const void * untyped_members,
  dds_entity_t dds_ppant, struct sertype_rmw * st);

void create_res_dds_dynamic_type(
  const char * type_support_identifier, const void * untyped_members,
  dds_entity_t dds_ppant, struct sertype_rmw * st);

#endif  // DYNTYPE_HPP_
