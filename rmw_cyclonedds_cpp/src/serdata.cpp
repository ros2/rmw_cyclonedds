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

#include "dds/dds.h"
#include "cdds_version.hpp"

#include "rmw/allocators.h"
#include "Serialization.hpp"
#include "TypeSupport2.hpp"
#include "bytewise.hpp"
#if CDDS_VERSION == CDDS_VERSION_0_10
#include "dds/ddsi/q_radmin.h"
#define ddsi_rdata nn_rdata
#define DDSI_RMSG_PAYLOADOFF(rmsg,rdata) NN_RMSG_PAYLOADOFF((rmsg), (rdata))
#define DDSI_RDATA_PAYLOAD_OFF(rdata) NN_RDATA_PAYLOAD_OFF((rdata))
#else
#include "dds/ddsi/ddsi_radmin.h"
#include "dds/ddsc/dds_psmx.h"
#endif
#include "rmw/error_handling.h"
#include "MessageTypeSupport.hpp"
#include "ServiceTypeSupport.hpp"
#include "serdes.hpp"

#if DDS_HAS_TYPELIB
#include "dds/ddsrt/string.h"
#include "dds/ddsrt/heap.h"
#include "dds/ddsi/ddsi_typelib.h"
#endif

// When non-zero throw an exception when dynamic type construction fails.  Right now, it
// should handle everything but WStrings fine, but those are part of the test suite.
//
// Not having a dynamic type associated with the topic doesn't do damage, it just means
// the integration with the DDS type system is missing just like when you don't do this at
// all.
#define THROW_ON_DYNAMIC_TYPE_ERROR 0

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
  return strcmp(
    typesupport_identifier,
    rosidl_typesupport_introspection_c__identifier) == 0;
}

static bool using_introspection_cpp_typesupport(const char * typesupport_identifier)
{
  return strcmp(
    typesupport_identifier,
    rosidl_typesupport_introspection_cpp::typesupport_identifier) == 0;
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
#if CDDS_VERSION > CDDS_VERSION_0_10
  auto type = const_cast<sertype_rmw *>(static_cast<const sertype_rmw *>(d->type));
  {
    std::lock_guard<std::mutex> lock(type->serialize_lock);
    if (d->loan && d->data() == nullptr) {
      if (d->loan->metadata->sample_state == DDS_LOANED_SAMPLE_STATE_SERIALIZED_DATA) {
        d->resize(d->loan->metadata->sample_size);
        memcpy(d->data(), d->loan->sample_ptr, d->loan->metadata->sample_size);
      } else if (d->loan->metadata->sample_state == DDS_LOANED_SAMPLE_STATE_RAW_DATA) {
        serialize_into_serdata_rmw(const_cast<serdata_rmw *>(d), d->loan->sample_ptr);
      } else {
        RMW_SET_ERROR_MSG("Received iox chunk is uninitialized");
      }
    }
  }
#elif defined DDS_HAS_SHM
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

#if CDDS_VERSION > CDDS_VERSION_0_10
  if (d->loan) {
    dds_loaned_sample_unref (d->loan);
  }
#elif defined DDS_HAS_SHM
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
  const struct ddsi_rdata * fragchain, size_t size)
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
          DDSI_RMSG_PAYLOADOFF(fragchain->rmsg, DDSI_RDATA_PAYLOAD_OFF(fragchain));
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

static std::unique_ptr<serdata_rmw> serdata_rmw_from_sample_unique(
  const struct ddsi_sertype * typecmn,
  enum ddsi_serdata_kind kind,
  const void * sample)
{
  try {
    const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(typecmn);
    auto d = std::make_unique<serdata_rmw>(type, kind);
    serialize_into_serdata_rmw(d.get(), sample);
    return d;
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return nullptr;
  }
}

static struct ddsi_serdata * serdata_rmw_from_sample(
  const struct ddsi_sertype * typecmn,
  enum ddsi_serdata_kind kind,
  const void * sample)
{
  return serdata_rmw_from_sample_unique(typecmn, kind, sample).release();
}

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

