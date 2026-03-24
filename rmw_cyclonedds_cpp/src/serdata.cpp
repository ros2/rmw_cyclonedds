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
#include <vector>

#include "dds/dds.h"
#include "cdds_version.hpp"

#include "rmw/allocators.h"
#include "Serialization.hpp"
#include "TypeSupport2.hpp"
#include "bytewise.hpp"
#if CDDS_VERSION == CDDS_VERSION_0_10
#include "dds/ddsi/q_radmin.h"
#define ddsi_rdata nn_rdata
#define DDSI_RMSG_PAYLOADOFF(rmsg, rdata) NN_RMSG_PAYLOADOFF((rmsg), (rdata))
#define DDSI_RDATA_PAYLOAD_OFF(rdata) NN_RDATA_PAYLOAD_OFF((rdata))
#else
#include "dds/ddsi/ddsi_radmin.h"
#include "dds/ddsc/dds_psmx.h"
#endif
#include "rmw/error_handling.h"
#include "dds/ddsrt/mh3.h"
#include "dds/ddsrt/md5.h"

#if DDS_HAS_TYPELIB
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

void serdata_rmw::set_key(size_t keysize, std::unique_ptr<byte[]> & key)
{
  m_key.reset(key.release());
  m_keysize = keysize;
  hash = ddsrt_mh3(m_key.get(), keysize, type->serdata_basehash);
}

void serdata_rmw::set_key(size_t keysize, const void * key)
{
  if (!keysize) {
    m_keysize = 0;
    m_key.reset();
  } else {
    auto ukey = std::make_unique<byte[]>(keysize);
    std::memcpy(ukey.get(), key, keysize);
    serdata_rmw::set_key(keysize, ukey);
  }
}

serdata_rmw::serdata_rmw(const ddsi_sertype * type, ddsi_serdata_kind kind)
: ddsi_serdata{}
{
  ddsi_serdata_init(this, type, kind);
}

static bool type_contains_keys(struct ddsi_sertype const * t)
{
#if CDDS_VERSION > CDDS_VERSION_0_10
  return t->data_type_props & DDS_DATA_TYPE_CONTAINS_KEY;
#else
  return !t->typekind_no_key;
#endif
}

static bool serdata_rmw_eqkey(const struct ddsi_serdata * va, const struct ddsi_serdata * vb)
{
  auto a = static_cast<const serdata_rmw *>(va);
  auto b = static_cast<const serdata_rmw *>(vb);
  return a->keysize() == b->keysize() &&
         (a->keysize() == 0 || std::memcmp(a->key(), b->key(), a->keysize()) == 0);
}

static void serdata_rmw_set_key_from_sample(serdata_rmw * d, const void * sample)
{
  auto * type = static_cast<const struct sertype_rmw *>(d->type);
  if (type_contains_keys(type)) {
    const size_t keysize = type->cdr_writer->get_serialized_size(
      sample,
      rmw_cyclonedds_cpp::SampleOrKey::Key);
    auto key = std::make_unique<byte[]>(keysize);
    type->cdr_writer->serialize(key.get(), sample, rmw_cyclonedds_cpp::SampleOrKey::Key);
    d->set_key(keysize, key);
  }
}

static void serdata_rmw_set_key_from_ser(serdata_rmw * d)
{
  auto type = static_cast<const struct sertype_rmw *>(d->type);
  if (type_contains_keys(type)) {
    try {
      std::vector<byte> key;
      type->cdr_reader->extractkey(
        key, d->data(), d->size(),
        (d->kind ==
        SDK_DATA) ? rmw_cyclonedds_cpp::SampleOrKey::Sample : rmw_cyclonedds_cpp::SampleOrKey::Key);
      d->set_key(key.size(), key.data());
    } catch (std::exception & e) {
      RMW_SET_ERROR_MSG(e.what());
    }
  }
}

