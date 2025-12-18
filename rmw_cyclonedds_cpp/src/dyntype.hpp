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

#endif // DYNTYPE_HPP_
