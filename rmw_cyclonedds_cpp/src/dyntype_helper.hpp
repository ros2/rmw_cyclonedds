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

#ifndef DYNTYPE_HELPER_HPP_
#define DYNTYPE_HELPER_HPP_

#include "dds/dds.h"
#include "cdds_version.hpp"

#if defined (__cplusplus)
extern "C" {
#endif

#if DDS_HAS_TYPELIB
dds_dynamic_type_descriptor_t get_dynamic_type_descriptor_prim(
  dds_dynamic_type_kind_t kind, const char * name, uint32_t num_bounds, const uint32_t * bounds,
  dds_dynamic_type_kind_t type);

dds_dynamic_type_descriptor_t get_dynamic_type_descriptor(
  dds_dynamic_type_kind_t kind, const char * name, uint32_t num_bounds, const uint32_t * bounds,
  dds_dynamic_type_t type);

dds_dynamic_member_descriptor_t get_dynamic_member_descriptor_prim(
  dds_dynamic_type_kind_t type, const char * name);

dds_dynamic_member_descriptor_t get_dynamic_member_descriptor(
  dds_dynamic_type_t ddt, const char * name);
#endif

#if defined (__cplusplus)
}
#endif

#endif  // DYNTYPE_HELPER_HPP_