static void serdata_rmw_serialize_into(serdata_rmw * d, const void * sample)
{
  auto type = static_cast<const struct sertype_rmw *>(d->type);
  try {
    const auto cdrmode = (d->kind ==
      SDK_DATA) ? rmw_cyclonedds_cpp::SampleOrKey::Sample : rmw_cyclonedds_cpp::SampleOrKey::Key;
    size_t sz = type->cdr_writer->get_serialized_size(sample, cdrmode);
    d->resize(sz);
    type->cdr_writer->serialize(d->data(), sample, cdrmode);
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
  }
}

static void serdata_rmw_serialize_into_on_demand(serdata_rmw * d)
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
        serdata_rmw_serialize_into(d, d->loan->sample_ptr);
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
        serdata_rmw_serialize_into(d, d->iox_chunk);
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
  serdata_rmw_serialize_into_on_demand(const_cast<serdata_rmw *>(d));
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
    dds_loaned_sample_unref(d->loan);
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
    serdata_rmw_set_key_from_ser(d.get());
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
    serdata_rmw_set_key_from_ser(d.get());
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
  static_cast<void>(type);
  static_cast<void>(keyhash);
  // can't be bothered to try: not even really needed for RTI compatibility anymore ...
  return nullptr;
}

static std::unique_ptr<serdata_rmw> serdata_rmw_from_sample_unique(
  const struct ddsi_sertype * typecmn,
  enum ddsi_serdata_kind kind,
  const void * sample)
{
  try {
    auto type = static_cast<const struct sertype_rmw *>(typecmn);
    auto d = std::make_unique<serdata_rmw>(type, kind);
    serdata_rmw_set_key_from_sample(d.get(), sample);
    serdata_rmw_serialize_into(d.get(), sample);
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
  enum ddsi_serdata_kind kind,
  const void * raw, size_t size)
{
  ddsrt_iovec_t iov;
  iov.iov_len = static_cast<ddsrt_iov_len_t>(size);
  if (iov.iov_len != size) {
    return nullptr;
  }
  iov.iov_base = const_cast<void *>(raw);
  return serdata_rmw_from_ser_iov(typecmn, kind, 1, &iov, size);
}

#if CDDS_VERSION > CDDS_VERSION_0_10
static struct ddsi_serdata * serdata_rmw_from_loaned_sample(
  const struct ddsi_sertype * typecmn, enum ddsi_serdata_kind kind,
  const char * sample, dds_loaned_sample_t * loaned_sample,
  bool will_require_cdr)
{
  /*
    type = the type of data being serialized
    kind = the kind of data contained (key or normal)
    sample = the raw sample made into the serdata
    loaned_sample = the loaned buffer in use
    will_require_cdr = whether we will need the CDR (or a highly likely to need it)
  */
  auto type = static_cast<const struct sertype_rmw *>(typecmn);

  assert(sample == loaned_sample->sample_ptr);
  assert(
    loaned_sample->metadata->sample_state ==
    (kind == SDK_KEY ? DDS_LOANED_SAMPLE_STATE_RAW_KEY : DDS_LOANED_SAMPLE_STATE_RAW_DATA));
  assert(loaned_sample->metadata->cdr_identifier == DDSI_RTPS_SAMPLE_NATIVE);
  assert(loaned_sample->metadata->cdr_options == 0);

  struct std::unique_ptr<serdata_rmw> d;
  if (will_require_cdr) {
    // If serialization is/will be required, construct the serdata the normal way
    d = serdata_rmw_from_sample_unique(type, kind, sample);
  } else {
    // If we know there is no neeed for the serialized representation (so only PSMX and
    // "memcpy safe"), construct an empty serdata and stay away from the serializers
    d = std::make_unique<serdata_rmw>(type, kind);
    serdata_rmw_set_key_from_sample(d.get(), sample);
  }
  if (d == nullptr) {
    return nullptr;
  } else {
    // now owner of loan
    d->loan = loaned_sample;
    return d.release();
  }
}

static bool loaned_sample_state_to_serdata_kind(
  dds_loaned_sample_state_t lss,
  enum ddsi_serdata_kind & kind)
{
  switch (lss) {
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
  const struct ddsi_sertype * typecmn, dds_loaned_sample_t * loaned_sample)
{
  auto type = static_cast<const struct sertype_rmw *>(typecmn);
  struct dds_psmx_metadata * const md = loaned_sample->metadata;
  enum ddsi_serdata_kind kind;
  if (!loaned_sample_state_to_serdata_kind(md->sample_state, kind)) {
    return nullptr;
  }

  switch (md->sample_state) {
    case DDS_LOANED_SAMPLE_STATE_UNITIALIZED:
      assert(0);
      break;
    case DDS_LOANED_SAMPLE_STATE_SERIALIZED_KEY:
    case DDS_LOANED_SAMPLE_STATE_SERIALIZED_DATA:
      // for simplicity we copy the serialized data into the heap so we can rely on
      // existing code for making a serdata_rmw
      return serdata_rmw_from_serialized_message(
        typecmn, kind, loaned_sample->sample_ptr,
        md->sample_size);
    case DDS_LOANED_SAMPLE_STATE_RAW_KEY:
    case DDS_LOANED_SAMPLE_STATE_RAW_DATA:
      try {
        // reference the sample in (what is presumably) shared memory
        auto d = std::make_unique<serdata_rmw>(type, kind);
        d->statusinfo = md->statusinfo;
        d->timestamp.v = md->timestamp;
        d->loan = loaned_sample;
        serdata_rmw_set_key_from_sample(d.get(), d->loan->sample_ptr);
        // increment refcount after "set_key" so we don't have to worry about possible exceptions
        dds_loaned_sample_ref(d->loan);
        return d.release();
      } catch (std::exception & e) {
        RMW_SET_ERROR_MSG(e.what());
      }
      break;
  }
  return nullptr;
}
#elif defined DDS_HAS_SHM
static struct ddsi_serdata * serdata_rmw_from_iox(
  const struct ddsi_sertype * typecmn,
  enum  ddsi_serdata_kind kind, void * sub, void * iox_buffer)
{
  try {
    auto type = static_cast<const struct sertype_rmw *>(typecmn);
    auto d = std::make_unique<serdata_rmw>(type, kind);
    d->iox_chunk = iox_buffer;
    d->iox_subscriber = sub;
    serdata_rmw_set_key_from_sample(d.get(), d->iox_chunk);
    return d.release();
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return nullptr;
  }
}
#endif

static void serdata_rmw_to_ser(const struct ddsi_serdata * dcmn, size_t off, size_t sz, void * buf)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  serdata_rmw_serialize_into_on_demand(const_cast<serdata_rmw *>(d));
  memcpy(buf, byte_offset(d->data(), off), sz);
}

static struct ddsi_serdata * serdata_rmw_to_ser_ref(
  const struct ddsi_serdata * dcmn, size_t off,
  size_t sz, ddsrt_iovec_t * ref)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  serdata_rmw_serialize_into_on_demand(const_cast<serdata_rmw *>(d));
  ref->iov_base = byte_offset(d->data(), off);
  ref->iov_len = (ddsrt_iov_len_t) sz;
  return ddsi_serdata_ref(d);
}

