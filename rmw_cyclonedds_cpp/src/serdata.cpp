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
#include "serdata.hpp"

#include <cstring>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

#include "rmw/allocators.h"
#include "Serialization.hpp"
#include "TypeSupport2.hpp"
#include "bytewise.hpp"
#include "dds/ddsi/q_radmin.h"
#include "rmw/error_handling.h"
#include "MessageTypeSupport.hpp"
#include "ServiceTypeSupport.hpp"
#include "serdes.hpp"

using TypeSupport_c =
  rmw_cyclonedds_cpp::TypeSupport<rosidl_typesupport_introspection_c__MessageMembers>;
using TypeSupport_cpp =
  rmw_cyclonedds_cpp::TypeSupport<rosidl_typesupport_introspection_cpp::MessageMembers>;
using MessageTypeSupport_c =
  rmw_cyclonedds_cpp::MessageTypeSupport<rosidl_typesupport_introspection_c__MessageMembers>;
using MessageTypeSupport_cpp =
  rmw_cyclonedds_cpp::MessageTypeSupport<rosidl_typesupport_introspection_cpp::MessageMembers>;
using RequestTypeSupport_c = rmw_cyclonedds_cpp::RequestTypeSupport<
  rosidl_typesupport_introspection_c__ServiceMembers,
  rosidl_typesupport_introspection_c__MessageMembers>;
using RequestTypeSupport_cpp = rmw_cyclonedds_cpp::RequestTypeSupport<
  rosidl_typesupport_introspection_cpp::ServiceMembers,
  rosidl_typesupport_introspection_cpp::MessageMembers>;
using ResponseTypeSupport_c = rmw_cyclonedds_cpp::ResponseTypeSupport<
  rosidl_typesupport_introspection_c__ServiceMembers,
  rosidl_typesupport_introspection_c__MessageMembers>;
using ResponseTypeSupport_cpp = rmw_cyclonedds_cpp::ResponseTypeSupport<
  rosidl_typesupport_introspection_cpp::ServiceMembers,
  rosidl_typesupport_introspection_cpp::MessageMembers>;

static bool using_introspection_c_typesupport(const char * typesupport_identifier)
{
  return typesupport_identifier == rosidl_typesupport_introspection_c__identifier;
}

static bool using_introspection_cpp_typesupport(const char * typesupport_identifier)
{
  return typesupport_identifier == rosidl_typesupport_introspection_cpp::typesupport_identifier;
}

void * create_message_type_support(
  const void * untyped_members,
  const char * typesupport_identifier)
{
  if (using_introspection_c_typesupport(typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(untyped_members);
    return new MessageTypeSupport_c(members);
  } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(untyped_members);
    return new MessageTypeSupport_cpp(members);
  }
  RMW_SET_ERROR_MSG("Unknown typesupport identifier");
  return nullptr;
}

void * create_request_type_support(
  const void * untyped_members,
  const char * typesupport_identifier)
{
  if (using_introspection_c_typesupport(typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(untyped_members);
    return new RequestTypeSupport_c(members);
  } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *>(untyped_members);
    return new RequestTypeSupport_cpp(members);
  }
  RMW_SET_ERROR_MSG("Unknown typesupport identifier");
  return nullptr;
}

void * create_response_type_support(
  const void * untyped_members,
  const char * typesupport_identifier)
{
  if (using_introspection_c_typesupport(typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(untyped_members);
    return new ResponseTypeSupport_c(members);
  } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *>(untyped_members);
    return new ResponseTypeSupport_cpp(members);
  }
  RMW_SET_ERROR_MSG("Unknown typesupport identifier");
  return nullptr;
}

static void serialize_into_serdata_rmw(serdata_rmw * d, const void * sample)
{
  const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(d->type);
  try {
    if (d->kind != SDK_DATA) {
      /* ROS 2 doesn't do keys, so SDK_KEY is trivial */
    } else if (!type->is_request_header) {
      size_t sz = type->cdr_writer->get_serialized_size(sample);
      d->resize(sz);
      type->cdr_writer->serialize(d->data(), sample);
    } else {
      /* inject the service invocation header data into the CDR stream --
       * I haven't checked how it is done in the official RMW implementations, so it is
       * probably incompatible. */
      auto wrap = *static_cast<const cdds_request_wrapper_t *>(sample);
      size_t sz = type->cdr_writer->get_serialized_size(wrap);
      d->resize(sz);
      type->cdr_writer->serialize(d->data(), wrap);
    }
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
  }
}

