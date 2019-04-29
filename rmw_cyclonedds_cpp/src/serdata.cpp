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
#include <vector>

#include "rmw/error_handling.h"

#include "rmw_cyclonedds_cpp/MessageTypeSupport.hpp"
#include "rmw_cyclonedds_cpp/ServiceTypeSupport.hpp"

#include "rmw_cyclonedds_cpp/serdes.hpp"
#include "rmw_cyclonedds_cpp/serdata.hpp"

#include "dds/ddsi/ddsi_iid.h"
#include "dds/ddsi/q_radmin.h"

using MessageTypeSupport_c = rmw_cyclonedds_cpp::MessageTypeSupport<rosidl_typesupport_introspection_c__MessageMembers>;
using MessageTypeSupport_cpp = rmw_cyclonedds_cpp::MessageTypeSupport<rosidl_typesupport_introspection_cpp::MessageMembers>;
using RequestTypeSupport_c = rmw_cyclonedds_cpp::RequestTypeSupport<rosidl_typesupport_introspection_c__ServiceMembers, rosidl_typesupport_introspection_c__MessageMembers>;
using RequestTypeSupport_cpp = rmw_cyclonedds_cpp::RequestTypeSupport<rosidl_typesupport_introspection_cpp::ServiceMembers, rosidl_typesupport_introspection_cpp::MessageMembers>;
using ResponseTypeSupport_c = rmw_cyclonedds_cpp::ResponseTypeSupport<rosidl_typesupport_introspection_c__ServiceMembers, rosidl_typesupport_introspection_c__MessageMembers>;
using ResponseTypeSupport_cpp = rmw_cyclonedds_cpp::ResponseTypeSupport<rosidl_typesupport_introspection_cpp::ServiceMembers, rosidl_typesupport_introspection_cpp::MessageMembers>;

static bool using_introspection_c_typesupport (const char *typesupport_identifier)
{
    return typesupport_identifier == rosidl_typesupport_introspection_c__identifier;
}

static bool using_introspection_cpp_typesupport (const char *typesupport_identifier)
{
    return typesupport_identifier == rosidl_typesupport_introspection_cpp::typesupport_identifier;
}