static void serdata_rmw_to_ser_unref(struct ddsi_serdata * dcmn, const ddsrt_iovec_t * ref)
{
  static_cast<void>(ref);    // unused
  ddsi_serdata_unref(static_cast<serdata_rmw *>(dcmn));
}

static bool serdata_rmw_to_sample_impl(
  const sertype_rmw * type, const serdata_rmw * d,
  void * sample)
{
  const auto cdrmode = (d->kind ==
    SDK_DATA) ? rmw_cyclonedds_cpp::SampleOrKey::Sample : rmw_cyclonedds_cpp::SampleOrKey::Key;
  try {
    if (d->type != nullptr) {
      serdata_rmw_serialize_into_on_demand(const_cast<serdata_rmw *>(d));
      type->cdr_reader->deserialize(sample, d->data(), d->size(), cdrmode);
    } else {
      assert(d->kind == SDK_KEY);
      type->cdr_reader->deserialize(sample, d->key(), d->keysize(), cdrmode);
    }
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
    return false;
  }
  return true;
}

static bool serdata_rmw_to_sample(
  const struct ddsi_serdata * dcmn, void * sample, void ** bufptr,
  void * buflim)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  auto tp = static_cast<const sertype_rmw *>(d->type);
  static_cast<void>(bufptr);
  static_cast<void>(buflim);
  return serdata_rmw_to_sample_impl(tp, d, sample);
}