static void serialize_into_serdata_rmw_on_demand(serdata_rmw * d)
{
#ifdef DDS_HAS_SHM
  auto type = const_cast<sertype_rmw *>(static_cast<const sertype_rmw *>(d->type));
  {
    std::lock_guard<std::mutex> lock(type->serialize_lock);
    if (d->iox_chunk && d->data() == nullptr) {
      auto iox_header = iceoryx_header_from_chunk(d->iox_chunk);
      // if the iox chunk has the data in serialized form
      if (iox_header->shm_data_state == IOX_CHUNK_CONTAINS_SERIALIZED_DATA) {
        d->resize(iox_header->data_size);
        memcpy(d->data(), d->iox_chunk, iox_header->data_size);
      } else if (iox_header->shm_data_state == IOX_CHUNK_CONTAINS_RAW_DATA) {
        serialize_into_serdata_rmw(const_cast<serdata_rmw *>(d), d->iox_chunk);
      } else {
        RMW_SET_ERROR_MSG("Received iox chunk is uninitialized");
      }
    }
  }
#endif
  (void)d;
}

static uint32_t serdata_rmw_size(const struct ddsi_serdata * dcmn)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  serialize_into_serdata_rmw_on_demand(const_cast<serdata_rmw *>(d));
  size_t size = d->size();
  uint32_t size_u32 = static_cast<uint32_t>(size);
  assert(size == size_u32);
  return size_u32;
}

static void serdata_rmw_free(struct ddsi_serdata * dcmn)
{
  auto * d = static_cast<serdata_rmw *>(dcmn);

#ifdef DDS_HAS_SHM
  if (d->iox_chunk && d->iox_subscriber) {
    free_iox_chunk(static_cast<iox_sub_t *>(d->iox_subscriber), &d->iox_chunk);
    d->iox_chunk = nullptr;
  }
#endif
  delete d;
}

static struct ddsi_serdata * serdata_rmw_from_ser(
  const struct ddsi_sertype * type,
  enum ddsi_serdata_kind kind,
  const struct nn_rdata * fragchain, size_t size)
{
  try {
    auto d = std::make_unique<serdata_rmw>(type, kind);
    uint32_t off = 0;
    assert(fragchain->min == 0);
    assert(fragchain->maxp1 >= off);    /* CDR header must be in first fragment */
    d->resize(size);

    auto cursor = d->data();
    while (fragchain) {
      if (fragchain->maxp1 > off) {
        /* only copy if this fragment adds data */
        const unsigned char * payload =
          NN_RMSG_PAYLOADOFF(fragchain->rmsg, NN_RDATA_PAYLOAD_OFF(fragchain));
        auto src = payload + off - fragchain->min;
        auto n_bytes = fragchain->maxp1 - off;
        memcpy(cursor, src, n_bytes);
        cursor = byte_offset(cursor, n_bytes);
        off = fragchain->maxp1;
        assert(off <= size);
      }
      fragchain = fragchain->nextfrag;
    }
    return d.release();
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return nullptr;
  }
}

static struct ddsi_serdata * serdata_rmw_from_ser_iov(
  const struct ddsi_sertype * type,
  enum ddsi_serdata_kind kind,
  ddsrt_msg_iovlen_t niov, const ddsrt_iovec_t * iov,
  size_t size)
{
  try {
    auto d = std::make_unique<serdata_rmw>(type, kind);
    d->resize(size);

    auto cursor = d->data();
    for (ddsrt_msg_iovlen_t i = 0; i < niov; i++) {
      memcpy(cursor, iov[i].iov_base, iov[i].iov_len);
      cursor = byte_offset(cursor, iov[i].iov_len);
    }
    return d.release();
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return nullptr;
  }
}

static struct ddsi_serdata * serdata_rmw_from_keyhash(
  const struct ddsi_sertype * type,
  const struct ddsi_keyhash * keyhash)
{
  static_cast<void>(keyhash);    // unused
  /* there is no key field, so from_keyhash is trivial */
  return new serdata_rmw(type, SDK_KEY);
}

static struct ddsi_serdata * serdata_rmw_from_sample(
  const struct ddsi_sertype * typecmn,
  enum ddsi_serdata_kind kind,
  const void * sample)
{
  try {
    const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(typecmn);
    auto d = std::make_unique<serdata_rmw>(type, kind);
    serialize_into_serdata_rmw(d.get(), sample);
    return d.release();
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return nullptr;
  }
}