#if CDDS_VERSION > CDDS_VERSION_0_10
static struct ddsi_serdata *serdata_rmw_from_loaned_sample(
  const struct ddsi_sertype *typecmn, enum ddsi_serdata_kind kind,
  const char *sample, dds_loaned_sample_t *loaned_sample,
  bool will_require_cdr)
{
  /*
    type = the type of data being serialized
    kind = the kind of data contained (key or normal)
    sample = the raw sample made into the serdata
    loaned_sample = the loaned buffer in use
    will_require_cdr = whether we will need the CDR (or a highly likely to need it)
  */
  const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(typecmn);
    
  assert(sample == loaned_sample->sample_ptr);
  assert(loaned_sample->metadata->sample_state == (kind == SDK_KEY ? DDS_LOANED_SAMPLE_STATE_RAW_KEY : DDS_LOANED_SAMPLE_STATE_RAW_DATA));
  assert(loaned_sample->metadata->cdr_identifier == DDSI_RTPS_SAMPLE_NATIVE);
  assert(loaned_sample->metadata->cdr_options == 0);

  struct std::unique_ptr<serdata_rmw> d;
  if (will_require_cdr)
  {
    // If serialization is/will be required, construct the serdata the normal way
    d = serdata_rmw_from_sample_unique(type, kind, sample);
  }
  else
  {
    // If we know there is no neeed for the serialized representation (so only PSMX and "memcpy safe"),
    // construct an empty serdata and stay away from the serializers
    d = std::make_unique<serdata_rmw>(type, kind);
  }
  if (d == nullptr)
    return nullptr;
  else
  {
    // now owner of loan
    d->loan = loaned_sample;
    return d.release();
  }
}

static bool loaned_sample_state_to_serdata_kind (dds_loaned_sample_state_t lss, enum ddsi_serdata_kind& kind)
{
  switch (lss)
  {
    case DDS_LOANED_SAMPLE_STATE_RAW_KEY:
    case DDS_LOANED_SAMPLE_STATE_SERIALIZED_KEY:
      kind = SDK_KEY;
      return true;
    case DDS_LOANED_SAMPLE_STATE_RAW_DATA:
    case DDS_LOANED_SAMPLE_STATE_SERIALIZED_DATA:
      kind = SDK_DATA;
      return true;
    case DDS_LOANED_SAMPLE_STATE_UNITIALIZED:
      // invalid
      return false;
  }
  // "impossible" value
  return false;
}

static struct ddsi_serdata * serdata_rmw_from_psmx(
  const struct ddsi_sertype * typecmn, dds_loaned_sample_t *loaned_sample)
{
  try {
    const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(typecmn);
    struct dds_psmx_metadata * const md = loaned_sample->metadata;
    enum ddsi_serdata_kind kind;
    if (!loaned_sample_state_to_serdata_kind (md->sample_state, kind))
      return nullptr;

    auto d = std::make_unique<serdata_rmw>(type, kind);
    d->statusinfo = md->statusinfo;
    d->timestamp.v = md->timestamp;
    switch (md->sample_state)
    {
      case DDS_LOANED_SAMPLE_STATE_UNITIALIZED:
        assert (0);
        return nullptr;
      case DDS_LOANED_SAMPLE_STATE_RAW_KEY:
        // nothing to be done for key (yet)
        break;
      case DDS_LOANED_SAMPLE_STATE_RAW_DATA:
        d->loan = loaned_sample;
        dds_loaned_sample_ref (d->loan);
        break;
      case DDS_LOANED_SAMPLE_STATE_SERIALIZED_KEY:
        // nothing to be done for key (yet)
        break;
      case DDS_LOANED_SAMPLE_STATE_SERIALIZED_DATA:
        // ugly hack - drops `d`, makes a new one ... :(
        return serdata_rmw_from_serialized_message(typecmn, loaned_sample->sample_ptr, md->sample_size);
    }
    return d.release();
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return nullptr;
  }
}
#elif defined DDS_HAS_SHM
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
#endif

