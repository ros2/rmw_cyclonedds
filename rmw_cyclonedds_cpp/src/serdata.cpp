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

#include <rmw/allocators.h>

#include <cstring>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

#include "Serialization.hpp"
#include "TypeSupport2.hpp"
#include "bytewise.hpp"
#include "dds/ddsi/q_radmin.h"
#include "rmw/error_handling.h"
#include "rmw_cyclonedds_cpp/MessageTypeSupport.hpp"
#include "rmw_cyclonedds_cpp/ServiceTypeSupport.hpp"
#include "rmw_cyclonedds_cpp/serdes.hpp"

/* Cyclone's nn_keyhash got renamed to ddsi_keyhash and shuffled around in the header
   files to avoid pulling in tons of things just for a definition of a keyhash.  This
   coincides with the introduction of DDS Security, which itself adds another function
   that was missing.

   If that additional function is not needed, it is known as nn_keyhash and only a forward
   declaration is available; else it is known as ddsi_keyhash and a definition is
   available */
#if !DDSI_SERDATA_HAS_GET_KEYHASH
#define ddsi_keyhash nn_keyhash
#endif

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

static uint32_t serdata_rmw_size(const struct ddsi_serdata * dcmn)
{
  size_t size = static_cast<const serdata_rmw *>(dcmn)->size();
  uint32_t size_u32 = static_cast<uint32_t>(size);
  assert(size == size_u32);
  return size_u32;
}

static void serdata_rmw_free(struct ddsi_serdata * dcmn)
{
  auto * d = static_cast<const serdata_rmw *>(dcmn);
  delete d;
}

static struct ddsi_serdata * serdata_rmw_from_ser(
  const struct ddsi_sertopic * topic,
  enum ddsi_serdata_kind kind,
  const struct nn_rdata * fragchain, size_t size)
{
  auto d = std::make_unique<serdata_rmw>(topic, kind);
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
}

#if DDSI_SERDATA_HAS_FROM_SER_IOV
static struct ddsi_serdata * serdata_rmw_from_ser_iov(
  const struct ddsi_sertopic * topic,
  enum ddsi_serdata_kind kind,
  ddsrt_msg_iovlen_t niov, const ddsrt_iovec_t * iov,
  size_t size)
{
  auto d = std::make_unique<serdata_rmw>(topic, kind);
  d->resize(size);

  auto cursor = d->data();
  for (ddsrt_msg_iovlen_t i = 0; i < niov; i++) {
    memcpy(cursor, iov[i].iov_base, iov[i].iov_len);
    cursor = byte_offset(cursor, iov[i].iov_len);
  }
  return d.release();
}
#endif

static struct ddsi_serdata * serdata_rmw_from_keyhash(
  const struct ddsi_sertopic * topic,
  const struct ddsi_keyhash * keyhash)
{
  static_cast<void>(keyhash);    // unused
  /* there is no key field, so from_keyhash is trivial */
  return new serdata_rmw(topic, SDK_KEY);
}

static struct ddsi_serdata * serdata_rmw_from_sample(
  const struct ddsi_sertopic * topiccmn,
  enum ddsi_serdata_kind kind,
  const void * sample)
{
  try {
    const struct sertopic_rmw * topic = static_cast<const struct sertopic_rmw *>(topiccmn);
    auto d = std::make_unique<serdata_rmw>(topic, kind);
    if (kind != SDK_DATA) {
      /* ROS2 doesn't do keys, so SDK_KEY is trivial */
    } else if (!topic->is_request_header) {
      size_t sz = topic->cdr_writer->get_serialized_size(sample);
      d->resize(sz);
      topic->cdr_writer->serialize(d->data(), sample);
    } else {
      /* inject the service invocation header data into the CDR stream --
       * I haven't checked how it is done in the official RMW implementations, so it is
       * probably incompatible. */
      auto wrap = *static_cast<const cdds_request_wrapper_t *>(sample);
      size_t sz = topic->cdr_writer->get_serialized_size(wrap);
      d->resize(sz);
      topic->cdr_writer->serialize(d->data(), wrap);
    }
    return d.release();
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return nullptr;
  }
}

struct ddsi_serdata * serdata_rmw_from_serialized_message(
  const struct ddsi_sertopic * topiccmn,
  const void * raw, size_t size)
{
  const struct sertopic_rmw * topic = static_cast<const struct sertopic_rmw *>(topiccmn);
  auto d = new serdata_rmw(topic, SDK_DATA);
  d->resize(size);
  memcpy(d->data(), raw, size);
  return d;
}

static struct ddsi_serdata * serdata_rmw_to_topicless(const struct ddsi_serdata * dcmn)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  auto d1 = new serdata_rmw(d->topic, SDK_KEY);
  d1->topic = nullptr;
  return d1;
}