#ifdef DDS_HAS_SHM
static struct ddsi_serdata * serdata_rmw_from_iox(
  const struct ddsi_sertype * typecmn,
  enum  ddsi_serdata_kind kind, void * sub, void * iox_buffer)
{
  try {
    const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(typecmn);
    auto d = std::make_unique<serdata_rmw>(type, kind);
    d->iox_chunk = iox_buffer;
    d->iox_subscriber = sub;
    return d.release();
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return nullptr;
  }
}
#endif  // DDS_HAS_SHM

struct ddsi_serdata * serdata_rmw_from_serialized_message(
  const struct ddsi_sertype * typecmn,
  const void * raw, size_t size)
{
  ddsrt_iovec_t iov;
  iov.iov_len = static_cast<ddsrt_iov_len_t>(size);
  if (iov.iov_len != size) {
    return nullptr;
  }
  iov.iov_base = const_cast<void *>(raw);
  return ddsi_serdata_from_ser_iov(typecmn, SDK_DATA, 1, &iov, size);
}

static struct ddsi_serdata * serdata_rmw_to_untyped(const struct ddsi_serdata * dcmn)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
#if DDS_HAS_DDSI_SERTYPE
  auto d1 = new serdata_rmw(d->type, SDK_KEY);
  d1->type = nullptr;
#else
  auto d1 = new serdata_rmw(d->topic, SDK_KEY);
  d1->topic = nullptr;
#endif
  return d1;
}

static void serdata_rmw_to_ser(const struct ddsi_serdata * dcmn, size_t off, size_t sz, void * buf)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  serialize_into_serdata_rmw_on_demand(const_cast<serdata_rmw *>(d));
  memcpy(buf, byte_offset(d->data(), off), sz);
}

static struct ddsi_serdata * serdata_rmw_to_ser_ref(
  const struct ddsi_serdata * dcmn, size_t off,
  size_t sz, ddsrt_iovec_t * ref)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  serialize_into_serdata_rmw_on_demand(const_cast<serdata_rmw *>(d));
  ref->iov_base = byte_offset(d->data(), off);
  ref->iov_len = (ddsrt_iov_len_t) sz;
  return ddsi_serdata_ref(d);
}

static void serdata_rmw_to_ser_unref(struct ddsi_serdata * dcmn, const ddsrt_iovec_t * ref)
{
  static_cast<void>(ref);    // unused
  ddsi_serdata_unref(static_cast<serdata_rmw *>(dcmn));
}