static struct ddsi_serdata * serdata_rmw_to_untyped(const struct ddsi_serdata * dcmn)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  auto d1 = new serdata_rmw(d->type, SDK_KEY);
  d1->type = nullptr;
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
    const struct sertype_rmw * type = static_cast<const struct sertype_rmw *>(d->type);
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
#if CDDS_VERSION > CDDS_VERSION_0_10
  , serdata_rmw_from_loaned_sample,
  serdata_rmw_from_psmx
#elif defined DDS_HAS_SHM
  , ddsi_serdata_iox_size,
  serdata_rmw_from_iox
#endif
};

static void sertype_rmw_free(struct ddsi_sertype * tpcmn)
{
  struct sertype_rmw * tp = static_cast<struct sertype_rmw *>(tpcmn);
  ddsi_sertype_fini(tpcmn);
  if (tp->type_support.type_support_) {
    if (using_introspection_c_typesupport(tp->type_support.typesupport_identifier_)) {
      delete static_cast<TypeSupport_c *>(tp->type_support.type_support_);
    } else if (using_introspection_cpp_typesupport(tp->type_support.typesupport_identifier_)) {
      delete static_cast<TypeSupport_cpp *>(tp->type_support.type_support_);
    }
    tp->type_support.type_support_ = NULL;
  }
#if DDS_HAS_TYPELIB
  ddsrt_free((void*)tp->type_information.data);
  ddsrt_free((void*)tp->type_mapping.data);
#endif
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

#if DDS_HAS_TYPELIB
static ddsi_typeid_t * sertype_rmw_typeid (const struct ddsi_sertype * d, ddsi_typeid_kind_t kind)
{
  assert(d);
  const struct sertype_rmw *tp = static_cast<const struct sertype_rmw *>(d);
  ddsi_typeinfo_t *type_info = ddsi_typeinfo_deser(
      tp->type_information.data, tp->type_information.sz);
  if (type_info == NULL)
    return NULL;
  ddsi_typeid_t *type_id = ddsi_typeinfo_typeid(type_info, kind);

  dds_free_typeinfo(type_info);

  return type_id;
}

static ddsi_typemap_t * sertype_rmw_typemap (const struct ddsi_sertype * d)
{
  assert(d);
  const struct sertype_rmw *tp = static_cast<const struct sertype_rmw *>(d); 
  return ddsi_typemap_deser (tp->type_mapping.data, tp->type_mapping.sz);
}

static ddsi_typeinfo_t *sertype_rmw_typeinfo (const struct ddsi_sertype * d)
{
  assert(d);
  const struct sertype_rmw *tp = static_cast<const struct sertype_rmw *>(d);  
  return ddsi_typeinfo_deser (tp->type_information.data, tp->type_information.sz);
}

static struct ddsi_sertype * sertype_rmw_derive_sertype (
  const struct ddsi_sertype *base_sertype, 
  dds_data_representation_id_t data_representation, 
  dds_type_consistency_enforcement_qospolicy_t tce_qos)
{
  const struct sertype_rmw *tp = static_cast<const struct sertype_rmw *>(base_sertype);
  struct sertype_rmw *derived_sertype = NULL;
 
  assert(base_sertype);

  (void) tce_qos;

  if (base_sertype->serdata_ops == &serdata_rmw_ops)
    derived_sertype = (struct sertype_rmw *) base_sertype;
  else
  {
    derived_sertype = static_cast<sertype_rmw*>(ddsrt_memdup (tp, sizeof (*derived_sertype)));
    ddsrt_atomic_st32 (&derived_sertype->flags_refc, 1);
    derived_sertype->base_sertype = ddsi_sertype_ref (base_sertype);
    derived_sertype->serdata_ops = &serdata_rmw_ops;
    derived_sertype->allowed_data_representation = data_representation;
  }

  return (struct ddsi_sertype *) derived_sertype;
}
#endif
static const struct ddsi_sertype_ops sertype_rmw_ops = {
  ddsi_sertype_v0,
  nullptr,
  sertype_rmw_free,
  sertype_rmw_zero_samples,
  sertype_rmw_realloc_samples,
  sertype_rmw_free_samples,
  sertype_rmw_equal,
  sertype_rmw_hash
  /* type discovery, assignability checking only if cyclone has type library */
#if DDS_HAS_TYPELIB
  ,
  sertype_rmw_typeid,
  sertype_rmw_typemap,
  sertype_rmw_typeinfo,
  sertype_rmw_derive_sertype
#else
  ,
  nullptr,
  nullptr,
  nullptr,
  nullptr
#endif
  ,
  sertype_get_serialized_size,
  sertype_serialize_into
};

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
  std::string message_name(members->message_name_);

  if (!message_namespace.empty()) {
    // Find and replace C namespace separator with C++, in case this is using C typesupport
    message_namespace = std::regex_replace(message_namespace, std::regex("__"), "::");
    ss << message_namespace << "::";
  }

  ss << "dds_::" << message_name << "_";
  return ss.str();
}