static struct ddsi_serdata * serdata_rmw_to_untyped(const struct ddsi_serdata * dcmn)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  auto d1 = new serdata_rmw(d->type, SDK_KEY);
  // FIXME: make it so d1->type may be a nullptr
  d1->set_key(d->keysize(), d->key());
  d1->type = nullptr;
  return d1;
}

static bool serdata_rmw_untyped_to_sample(
  const struct ddsi_sertype * type,
  const struct ddsi_serdata * dcmn, void * sample,
  void ** bufptr, void * buflim)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  auto tp = static_cast<const sertype_rmw *>(type);
  static_cast<void>(bufptr);
  static_cast<void>(buflim);
  return serdata_rmw_to_sample_impl(tp, d, sample);
}

static void snprintf_wrapper(char ** buf, size_t * bufsize, const char * fmt, ...)
{
  if (*bufsize > 0) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*buf, *bufsize, fmt, ap);
    va_end(ap);
    if (n > 0) {
      if (static_cast<size_t>(n) > *bufsize) {
        n = static_cast<int>(*bufsize);
      }
      *buf += n;
      *bufsize -= static_cast<size_t>(n);
    }
  }
}

static size_t serdata_rmw_print(
  const struct ddsi_sertype * tpcmn, const struct ddsi_serdata * dcmn, char * buf, size_t bufsize)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  auto type = static_cast<const struct sertype_rmw *>(tpcmn);
  char * const init_buf = buf;
  snprintf_wrapper(&buf, &bufsize, "[");
  for (size_t i = 0; i < d->keysize() && bufsize > 0; i++) {
    snprintf_wrapper(&buf, &bufsize, "%02x", static_cast<unsigned char *>(d->key())[i]);
  }
  snprintf_wrapper(&buf, &bufsize, "] ");
  const size_t init_len = static_cast<size_t>(buf - init_buf);
  if (bufsize > 0) {
    using rmw_cyclonedds_cpp::SampleOrKey;
    auto what = (d->kind == SDK_DATA) ? SampleOrKey::Sample : SampleOrKey::Key;
    if (d->type != nullptr) {
      return init_len + type->cdr_reader->print(
        buf, bufsize, d->data(), d->size(),
        what);
    } else {
      return init_len + type->cdr_reader->print(
        buf, bufsize, d->key(), d->keysize(),
        what);
    }
  } else {
    return init_len;
  }
}