void *create_message_type_support (const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport (typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__MessageMembers *> (untyped_members);
        return new MessageTypeSupport_c (members);
    } else if (using_introspection_cpp_typesupport (typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *> (untyped_members);
        return new MessageTypeSupport_cpp (members);
    }
    RMW_SET_ERROR_MSG ("Unknown typesupport identifier");
    return nullptr;
}

void *create_request_type_support (const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport (typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *> (untyped_members);
        return new RequestTypeSupport_c (members);
    } else if (using_introspection_cpp_typesupport (typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *> (untyped_members);
        return new RequestTypeSupport_cpp (members);
    }
    RMW_SET_ERROR_MSG ("Unknown typesupport identifier");
    return nullptr;
}

void *create_response_type_support (const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport (typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *> (untyped_members);
        return new ResponseTypeSupport_c (members);
    } else if (using_introspection_cpp_typesupport (typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *> (untyped_members);
        return new ResponseTypeSupport_cpp (members);
    }
    RMW_SET_ERROR_MSG ("Unknown typesupport identifier");
    return nullptr;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc99-extensions"

static uint32_t serdata_rmw_size (const struct ddsi_serdata *dcmn)
{
    const struct serdata_rmw *d = static_cast<const struct serdata_rmw *> (dcmn);
    return d->data.size ();
}

static void serdata_rmw_free (struct ddsi_serdata *dcmn)
{
    const struct serdata_rmw *d = static_cast<const struct serdata_rmw *> (dcmn);
    delete d;
}

static struct serdata_rmw *new_serdata_rmw (const struct ddsi_sertopic *topic, enum ddsi_serdata_kind kind)
{
    struct serdata_rmw *sd = new serdata_rmw;
    ddsi_serdata_init (sd, topic, kind);
    return sd;
}

static struct ddsi_serdata *serdata_rmw_from_ser (const struct ddsi_sertopic *topic, enum ddsi_serdata_kind kind, const struct nn_rdata *fragchain, size_t size)
{
    struct serdata_rmw *d = new_serdata_rmw (topic, kind);
    uint32_t off = 0;
    assert (fragchain->min == 0);
    assert (fragchain->maxp1 >= off); /* CDR header must be in first fragment */
    d->data.reserve (size);
    while (fragchain) {
        if (fragchain->maxp1 > off) {
            /* only copy if this fragment adds data */
            const unsigned char *payload = NN_RMSG_PAYLOADOFF (fragchain->rmsg, NN_RDATA_PAYLOAD_OFF (fragchain));
            d->data.insert (d->data.end (), payload + off - fragchain->min, payload + fragchain->maxp1 - fragchain->min);
            off = fragchain->maxp1;
        }
        fragchain = fragchain->nextfrag;
    }
    return d;
}

static struct ddsi_serdata *serdata_rmw_from_keyhash (const struct ddsi_sertopic *topic, const struct nn_keyhash *keyhash __attribute__ ((unused)))
{
    /* there is no key field, so from_keyhash is trivial */
    return new_serdata_rmw (topic, SDK_KEY);
}

static struct ddsi_serdata *serdata_rmw_from_sample (const struct ddsi_sertopic *topiccmn, enum ddsi_serdata_kind kind, const void *sample)
{
    const struct sertopic_rmw *topic = static_cast<const struct sertopic_rmw *> (topiccmn);
    struct serdata_rmw *d = new_serdata_rmw (topic, kind);
    if (kind != SDK_DATA) {
        /* ROS2 doesn't do keys, so SDK_KEY is trivial */
    } else if (!topic->is_request_header) {
        cycser sd (d->data);
        if (using_introspection_c_typesupport (topic->type_support.typesupport_identifier_)) {
            auto typed_typesupport = static_cast<MessageTypeSupport_c *> (topic->type_support.type_support_);
            (void) typed_typesupport->serializeROSmessage (sample, sd, nullptr);
        } else if (using_introspection_cpp_typesupport (topic->type_support.typesupport_identifier_)) {
            auto typed_typesupport = static_cast<MessageTypeSupport_cpp *> (topic->type_support.type_support_);
            (void) typed_typesupport->serializeROSmessage (sample, sd, nullptr);
        }
    } else {
        /* The "prefix" lambda is there to inject the service invocation header data into the CDR
           stream -- I haven't checked how it is done in the official RMW implementations, so it is
           probably incompatible. */
        const cdds_request_wrapper_t *wrap = static_cast<const cdds_request_wrapper_t *> (sample);
        auto prefix = [wrap](cycser& ser) { ser << wrap->header.guid; ser << wrap->header.seq; };
        cycser sd (d->data);
        if (using_introspection_c_typesupport (topic->type_support.typesupport_identifier_)) {
            auto typed_typesupport = static_cast<MessageTypeSupport_c *> (topic->type_support.type_support_);
            (void) typed_typesupport->serializeROSmessage (wrap->data, sd, prefix);
        } else if (using_introspection_cpp_typesupport (topic->type_support.typesupport_identifier_)) {
            auto typed_typesupport = static_cast<MessageTypeSupport_cpp *> (topic->type_support.type_support_);
            (void) typed_typesupport->serializeROSmessage (wrap->data, sd, prefix);
        }
    }
    return d;
}

static struct ddsi_serdata *serdata_rmw_to_topicless (const struct ddsi_serdata *dcmn)
{
    const struct serdata_rmw *d = static_cast<const struct serdata_rmw *> (dcmn);
    struct serdata_rmw *d1 = new_serdata_rmw (d->topic, SDK_KEY);
    d1->topic = nullptr;
    return d1;
}

static void serdata_rmw_to_ser (const struct ddsi_serdata *dcmn, size_t off, size_t sz, void *buf)
{
    const struct serdata_rmw *d = static_cast<const struct serdata_rmw *> (dcmn);
    memcpy (buf, (char *) d->data.data () + off, sz);
}

static struct ddsi_serdata *serdata_rmw_to_ser_ref (const struct ddsi_serdata *dcmn, size_t off, size_t sz, ddsrt_iovec_t *ref)
{
    const struct serdata_rmw *d = static_cast<const struct serdata_rmw *> (dcmn);
    ref->iov_base = (char *) d->data.data () + off;
    ref->iov_len = (ddsrt_iov_len_t) sz;
    return ddsi_serdata_ref (d);
}

static void serdata_rmw_to_ser_unref (struct ddsi_serdata *dcmn, const ddsrt_iovec_t *ref __attribute__ ((unused)))
{
    struct serdata_rmw *d = static_cast<struct serdata_rmw *> (dcmn);
    ddsi_serdata_unref (d);
}

static bool serdata_rmw_to_sample (const struct ddsi_serdata *dcmn, void *sample, void **bufptr __attribute__ ((unused)), void *buflim __attribute__ ((unused)))
{
    const struct serdata_rmw *d = static_cast<const struct serdata_rmw *> (dcmn);
    const struct sertopic_rmw *topic = static_cast<const struct sertopic_rmw *> (d->topic);
    assert (bufptr == NULL);
    assert (buflim == NULL);
    if (d->kind != SDK_DATA) {
        /* ROS2 doesn't do keys in a meaningful way yet */
    } else if (!topic->is_request_header) {
        cycdeser sd (static_cast<const void *> (d->data.data ()), d->data.size ());
        if (using_introspection_c_typesupport (topic->type_support.typesupport_identifier_)) {
            auto typed_typesupport = static_cast<MessageTypeSupport_c *> (topic->type_support.type_support_);
            return typed_typesupport->deserializeROSmessage (sd, sample, nullptr);
        } else if (using_introspection_cpp_typesupport (topic->type_support.typesupport_identifier_)) {
            auto typed_typesupport = static_cast<MessageTypeSupport_cpp *> (topic->type_support.type_support_);
            return typed_typesupport->deserializeROSmessage (sd, sample, nullptr);
        }
    } else {
        /* The "prefix" lambda is there to inject the service invocation header data into the CDR
           stream -- I haven't checked how it is done in the official RMW implementations, so it is
           probably incompatible. */
        cdds_request_wrapper_t * const wrap = static_cast<cdds_request_wrapper_t *> (sample);
        auto prefix = [wrap](cycdeser& ser) { ser >> wrap->header.guid; ser >> wrap->header.seq; };
        cycdeser sd (static_cast<const void *> (d->data.data ()), d->data.size ());
        if (using_introspection_c_typesupport (topic->type_support.typesupport_identifier_)) {
            auto typed_typesupport = static_cast<MessageTypeSupport_c *> (topic->type_support.type_support_);
            return typed_typesupport->deserializeROSmessage (sd, wrap->data, prefix);
        } else if (using_introspection_cpp_typesupport (topic->type_support.typesupport_identifier_)) {
            auto typed_typesupport = static_cast<MessageTypeSupport_cpp *> (topic->type_support.type_support_);
            return typed_typesupport->deserializeROSmessage (sd, wrap->data, prefix);
        }
    }
    return false;
}

static bool serdata_rmw_topicless_to_sample (const struct ddsi_sertopic *topic __attribute__ ((unused)), const struct ddsi_serdata *dcmn __attribute__ ((unused)), void *sample __attribute__ ((unused)), void **bufptr __attribute__ ((unused)), void *buflim __attribute__ ((unused)))
{
    /* ROS2 doesn't do keys in a meaningful way yet */
    return true;
}

static bool serdata_rmw_eqkey (const struct ddsi_serdata *a __attribute__ ((unused)), const struct ddsi_serdata *b __attribute__ ((unused)))
{
    /* ROS2 doesn't do keys in a meaningful way yet */
    return true;
}

static const struct ddsi_serdata_ops serdata_rmw_ops = {
    .eqkey = serdata_rmw_eqkey,
    .get_size = serdata_rmw_size,
    .from_ser = serdata_rmw_from_ser,
    .from_keyhash = serdata_rmw_from_keyhash,
    .from_sample = serdata_rmw_from_sample,
    .to_ser = serdata_rmw_to_ser,
    .to_ser_ref = serdata_rmw_to_ser_ref,
    .to_ser_unref = serdata_rmw_to_ser_unref,
    .to_sample = serdata_rmw_to_sample,
    .to_topicless = serdata_rmw_to_topicless,
    .topicless_to_sample = serdata_rmw_topicless_to_sample,
    .free = serdata_rmw_free
};

static void sertopic_rmw_deinit (struct ddsi_sertopic *tpcmn)
{
    struct sertopic_rmw *tp = static_cast<struct sertopic_rmw *> (tpcmn);
    delete tp;
}

static void sertopic_rmw_zero_samples (const struct ddsi_sertopic *d __attribute__ ((unused)), void *samples __attribute__ ((unused)), size_t count __attribute__ ((unused)))
{
    /* Not using code paths that rely on the samples getting zero'd out */
}

static void sertopic_rmw_realloc_samples (void **ptrs __attribute__ ((unused)), const struct ddsi_sertopic *d __attribute__ ((unused)), void *old __attribute__ ((unused)), size_t oldcount __attribute__ ((unused)), size_t count __attribute__ ((unused)))
{
    /* Not using code paths that rely on this (loans, dispose, unregister with instance handle,
       content filters) */
    abort ();
}

static void sertopic_rmw_free_samples (const struct ddsi_sertopic *d __attribute__ ((unused)), void **ptrs __attribute__ ((unused)), size_t count __attribute__ ((unused)), dds_free_op_t op)
{
    /* Not using code paths that rely on this (dispose, unregister with instance handle, content
       filters) */
    assert (!(op & DDS_FREE_ALL_BIT));
}

static const struct ddsi_sertopic_ops sertopic_rmw_ops = {
    .deinit = sertopic_rmw_deinit,
    .zero_samples = sertopic_rmw_zero_samples,
    .realloc_samples = sertopic_rmw_realloc_samples,
    .free_samples = sertopic_rmw_free_samples
};

struct sertopic_rmw *create_sertopic (const char *topicname, const char *type_support_identifier, void *type_support, bool is_request_header)
{
    struct sertopic_rmw *st = new struct sertopic_rmw;
    st->cpp_name = std::string (topicname);
    st->cpp_type_name = std::string ("absent"); // FIXME: obviously a hack
    st->cpp_name_type_name = st->cpp_name + std::string (";") + std::string (st->cpp_type_name);
    st->ops = &sertopic_rmw_ops;
    st->serdata_ops = &serdata_rmw_ops;
    st->serdata_basehash = ddsi_sertopic_compute_serdata_basehash (st->serdata_ops);
    st->name_type_name = const_cast<char *> (st->cpp_name_type_name.c_str ());
    st->name = const_cast<char *> (st->cpp_name.c_str ());
    st->type_name = const_cast<char *> (st->cpp_type_name.c_str ());
    st->iid = ddsi_iid_gen ();
    ddsrt_atomic_st32 (&st->refc, 1);

    st->type_support.typesupport_identifier_ = type_support_identifier;
    st->type_support.type_support_ = type_support;
    st->is_request_header = is_request_header;
    return st;
}

#pragma GCC diagnostic pop
