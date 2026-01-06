#ifndef DYNTYPE_HPP_
#define DYNTYPE_HPP_

#include "dds/dds.h"
#include "serdata.hpp"

template<typename MembersType>
ROSIDL_TYPESUPPORT_INTROSPECTION_CPP_LOCAL
std::string create_type_name(const void * untyped_members);

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