static void serdata_rmw_get_keyhash(
  const struct ddsi_serdata * dcmn, struct ddsi_keyhash * buf,
  bool force_md5)
{
  auto d = static_cast<const serdata_rmw *>(dcmn);
  auto type = static_cast<const sertype_rmw *>(d->type);
  std::memset(buf, 0, sizeof(*buf));
  if (type_contains_keys(d->type)) {
    std::vector<byte> key_be;
    type->cdr_reader->extractkey_be(
      key_be, d->key(), d->keysize(),
      rmw_cyclonedds_cpp::SampleOrKey::Key);
    assert(key_be.size() > 4);  // 4 bytes header and at least 1 byte key
    if (type->cdr_writer->get_max_serialized_size(rmw_cyclonedds_cpp::SampleOrKey::Key) <= 20 &&
      !force_md5)
    {
      std::memcpy(buf, key_be.data() + 4, key_be.size() - 4);
    } else {
      ddsrt_md5_state_t md5st;
      ddsrt_md5_init(&md5st);
      ddsrt_md5_append(
        &md5st, reinterpret_cast<const ddsrt_md5_byte_t *>(key_be.data() + 4),
        key_be.size() - 4);
      ddsrt_md5_finish(&md5st, buf->value);
    }
  }
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
  auto tp = static_cast<struct sertype_rmw *>(tpcmn);
  ddsi_sertype_fini(tpcmn);
#if DDS_HAS_TYPELIB
  ddsrt_free(const_cast<void *>(static_cast<const void *>(tp->type_information.data)));
  ddsrt_free(const_cast<void *>(static_cast<const void *>(tp->type_mapping.data)));
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
  throw std::logic_error("not implemented");
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
  auto a = static_cast<const struct sertype_rmw *>(acmn);
  auto b = static_cast<const struct sertype_rmw *>(bcmn);
  if (a->is_request_header != b->is_request_header) {
    return false;
  }
  if (a->message_type->type_generator() != b->message_type->type_generator()) {
    return false;
  }
  return true;
}

uint32_t sertype_rmw_hash(const struct ddsi_sertype * tpcmn)
{
  auto tp = static_cast<const struct sertype_rmw *>(tpcmn);
  uint32_t h2 = static_cast<uint32_t>(std::hash<bool>{}(tp->is_request_header));
  // FIXME: there's got to be an easier way
  auto gen =
    static_cast<std::underlying_type<decltype(tp->message_type->type_generator())>::type>(tp->
    message_type->type_generator());
  uint32_t h1 = static_cast<uint32_t>(std::hash<decltype(gen)>{}(gen));
  return h1 ^ h2;
}

static size_t sertype_get_serialized_size_impl(const struct ddsi_sertype * d, const void * sample)
{
  auto type = static_cast<const struct sertype_rmw *>(d);
  size_t serialized_size = 0;
  try {
    // ROS 2 doesn't really support keys, so only data is handled
    serialized_size = type->cdr_writer->get_serialized_size(
      sample,
      rmw_cyclonedds_cpp::SampleOrKey::Sample);
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
  }

  return serialized_size;
}

static bool sertype_serialize_into_impl(
  const struct ddsi_sertype * d,
  const void * sample,
  void * dst_buffer)
{
  auto type = static_cast<const struct sertype_rmw *>(d);
  try {
    type->cdr_writer->serialize(dst_buffer, sample, rmw_cyclonedds_cpp::SampleOrKey::Sample);
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG(e.what());
  }
  return true;
}

#if CDDS_VERSION == CDDS_VERSION_0_10
size_t sertype_get_serialized_size(const struct ddsi_sertype * d, const void * sample)
{
  return sertype_get_serialized_size_impl(d, sample);
}

bool sertype_serialize_into(
  const struct ddsi_sertype * d,
  const void * sample,
  void * dst_buffer,
  size_t dst_size)
{
  static_cast<void>(dst_size);
  return sertype_serialize_into_impl(d, sample, dst_buffer);
}
#else
dds_return_t sertype_get_serialized_size(
  const struct ddsi_sertype * d,
  enum ddsi_serdata_kind sdkind,
  const void * sample,
  size_t * size,
  uint16_t * enc_identifier)
{
  static_cast<void>(sdkind);
  size_t serialized_size = sertype_get_serialized_size_impl(d, sample);
  // encoding identifier is always plain CDR for this serializer
  *enc_identifier = (native_endian() == endian::little) ? DDSI_RTPS_CDR_LE : DDSI_RTPS_CDR_BE;
  // Cyclone's including or excluding the CDR encoding header in the various situations is
  // painfully inconsistent ...
  assert(serialized_size >= 4);
  *size = serialized_size - 4;
  return DDS_RETCODE_OK;
}