#if DDS_HAS_TYPELIB
dds_dynamic_type_descriptor_t get_dynamic_type_descriptor_prim(
  dds_dynamic_type_kind_t kind, const char *name, uint32_t num_bounds, const uint32_t *bounds, dds_dynamic_type_kind_t type)
{
  dds_dynamic_type_descriptor_t desc{};
  desc.kind = kind;
  desc.name = name;
  desc.num_bounds = num_bounds;
  desc.bounds = bounds; 
  desc.element_type = DDS_DYNAMIC_TYPE_SPEC_PRIM(type);
  return desc;
}

dds_dynamic_type_descriptor_t get_dynamic_type_descriptor(
  dds_dynamic_type_kind_t kind, const char *name, uint32_t num_bounds, const uint32_t *bounds, dds_dynamic_type_t type)
{
  dds_dynamic_type_descriptor_t desc{};
  desc.kind = kind;
  desc.name = name;
  desc.num_bounds = num_bounds;
  desc.bounds = bounds;
  desc.element_type = DDS_DYNAMIC_TYPE_SPEC(type);
  return desc;
}

dds_dynamic_member_descriptor_t get_dynamic_member_descriptor_prim(
  dds_dynamic_type_kind_t type, const char *name)
{
  return DDS_DYNAMIC_MEMBER_PRIM(type, name);
}

dds_dynamic_member_descriptor_t get_dynamic_member_descriptor(
  dds_dynamic_type_t ddt, const char *name)
{
  return DDS_DYNAMIC_MEMBER(ddt, name);
}


template<typename MemberType>
static void dynamic_type_add_array_prim(
  dds_dynamic_type_t * dstruct, dds_entity_t dds_ppant, const MemberType * member, const dds_dynamic_type_kind_t type)
{
  dds_dynamic_type_t ddt;

  if (member->array_size_) 
  { 
    uint32_t array_size = static_cast<uint32_t>(member->array_size_);
    if (!member->is_upper_bound_)
    {
      ddt = dds_dynamic_type_create(dds_ppant, 
        get_dynamic_type_descriptor_prim(DDS_DYNAMIC_ARRAY, member->name_, 1, &array_size, type));
    } else {
      ddt = dds_dynamic_type_create(dds_ppant, 
        get_dynamic_type_descriptor_prim(DDS_DYNAMIC_SEQUENCE, member->name_, 1, &array_size, type)); 
    }
  } else {
    ddt = dds_dynamic_type_create(dds_ppant,
       get_dynamic_type_descriptor_prim(DDS_DYNAMIC_SEQUENCE, member->name_, 0, nullptr, type)); 
  }

  dds_return_t ret = dds_dynamic_type_add_member(dstruct, get_dynamic_member_descriptor(ddt, member->name_));
  assert(ret == DDS_RETCODE_OK);
}

