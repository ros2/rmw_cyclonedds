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

#include <string.h>
#include "dds/dds.h"
#include "cdds_version.hpp"
#include "dyntype_helper.hpp"

#if DDS_HAS_TYPELIB
dds_dynamic_type_descriptor_t get_dynamic_type_descriptor_prim(
  dds_dynamic_type_kind_t kind, const char * name, uint32_t num_bounds, const uint32_t * bounds,
  dds_dynamic_type_kind_t type)
{
  dds_dynamic_type_descriptor_t desc;
  memset(&desc, 0, sizeof(desc));
  desc.kind = kind;
  desc.name = name;
  desc.num_bounds = num_bounds;
  desc.bounds = bounds;
  desc.element_type = DDS_DYNAMIC_TYPE_SPEC_PRIM(type);
  return desc;
}

dds_dynamic_type_descriptor_t get_dynamic_type_descriptor(
  dds_dynamic_type_kind_t kind, const char * name, uint32_t num_bounds, const uint32_t * bounds,
  dds_dynamic_type_t type)
{
  dds_dynamic_type_descriptor_t desc;
  memset(&desc, 0, sizeof(desc));
  desc.kind = kind;
  desc.name = name;
  desc.num_bounds = num_bounds;
  desc.bounds = bounds;
  desc.element_type = DDS_DYNAMIC_TYPE_SPEC(type);
  return desc;
}

dds_dynamic_member_descriptor_t get_dynamic_member_descriptor_prim(
  dds_dynamic_type_kind_t type, const char * name)
{
  return DDS_DYNAMIC_MEMBER_PRIM(type, name);
}

dds_dynamic_member_descriptor_t get_dynamic_member_descriptor(
  dds_dynamic_type_t ddt, const char * name)
{
  return DDS_DYNAMIC_MEMBER(ddt, name);
}
#endif