bool sertype_serialize_into(
  const struct ddsi_sertype * d,
  enum ddsi_serdata_kind sdkind,
  const void * sample,
  void * dst_buffer,
  size_t dst_size)
{
  static_cast<void>(sdkind);
  static_cast<void>(dst_size);
  return sertype_serialize_into_impl(d, sample, dst_buffer);
}
#endif

#if DDS_HAS_TYPELIB
static ddsi_typeid_t * sertype_rmw_typeid(const struct ddsi_sertype * d, ddsi_typeid_kind_t kind)
{
  assert(d);
  auto tp = static_cast<const struct sertype_rmw *>(d);
  ddsi_typeinfo_t * type_info = ddsi_typeinfo_deser(
    tp->type_information.data, tp->type_information.sz);
  if (type_info == nullptr) {
    return nullptr;
  }
  ddsi_typeid_t * type_id = ddsi_typeinfo_typeid(type_info, kind);

  dds_free_typeinfo(type_info);

  return type_id;
}

static ddsi_typemap_t * sertype_rmw_typemap(const struct ddsi_sertype * d)
{
  assert(d);
  auto tp = static_cast<const struct sertype_rmw *>(d);
  return ddsi_typemap_deser(tp->type_mapping.data, tp->type_mapping.sz);
}

static ddsi_typeinfo_t * sertype_rmw_typeinfo(const struct ddsi_sertype * d)
{
  assert(d);
  auto tp = static_cast<const struct sertype_rmw *>(d);
  return ddsi_typeinfo_deser(tp->type_information.data, tp->type_information.sz);
}

static struct ddsi_sertype * sertype_rmw_derive_sertype(
  const struct ddsi_sertype * base_sertype,
  dds_data_representation_id_t data_representation,
  dds_type_consistency_enforcement_qospolicy_t tce_qos)
{
  static_cast<void>(base_sertype);
  static_cast<void>(data_representation);
  static_cast<void>(tce_qos);
  return nullptr;
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

struct sertype_rmw * create_sertype(
  const std::string type_name,
  bool is_request_header,
  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> message_type)
{
  struct sertype_rmw * st = new struct sertype_rmw;
  const uint32_t sample_size = message_type->sizeof_type();
  const bool is_self_contained = message_type->is_self_contained();
  const bool is_keyed_type = message_type->has_keys();
#if CDDS_VERSION > CDDS_VERSION_0_10
  const uint32_t flags = 0;
  dds_data_type_properties_t props = 0;
  if (is_self_contained) {
    props |= DDS_DATA_TYPE_IS_MEMCPY_SAFE;
  }
  if (is_keyed_type) {
    props |= DDS_DATA_TYPE_CONTAINS_KEY;
  }
  ddsi_sertype_init_props(
    static_cast<struct ddsi_sertype *>(st),
    type_name.c_str(), &sertype_rmw_ops, &serdata_rmw_ops,
    sample_size, props, DDS_DATA_REPRESENTATION_FLAG_XCDR1, flags);
#else  // CDDS_VERSION > CDDS_VERSION_0_10
  uint32_t flags = 0;
  if (!is_keyed_type) {
    flags |= DDSI_SERTYPE_FLAG_TOPICKIND_NO_KEY;
  }
  if (is_self_contained) {
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
#endif  // CDDS_VERSION > CDDS_VERSION_0_10
  st->is_request_header = is_request_header;
  st->message_type = std::move(message_type);
  const auto variant =
    is_request_header ? rmw_cyclonedds_cpp::SampleOrRequest::Request :
    rmw_cyclonedds_cpp::SampleOrRequest::Sample;
  st->cdr_writer = rmw_cyclonedds_cpp::make_cdr_writer(st->message_type.get(), variant);
  st->cdr_reader = rmw_cyclonedds_cpp::make_cdr_reader(st->message_type.get(), variant);

  return st;
}