template<typename MemberType>
static void dynamic_type_add_array(
  dds_dynamic_type * dstruct, dds_entity_t dds_ppant, const MemberType * member, const dds_dynamic_type_t ddt)
{
  dds_dynamic_type_t dseq;

  if (member->array_size_) 
  { 
    uint32_t array_size = static_cast<uint32_t>(member->array_size_);
    if (!member->is_upper_bound_)
    {
      dseq = dds_dynamic_type_create(dds_ppant, 
       get_dynamic_type_descriptor(DDS_DYNAMIC_ARRAY, member->name_, 1, &array_size, ddt));
    } else {
      dseq = dds_dynamic_type_create(dds_ppant, 
        get_dynamic_type_descriptor(DDS_DYNAMIC_SEQUENCE, member->name_, 1, &array_size, ddt));
    }
  } else {
    dseq = dds_dynamic_type_create(dds_ppant,
     get_dynamic_type_descriptor(DDS_DYNAMIC_SEQUENCE, member->name_, 0, nullptr, ddt)); 
  }

  dds_return_t ret = dds_dynamic_type_add_member(dstruct, get_dynamic_member_descriptor(dseq, member->name_));
  assert(ret == DDS_RETCODE_OK);
}

template<typename MemberType>
static void dynamic_type_add_member(
    dds_dynamic_type_t * dstruct, dds_entity_t dds_ppant, const MemberType * member, const dds_dynamic_type_kind_t type)
{
  dds_return_t ret;

  assert(member->type_id_ != rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING);
  assert(member->type_id_ != rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE);

  if (!member->is_array_)
  {
    ret = dds_dynamic_type_add_member(dstruct, get_dynamic_member_descriptor_prim(type, member->name_));
    assert(ret == DDS_RETCODE_OK);
  } else {
    dynamic_type_add_array_prim(dstruct, dds_ppant, member, type);
  }
}

static void dynamic_type_register(struct sertype_rmw * st, dds_dynamic_type_t dt, dds_entity_t dds_ppant)
{
  dds_typeinfo_t *type_info;
  auto rc = dds_dynamic_type_register(&dt, &type_info);
  if (rc != DDS_RETCODE_OK)
  {
    RMW_SET_ERROR_MSG("dds_dynamic_type_register failed to register type");
    goto fail_typeinfo;
  }
  
  dds_topic_descriptor_t *desc;
  rc = dds_create_topic_descriptor(
    DDS_FIND_SCOPE_GLOBAL, dds_ppant, type_info, 0, &desc);
  if (rc != DDS_RETCODE_OK)
  {
    RMW_SET_ERROR_MSG("dds_create_topic_descriptor failed to create descriptor");
    goto fail_descriptor;
  }

  st->type_information.data = static_cast<const unsigned char *>(ddsrt_memdup(desc->type_information.data, desc->type_information.sz));
  st->type_information.sz = desc->type_information.sz;
  st->type_mapping.data = static_cast<const unsigned char *>(ddsrt_memdup(desc->type_mapping.data, desc->type_mapping.sz));
  st->type_mapping.sz = desc->type_mapping.sz;

  dds_dynamic_type_unref(&dt);
fail_descriptor:
  dds_delete_topic_descriptor(desc);
fail_typeinfo:
  dds_free_typeinfo(type_info);
}