static void serdata_rmw_to_ser(const struct ddsi_serdata * dcmn, size_t off, size_t sz, void * buf)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  memcpy(buf, byte_offset(d->data(), off), sz);
}

static struct ddsi_serdata * serdata_rmw_to_ser_ref(
  const struct ddsi_serdata * dcmn, size_t off,
  size_t sz, ddsrt_iovec_t * ref)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
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
    const struct sertopic_rmw * topic = static_cast<const struct sertopic_rmw *>(d->topic);
    assert(bufptr == NULL);
    assert(buflim == NULL);
    if (d->kind != SDK_DATA) {
      /* ROS2 doesn't do keys in a meaningful way yet */
    } else if (!topic->is_request_header) {
      cycdeser sd(d->data(), d->size());
      if (using_introspection_c_typesupport(topic->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_c *>(topic->type_support.type_support_);
        return typed_typesupport->deserializeROSmessage(sd, sample);
      } else if (using_introspection_cpp_typesupport(topic->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_cpp *>(topic->type_support.type_support_);
        return typed_typesupport->deserializeROSmessage(sd, sample);
      }
    } else {
      /* The "prefix" lambda is there to inject the service invocation header data into the CDR
        stream -- I haven't checked how it is done in the official RMW implementations, so it is
        probably incompatible. */
      cdds_request_wrapper_t * const wrap = static_cast<cdds_request_wrapper_t *>(sample);
      auto prefix = [wrap](cycdeser & ser) {ser >> wrap->header.guid; ser >> wrap->header.seq;};
      cycdeser sd(d->data(), d->size());
      if (using_introspection_c_typesupport(topic->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_c *>(topic->type_support.type_support_);
        return typed_typesupport->deserializeROSmessage(sd, wrap->data, prefix);
      } else if (using_introspection_cpp_typesupport(topic->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_cpp *>(topic->type_support.type_support_);
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

static bool serdata_rmw_topicless_to_sample(
  const struct ddsi_sertopic * topic,
  const struct ddsi_serdata * dcmn, void * sample,
  void ** bufptr, void * buflim)
{
  static_cast<void>(topic);
  static_cast<void>(dcmn);
  static_cast<void>(sample);
  static_cast<void>(bufptr);
  static_cast<void>(buflim);
  /* ROS2 doesn't do keys in a meaningful way yet */
  return true;
}

static bool serdata_rmw_eqkey(const struct ddsi_serdata * a, const struct ddsi_serdata * b)
{
  static_cast<void>(a);
  static_cast<void>(b);
  /* ROS2 doesn't do keys in a meaningful way yet */
  return true;
}

#if DDSI_SERDATA_HAS_PRINT
static size_t serdata_rmw_print(
  const struct ddsi_sertopic * tpcmn, const struct ddsi_serdata * dcmn, char * buf, size_t bufsize)
{
  try {
    auto d = static_cast<const serdata_rmw *>(dcmn);
    const struct sertopic_rmw * topic = static_cast<const struct sertopic_rmw *>(tpcmn);
    if (d->kind != SDK_DATA) {
      /* ROS2 doesn't do keys in a meaningful way yet */
      return static_cast<size_t>(snprintf(buf, bufsize, ":k:{}"));
    } else if (!topic->is_request_header) {
      cycprint sd(buf, bufsize, d->data(), d->size());
      if (using_introspection_c_typesupport(topic->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_c *>(topic->type_support.type_support_);
        return typed_typesupport->printROSmessage(sd);
      } else if (using_introspection_cpp_typesupport(topic->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_cpp *>(topic->type_support.type_support_);
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
      if (using_introspection_c_typesupport(topic->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_c *>(topic->type_support.type_support_);
        return typed_typesupport->printROSmessage(sd, prefix);
      } else if (using_introspection_cpp_typesupport(topic->type_support.typesupport_identifier_)) {
        auto typed_typesupport =
          static_cast<MessageTypeSupport_cpp *>(topic->type_support.type_support_);
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
#endif

#if DDSI_SERDATA_HAS_GET_KEYHASH
static void serdata_rmw_get_keyhash(
  const struct ddsi_serdata * d, struct ddsi_keyhash * buf,
  bool force_md5)
{
  /* ROS2 doesn't do keys in a meaningful way yet, this is never called for topics without
     key fields */
  static_cast<void>(d);
  static_cast<void>(force_md5);
  memset(buf, 0, sizeof(*buf));
}
#endif

static const struct ddsi_serdata_ops serdata_rmw_ops = {
  serdata_rmw_eqkey,
  serdata_rmw_size,
  serdata_rmw_from_ser,
#if DDSI_SERDATA_HAS_FROM_SER_IOV
  serdata_rmw_from_ser_iov,
#endif
  serdata_rmw_from_keyhash,
  serdata_rmw_from_sample,
  serdata_rmw_to_ser,
  serdata_rmw_to_ser_ref,
  serdata_rmw_to_ser_unref,
  serdata_rmw_to_sample,
  serdata_rmw_to_topicless,
  serdata_rmw_topicless_to_sample,
  serdata_rmw_free
#if DDSI_SERDATA_HAS_PRINT
  , serdata_rmw_print
#endif
#if DDSI_SERDATA_HAS_GET_KEYHASH
  , serdata_rmw_get_keyhash
#endif
};

static void sertopic_rmw_free(struct ddsi_sertopic * tpcmn)
{
  struct sertopic_rmw * tp = static_cast<struct sertopic_rmw *>(tpcmn);
#if DDSI_SERTOPIC_HAS_TOPICKIND_NO_KEY
  ddsi_sertopic_fini(tpcmn);
#endif
  delete tp;
}

static void sertopic_rmw_zero_samples(const struct ddsi_sertopic * d, void * samples, size_t count)
{
  static_cast<void>(d);
  static_cast<void>(samples);
  static_cast<void>(count);
  /* Not using code paths that rely on the samples getting zero'd out */
}

static void sertopic_rmw_realloc_samples(
  void ** ptrs, const struct ddsi_sertopic * d, void * old,
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

static void sertopic_rmw_free_samples(
  const struct ddsi_sertopic * d, void ** ptrs, size_t count,
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

#if DDSI_SERTOPIC_HAS_EQUAL_AND_HASH
bool sertopic_rmw_equal(
  const struct ddsi_sertopic * acmn, const struct ddsi_sertopic * bcmn)
{
  /* A bit of a guess: topics with the same name & type name are really the same if they have
     the same type support identifier as well */
  const struct sertopic_rmw * a = static_cast<const struct sertopic_rmw *>(acmn);
  const struct sertopic_rmw * b = static_cast<const struct sertopic_rmw *>(bcmn);
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

uint32_t sertopic_rmw_hash(const struct ddsi_sertopic * tpcmn)
{
  const struct sertopic_rmw * tp = static_cast<const struct sertopic_rmw *>(tpcmn);
  uint32_t h2 = static_cast<uint32_t>(std::hash<bool>{} (tp->is_request_header));
  uint32_t h1 =
    static_cast<uint32_t>(std::hash<std::string>{} (std::string(
      tp->type_support.typesupport_identifier_)));
  return h1 ^ h2;
}
#endif

static const struct ddsi_sertopic_ops sertopic_rmw_ops = {
  sertopic_rmw_free,
  sertopic_rmw_zero_samples,
  sertopic_rmw_realloc_samples,
  sertopic_rmw_free_samples
#if DDSI_SERTOPIC_HAS_EQUAL_AND_HASH
  , sertopic_rmw_equal,
  sertopic_rmw_hash
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

struct sertopic_rmw * create_sertopic(
  const char * topicname, const char * type_support_identifier,
  void * type_support, bool is_request_header,
  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> message_type)
{
  struct sertopic_rmw * st = new struct sertopic_rmw;
#if DDSI_SERTOPIC_HAS_TOPICKIND_NO_KEY
  std::string type_name = get_type_name(type_support_identifier, type_support);
  ddsi_sertopic_init(
    static_cast<struct ddsi_sertopic *>(st), topicname,
    type_name.c_str(), &sertopic_rmw_ops, &serdata_rmw_ops, true);
#else
  memset(st, 0, sizeof(struct ddsi_sertopic));
  st->cpp_name = std::string(topicname);
  st->cpp_type_name = get_type_name(type_support_identifier, type_support);
  st->cpp_name_type_name = st->cpp_name + std::string(";") + std::string(st->cpp_type_name);
  st->ops = &sertopic_rmw_ops;
  st->serdata_ops = &serdata_rmw_ops;
  st->serdata_basehash = ddsi_sertopic_compute_serdata_basehash(st->serdata_ops);
  st->name_type_name = const_cast<char *>(st->cpp_name_type_name.c_str());
  st->name = const_cast<char *>(st->cpp_name.c_str());
  st->type_name = const_cast<char *>(st->cpp_type_name.c_str());
  st->iid = ddsi_iid_gen();
  ddsrt_atomic_st32(&st->refc, 1);
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

serdata_rmw::serdata_rmw(const ddsi_sertopic * topic, ddsi_serdata_kind kind)
: ddsi_serdata{}
{
  ddsi_serdata_init(this, topic, kind);
}