static bool serdata_rmw_to_sample(
  const struct ddsi_serdata * dcmn, void * sample, void ** bufptr,
  void * buflim)
{
  try {
    static_cast<void>(bufptr);    // unused
    static_cast<void>(buflim);    // unused
    auto d = static_cast<const serdata_rmw *>(dcmn);
#if DDS_HAS_DDSI_SERTYPE
    const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(d->type);
#else
    const struct sertopic_rmw * type = static_cast<const struct sertopic_rmw *>(d->topic);
#endif
    assert(bufptr == NULL);
    assert(buflim == NULL);
    if (d->kind != SDK_DATA) {
      /* ROS 2 doesn't do keys in a meaningful way yet */
    } else if (!type->is_request_header) {
      serialize_into_serdata_rmw_on_demand(const_cast<serdata_rmw *>(d));
      cycdeser sd(d->data(), d->size());
      if (using_introspection_c_typesupport(type->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_c *>(type->type_support.type_support_);
        return typed_typesupport->deserializeROSmessage(sd, sample);
      } else if (    // NOLINT
        using_introspection_cpp_typesupport(type->type_support.typesupport_identifier_))
      {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_cpp *>(type->type_support.type_support_);
        return typed_typesupport->deserializeROSmessage(sd, sample);
      }
    } else {
      /* The "prefix" lambda is there to inject the service invocation header data into the CDR
        stream -- I haven't checked how it is done in the official RMW implementations, so it is
        probably incompatible. */
      cdds_request_wrapper_t * const wrap = static_cast<cdds_request_wrapper_t *>(sample);
      auto prefix = [wrap](cycdeser & ser) {ser >> wrap->header.guid; ser >> wrap->header.seq;};
      serialize_into_serdata_rmw_on_demand(const_cast<serdata_rmw *>(d));
      cycdeser sd(d->data(), d->size());
      if (using_introspection_c_typesupport(type->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_c *>(type->type_support.type_support_);
        return typed_typesupport->deserializeROSmessage(sd, wrap->data, prefix);
      } else if (using_introspection_cpp_typesupport(type->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_cpp *>(type->type_support.type_support_);
        return typed_typesupport->deserializeROSmessage(sd, wrap->data, prefix);
      }
    }
  } catch (rmw_cyclonedds_cpp::Exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return false;
  } catch (std::runtime_error & e) {
    RMW_SET_ERROR_MSG(e.what());
    return false;
  }

  return false;
}

static bool serdata_rmw_untyped_to_sample(
  const struct ddsi_sertype * type,
  const struct ddsi_serdata * dcmn, void * sample,
  void ** bufptr, void * buflim)
{
  static_cast<void>(type);
  static_cast<void>(dcmn);
  static_cast<void>(sample);
  static_cast<void>(bufptr);
  static_cast<void>(buflim);
  /* ROS 2 doesn't do keys in a meaningful way yet */
  return true;
}

static bool serdata_rmw_eqkey(const struct ddsi_serdata * a, const struct ddsi_serdata * b)
{
  static_cast<void>(a);
  static_cast<void>(b);
  /* ROS 2 doesn't do keys in a meaningful way yet */
  return true;
}

static size_t serdata_rmw_print(
  const struct ddsi_sertype * tpcmn, const struct ddsi_serdata * dcmn, char * buf, size_t bufsize)
{
  try {
    auto d = static_cast<const serdata_rmw *>(dcmn);
    const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(tpcmn);
    if (d->kind != SDK_DATA) {
      /* ROS 2 doesn't do keys in a meaningful way yet */
      return static_cast<size_t>(snprintf(buf, bufsize, ":k:{}"));
    } else if (!type->is_request_header) {
      serialize_into_serdata_rmw_on_demand(const_cast<serdata_rmw *>(d));
      cycprint sd(buf, bufsize, d->data(), d->size());
      if (using_introspection_c_typesupport(type->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_c *>(type->type_support.type_support_);
        return typed_typesupport->printROSmessage(sd);
      } else if (using_introspection_cpp_typesupport(type->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_cpp *>(type->type_support.type_support_);
        return typed_typesupport->printROSmessage(sd);
      }
    } else {
      /* The "prefix" lambda is there to inject the service invocation header data into the CDR
        stream -- I haven't checked how it is done in the official RMW implementations, so it is
        probably incompatible. */
      cdds_request_wrapper_t wrap;
      auto prefix = [&wrap](cycprint & ser) {
          ser >> wrap.header.guid; ser.print_constant(","); ser >> wrap.header.seq;
        };
      cycprint sd(buf, bufsize, d->data(), d->size());
      if (using_introspection_c_typesupport(type->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_c *>(type->type_support.type_support_);
        return typed_typesupport->printROSmessage(sd, prefix);
      } else if (using_introspection_cpp_typesupport(type->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_cpp *>(type->type_support.type_support_);
        return typed_typesupport->printROSmessage(sd, prefix);
      }
    }
  } catch (rmw_cyclonedds_cpp::Exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return false;
  } catch (std::runtime_error & e) {
    RMW_SET_ERROR_MSG(e.what());
    return false;
  }

  return false;
}

static void serdata_rmw_get_keyhash(
  const struct ddsi_serdata * d, struct ddsi_keyhash * buf,
  bool force_md5)
{
  /* ROS 2 doesn't do keys in a meaningful way yet, this is never called for types without
     key fields */
  static_cast<void>(d);
  static_cast<void>(force_md5);
  memset(buf, 0, sizeof(*buf));
}

static const struct ddsi_serdata_ops serdata_rmw_ops = {
  serdata_rmw_eqkey,
  serdata_rmw_size,
  serdata_rmw_from_ser,
  serdata_rmw_from_ser_iov,
  serdata_rmw_from_keyhash,
  serdata_rmw_from_sample,
  serdata_rmw_to_ser,
  serdata_rmw_to_ser_ref,
  serdata_rmw_to_ser_unref,
  serdata_rmw_to_sample,
  serdata_rmw_to_untyped,
  serdata_rmw_untyped_to_sample,
  serdata_rmw_free,
  serdata_rmw_print,
  serdata_rmw_get_keyhash
#ifdef DDS_HAS_SHM
  , ddsi_serdata_iox_size,
  serdata_rmw_from_iox
#endif  // DDS_HAS_SHM
};

static void sertype_rmw_free(struct ddsi_sertype * tpcmn)
{
  struct sertype_rmw * tp = static_cast<struct sertype_rmw *>(tpcmn);
#if DDS_HAS_DDSI_SERTYPE
  ddsi_sertype_fini(tpcmn);
#else
  ddsi_sertopic_fini(tpcmn);
#endif
  if (tp->type_support.type_support_) {
    if (using_introspection_c_typesupport(tp->type_support.typesupport_identifier_)) {
      delete static_cast<TypeSupport_c *>(tp->type_support.type_support_);
    } else if (using_introspection_cpp_typesupport(tp->type_support.typesupport_identifier_)) {
      delete static_cast<TypeSupport_cpp *>(tp->type_support.type_support_);
    }
    tp->type_support.type_support_ = NULL;
  }

  delete tp;
}

static void sertype_rmw_zero_samples(const struct ddsi_sertype * d, void * samples, size_t count)
{
  static_cast<void>(d);
  static_cast<void>(samples);
  static_cast<void>(count);
  /* Not using code paths that rely on the samples getting zero'd out */
}

static void sertype_rmw_realloc_samples(
  void ** ptrs, const struct ddsi_sertype * d, void * old,
  size_t oldcount, size_t count)
{
  static_cast<void>(ptrs);
  static_cast<void>(d);
  static_cast<void>(old);
  static_cast<void>(oldcount);
  static_cast<void>(count);
  /* Not using code paths that rely on this (loans, dispose, unregister with instance handle,
     content filters) */
  abort();
}

static void sertype_rmw_free_samples(
  const struct ddsi_sertype * d, void ** ptrs, size_t count,
  dds_free_op_t op)
{
  static_cast<void>(d);    // unused
  static_cast<void>(ptrs);    // unused
  static_cast<void>(count);    // unused
  /* Not using code paths that rely on this (dispose, unregister with instance handle, content
     filters) */
  assert(!(op & DDS_FREE_ALL_BIT));
  (void) op;
}

bool sertype_rmw_equal(
  const struct ddsi_sertype * acmn, const struct ddsi_sertype * bcmn)
{
  /* A bit of a guess: types with the same name & type name are really the same if they have
     the same type support identifier as well */
  const struct sertype_rmw * a = static_cast<const struct sertype_rmw *>(acmn);
  const struct sertype_rmw * b = static_cast<const struct sertype_rmw *>(bcmn);
  if (a->is_request_header != b->is_request_header) {
    return false;
  }
  if (strcmp(
      a->type_support.typesupport_identifier_,
      b->type_support.typesupport_identifier_) != 0)
  {
    return false;
  }
  return true;
}

uint32_t sertype_rmw_hash(const struct ddsi_sertype * tpcmn)
{
  const struct sertype_rmw * tp = static_cast<const struct sertype_rmw *>(tpcmn);
  uint32_t h2 = static_cast<uint32_t>(std::hash<bool>{}(tp->is_request_header));
  uint32_t h1 =
    static_cast<uint32_t>(std::hash<std::string>{}(
      std::string(
        tp->type_support.typesupport_identifier_)));
  return h1 ^ h2;
}

size_t sertype_get_serialized_size(const struct ddsi_sertype * d, const void * sample)
{
  const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(d);
  size_t serialized_size = 0;
  try {
    // ROS 2 doesn't support keys yet, so only data is handled
    if (!type->is_request_header) {
      serialized_size = type->cdr_writer->get_serialized_size(sample);
    } else {
      // inject the service invocation header data into the CDR stream
      auto wrap = *static_cast<const cdds_request_wrapper_t *>(sample);
      serialized_size = type->cdr_writer->get_serialized_size(wrap);
    }
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
  }

  return serialized_size;
}

bool sertype_serialize_into(
  const struct ddsi_sertype * d,
  const void * sample,
  void * dst_buffer,
  size_t dst_size)
{
  const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(d);
  try {
    // ignore destination size (assuming that the destination buffer is resized before correctly)
    static_cast<void>(dst_size);
    // ROS 2 doesn't support keys, so its all data (?)
    if (!type->is_request_header) {
      type->cdr_writer->serialize(dst_buffer, sample);
    } else {
      /* inject the service invocation header data into the CDR stream --
       * I haven't checked how it is done in the official RMW implementations, so it is
       * probably incompatible. */
      auto wrap = *static_cast<const cdds_request_wrapper_t *>(sample);
      type->cdr_writer->serialize(dst_buffer, wrap);
    }
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
  }
  return true;
}

static const struct ddsi_sertype_ops sertype_rmw_ops = {
#if DDS_HAS_DDSI_SERTYPE
  ddsi_sertype_v0,
  nullptr,
#endif
  sertype_rmw_free,
  sertype_rmw_zero_samples,
  sertype_rmw_realloc_samples,
  sertype_rmw_free_samples,
  sertype_rmw_equal,
  sertype_rmw_hash
#if DDS_HAS_DDSI_SERTYPE
  /* not yet providing type discovery, assignability checking */
  , nullptr,
  nullptr,
  nullptr,
  nullptr,
  sertype_get_serialized_size,
  sertype_serialize_into
#endif
};

template<typename MembersType>
ROSIDL_TYPESUPPORT_INTROSPECTION_CPP_LOCAL
inline std::string create_type_name(const void * untyped_members)
{
  auto members = static_cast<const MembersType *>(untyped_members);
  if (!members) {
    RMW_SET_ERROR_MSG("members handle is null");
    return "";
  }

  std::ostringstream ss;
  std::string message_namespace(members->message_namespace_);
  // Find and replace C namespace separator with C++, in case this is using C typesupport
  message_namespace = std::regex_replace(message_namespace, std::regex("__"), "::");
  std::string message_name(members->message_name_);
  if (!message_namespace.empty()) {
    ss << message_namespace << "::";
  }
  ss << "dds_::" << message_name << "_";
  return ss.str();
}

static std::string get_type_name(const char * type_support_identifier, void * type_support)
{
  if (using_introspection_c_typesupport(type_support_identifier)) {
    auto typed_typesupport = static_cast<MessageTypeSupport_c *>(type_support);
    return typed_typesupport->getName();
  } else if (using_introspection_cpp_typesupport(type_support_identifier)) {
    auto typed_typesupport = static_cast<MessageTypeSupport_cpp *>(type_support);
    return typed_typesupport->getName();
  } else {
    return "absent";
  }
}

struct sertype_rmw * create_sertype(
  const char * type_support_identifier,
  void * type_support, bool is_request_header,
  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> message_type,
  const uint32_t sample_size, const bool is_fixed_type)
{
  struct sertype_rmw * st = new struct sertype_rmw;
  std::string type_name = get_type_name(type_support_identifier, type_support);
  uint32_t flags = DDSI_SERTYPE_FLAG_TOPICKIND_NO_KEY;
  if (is_fixed_type) {
    flags |= DDSI_SERTYPE_FLAG_FIXED_SIZE;
  }
  ddsi_sertype_init_flags(
    static_cast<struct ddsi_sertype *>(st),
    type_name.c_str(), &sertype_rmw_ops, &serdata_rmw_ops, flags);
  st->allowed_data_representation = DDS_DATA_REPRESENTATION_FLAG_XCDR1;
#ifdef DDS_HAS_SHM
  // TODO(Sumanth) needs some API in cyclone to set this
  st->iox_size = sample_size;
#else
  static_cast<void>(sample_size);
#endif  // DDS_HAS_SHM
  st->type_support.typesupport_identifier_ = type_support_identifier;
  st->type_support.type_support_ = type_support;
  st->is_request_header = is_request_header;
  st->cdr_writer = rmw_cyclonedds_cpp::make_cdr_writer(std::move(message_type));

  return st;
}

void serdata_rmw::resize(size_t requested_size)
{
  if (!requested_size) {
    m_size = 0;
    m_data.reset();
    return;
  }

  /* FIXME: CDR padding in DDSI makes me do this to avoid reading beyond the bounds
  when copying data to network.  Should fix Cyclone to handle that more elegantly.  */
  size_t n_pad_bytes = (0 - requested_size) % 4;
  m_data.reset(new byte[requested_size + n_pad_bytes]);
  m_size = requested_size + n_pad_bytes;

  // zero the very end. The caller isn't necessarily going to overwrite it.
  std::memset(byte_offset(m_data.get(), requested_size), '\0', n_pad_bytes);
}

serdata_rmw::serdata_rmw(const ddsi_sertype * type, ddsi_serdata_kind kind)
: ddsi_serdata{}
{
  ddsi_serdata_init(this, type, kind);
}