template<typename MembersType>
static bool construct_dds_dynamic_type(
  dds_dynamic_type_t * dstruct, dds_entity_t dds_ppant, const MembersType * members)
{
  assert(members);
  assert(dds_ppant);
 
  dds_return_t ret;

  for (uint32_t i = 0; i < members->member_count_; ++i)
  {
    const auto * member = members->members_ + i; 
    switch (member->type_id_) 
    {
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BOOL:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_BOOLEAN);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_BYTE:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_BYTE);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT8);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_CHAR:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_CHAR8);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT8:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_INT8);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT32:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_FLOAT32);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_FLOAT64:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_FLOAT64);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT16:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_INT16);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT16:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT16);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT32:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_INT32);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT32:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT32);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_INT64:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_INT64);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT64:
        dynamic_type_add_member(dstruct, dds_ppant, member, DDS_DYNAMIC_UINT64);
        break;
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING:
        {
          dds_dynamic_type_t ddt;
          if (member->string_upper_bound_)
          {
            uint32_t string_size = static_cast<uint32_t>(member->string_upper_bound_);
            ddt = dds_dynamic_type_create(dds_ppant,
              get_dynamic_type_descriptor(DDS_DYNAMIC_STRING8, nullptr, 1, &string_size, {})); 
          } else {
            ddt = dds_dynamic_type_create(dds_ppant, 
              get_dynamic_type_descriptor(DDS_DYNAMIC_STRING8, nullptr, 0, nullptr, {})); 
          }
          if (!member->is_array_)
          {
            ret = dds_dynamic_type_add_member(dstruct, get_dynamic_member_descriptor(ddt, member->name_));
            assert(ret == DDS_RETCODE_OK);
          } else {
            dynamic_type_add_array(dstruct, dds_ppant, member, ddt);
          }

          break;
        }
      case ::rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE:
        {
          if (!member->members_)
          {
            RMW_SET_ERROR_MSG("no members value");
            return false;
          }

          auto sub_members = static_cast<const MembersType *>(member->members_->data);
          dds_dynamic_type_t ddt;
          ddt = dds_dynamic_type_create(dds_ppant, 
            get_dynamic_type_descriptor(DDS_DYNAMIC_STRUCTURE, create_type_name<MembersType>(member->members_->data).c_str(), 0, nullptr, {}));

          dds_dynamic_type_set_extensibility(&ddt, DDS_DYNAMIC_TYPE_EXT_FINAL);

          if (!construct_dds_dynamic_type(&ddt, dds_ppant, sub_members))
          {
            RMW_SET_ERROR_MSG("construct_dds_dynamic_type error");
            return false;
          }

          if (!member->is_array_)
          {
            ret = dds_dynamic_type_add_member(dstruct, get_dynamic_member_descriptor(ddt, member->name_));
            assert(ret == DDS_RETCODE_OK);
          } else {
            dynamic_type_add_array(dstruct, dds_ppant, member, ddt);
          }
          
          break;
        }
      default:
        RMW_SET_ERROR_MSG((std::string("unknown type id ") + std::to_string(member->type_id_)).c_str());
        return false;
    }
  }

  return true;
}
#endif
void create_msg_dds_dynamic_type(const char * type_support_identifier, const void * untyped_members, dds_entity_t dds_ppant, struct sertype_rmw * st)
{
#if DDS_HAS_TYPELIB
  if (using_introspection_c_typesupport(type_support_identifier)) 
  {
    auto members = static_cast<const rosidl_typesupport_introspection_c__MessageMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(dds_ppant, 
      get_dynamic_type_descriptor(DDS_DYNAMIC_STRUCTURE, create_type_name<rosidl_typesupport_introspection_c__MessageMembers_s>(untyped_members).c_str(), 0, nullptr, {}));
    
    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members))
      dynamic_type_register(st, dstruct, dds_ppant);
#if THROW_ON_DYNAMIC_TYPE_ERROR
    else
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
  } 
  else if (using_introspection_cpp_typesupport(type_support_identifier)) 
  {
    auto members = static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(dds_ppant, 
      get_dynamic_type_descriptor(DDS_DYNAMIC_STRUCTURE, create_type_name<rosidl_typesupport_introspection_cpp::MessageMembers_s>(untyped_members).c_str(), 0, nullptr, {}));

    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members))
      dynamic_type_register(st, dstruct, dds_ppant);
#if THROW_ON_DYNAMIC_TYPE_ERROR
    else
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
  } 
  else 
  {
    throw std::runtime_error("create_dds_dynamic_type, unsupported typesupport"); 
  }
#else
  static_cast<void>(type_support_identifier);
  static_cast<void>(untyped_members);
  static_cast<void>(dds_ppant);
  static_cast<void>(st);
#endif
}

void create_req_dds_dynamic_type(const char * type_support_identifier, const void * untyped_members, dds_entity_t dds_ppant, struct sertype_rmw * st)
{
#if DDS_HAS_TYPELIB
  if (using_introspection_c_typesupport(type_support_identifier)) 
  {
    auto members = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(dds_ppant, 
      get_dynamic_type_descriptor(DDS_DYNAMIC_STRUCTURE, create_type_name<rosidl_typesupport_introspection_c__MessageMembers_s>(untyped_members).c_str(), 0, nullptr, {})); 
    
    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members->request_members_))
      dynamic_type_register(st, dstruct, dds_ppant);
#if THROW_ON_DYNAMIC_TYPE_ERROR
    else
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
  } 
  else if (using_introspection_cpp_typesupport(type_support_identifier)) 
  {
    auto members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(dds_ppant, 
      get_dynamic_type_descriptor(DDS_DYNAMIC_STRUCTURE, create_type_name<rosidl_typesupport_introspection_cpp::MessageMembers_s>(untyped_members).c_str(), 0, nullptr, {})); 

    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members->request_members_))
      dynamic_type_register(st, dstruct, dds_ppant);
#if THROW_ON_DYNAMIC_TYPE_ERROR
    else
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
  } 
  else 
  {
    throw std::runtime_error("create_dds_dynamic_type, unsupported typesupport"); 
  }
#else
  static_cast<void>(type_support_identifier);
  static_cast<void>(untyped_members);
  static_cast<void>(dds_ppant);
  static_cast<void>(st);
#endif
}

void create_res_dds_dynamic_type(const char * type_support_identifier, const void * untyped_members, dds_entity_t dds_ppant, struct sertype_rmw * st)
{
#if DDS_HAS_TYPELIB
  if (using_introspection_c_typesupport(type_support_identifier)) 
  {
    auto members = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(dds_ppant, 
     get_dynamic_type_descriptor(DDS_DYNAMIC_STRUCTURE, create_type_name<rosidl_typesupport_introspection_c__MessageMembers_s>(untyped_members).c_str(), 0, nullptr, {})); 
    
    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members->response_members_))
      dynamic_type_register(st, dstruct, dds_ppant);
#if THROW_ON_DYNAMIC_TYPE_ERROR    
    else
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
  } 
  else if (using_introspection_cpp_typesupport(type_support_identifier)) 
  {
    auto members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers_s *>(untyped_members);
    auto dstruct = dds_dynamic_type_create(dds_ppant,
      get_dynamic_type_descriptor(DDS_DYNAMIC_STRUCTURE, create_type_name<rosidl_typesupport_introspection_cpp::MessageMembers_s>(untyped_members).c_str(), 0, nullptr, {})); 
    dds_dynamic_type_set_extensibility(&dstruct, DDS_DYNAMIC_TYPE_EXT_FINAL);

    if (construct_dds_dynamic_type(&dstruct, dds_ppant, members->response_members_))
      dynamic_type_register(st, dstruct, dds_ppant); 
#if THROW_ON_DYNAMIC_TYPE_ERROR
    else
      throw std::runtime_error("construct_dds_dynamic_type failed");
#endif
  } 
  else 
  {
    throw std::runtime_error("create_dds_dynamic_type, unsupported typesupport"); 
  }

#else
  static_cast<void>(type_support_identifier);
  static_cast<void>(untyped_members);
  static_cast<void>(dds_ppant);
  static_cast<void>(st);
#endif
}

struct sertype_rmw * create_sertype(
  const char * type_support_identifier,
  void * type_support, bool is_request_header,
  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> message_type,
  const uint32_t sample_size, const bool is_fixed_type)
{
  struct sertype_rmw * st = new struct sertype_rmw;
  std::string type_name = get_type_name(type_support_identifier, type_support);
#if CDDS_VERSION > CDDS_VERSION_0_10
  const uint32_t flags = 0;
  dds_data_type_properties_t props = 0;
  if (is_fixed_type) {
    props |= DDS_DATA_TYPE_IS_MEMCPY_SAFE;
  }
  ddsi_sertype_init_props(
    static_cast<struct ddsi_sertype *>(st),
    type_name.c_str(), &sertype_rmw_ops, &serdata_rmw_ops,
    sample_size, props, DDS_DATA_REPRESENTATION_FLAG_XCDR1, flags);
#else
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
#endif // DDS_HAS_SHM
#endif
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
