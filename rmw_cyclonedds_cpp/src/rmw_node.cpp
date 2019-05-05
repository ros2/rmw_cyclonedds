// Copyright 2018 ADLINK Technology Limited.
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

/* TODO:

   - topic names (should use the same as other RMW layers)

   - type names (make a copy of the generic type descriptor, modify the name and pass that)

   - need to handle endianness differences in deserialization

   - topic creation: shouldn't leak topics

   - introspection stuff not done yet

   - check / make sure a node remains valid while one of its subscriptions exists

   - service/client simply use the instance handle of its publisher as its GUID -- yikes! but it is
     actually only kinda wrong because the instance handles allocated by different instance of cdds
     are actually taken from uncorrelated (close to uncorrelated anyway) permutations of 64-bit
     unsigned integers

   - ... and probably many more things ...
*/

#include <mutex>
#include <unordered_set>
#include <algorithm>
#include <atomic>

#include "rcutils/logging_macros.h"

#include "rmw/allocators.h"
#include "rmw/convert_rcutils_ret_to_rmw_ret.h"
#include "rmw/error_handling.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h"

#include "rmw/impl/cpp/macros.hpp"

#include "rmw_cyclonedds_cpp/MessageTypeSupport.hpp"
#include "rmw_cyclonedds_cpp/ServiceTypeSupport.hpp"

#include "namespace_prefix.hpp"

#include "dds/dds.h"
#include "dds/ddsi/ddsi_sertopic.h"
#include "rmw_cyclonedds_cpp/serdes.hpp"
#include "rmw_cyclonedds_cpp/serdata.hpp"

#define RET_ERR_X(msg, code) do { RMW_SET_ERROR_MSG (msg); code; } while (0)
#define RET_NULL_X(var, code) do { if (!var) RET_ERR_X (#var " is null", code); } while (0)
#define RET_ALLOC_X(var, code) do { if (!var) RET_ERR_X ("failed to allocate " #var, code); } while (0)
#define RET_WRONG_IMPLID_X(var, code) do {                              \
        RET_NULL_X (var, code);                                         \
        if ((var)->implementation_identifier != eclipse_cyclonedds_identifier) { \
            RET_ERR_X (#var " not from this implementation", code);     \
        }                                                               \
    } while (0)
#define RET_NULL_OR_EMPTYSTR_X(var, code) do {                  \
        if (!var || strlen (var) == 0) {                        \
            RET_ERR_X (#var " is null or empty string", code);  \
        }                                                       \
    } while (0)
#define RET_ERR(msg) RET_ERR_X (msg, return RMW_RET_ERROR)
#define RET_NULL(var) RET_NULL_X (var, return RMW_RET_ERROR)
#define RET_ALLOC(var) RET_ALLOC_X (var, return RMW_RET_ERROR)
#define RET_WRONG_IMPLID(var) RET_WRONG_IMPLID_X (var, return RMW_RET_ERROR)
#define RET_NULL_OR_EMPTYSTR(var) RET_NULL_OR_EMPTYSTR_X (var, return RMW_RET_ERROR)

const char *const eclipse_cyclonedds_identifier = "rmw_cyclonedds_cpp";
const char * const eclipse_cyclonedds_serialization_format = "cdr";

/* instance handles are unsigned 64-bit integers carefully constructed to be as close to uniformly
   distributed as possible for no other reason than making them near-perfect hash keys, hence we can
   improve over the default hash function */
struct dds_instance_handle_hash {
public:
    std::size_t operator () (dds_instance_handle_t const& x) const noexcept {
        return static_cast<std::size_t> (x);
    }
};

struct CddsNode {
    rmw_guard_condition_t *graph_guard_condition;
};

struct CddsPublisher {
    dds_entity_t pubh;
    dds_instance_handle_t pubiid;
};

struct CddsSubscription {
    dds_entity_t subh;
    dds_entity_t rdcondh;
};

struct CddsCS {
    CddsPublisher *pub;
    CddsSubscription *sub;
};

struct CddsClient {
    CddsCS client;
};

struct CddsService {
    CddsCS service;
};

struct CddsGuardCondition {
    dds_entity_t gcondh;
};

struct CddsWaitset {
    dds_entity_t waitseth;

    std::vector<dds_attach_t> trigs;

    std::mutex lock;
    bool inuse;
    std::vector<CddsSubscription *> subs;
    std::vector<CddsGuardCondition *> gcs;
    std::vector<CddsClient *> cls;
    std::vector<CddsService *> srvs;
};

struct Cdds {
    std::mutex lock;
    uint32_t refcount;
    dds_entity_t ppant;
    std::unordered_set<CddsWaitset *> waitsets;
    Cdds () : refcount (0), ppant (0) {}
};

static Cdds gcdds;

static void clean_waitset_caches ();

#pragma GCC visibility push (default)

extern "C" const char *rmw_get_implementation_identifier ()
{
    return eclipse_cyclonedds_identifier;
}

extern "C" const char *rmw_get_serialization_format()
{
    return eclipse_cyclonedds_serialization_format;
}

extern "C" rmw_ret_t rmw_init_options_init (rmw_init_options_t *init_options, rcutils_allocator_t allocator)
{
    RMW_CHECK_ARGUMENT_FOR_NULL (init_options, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ALLOCATOR (&allocator, return RMW_RET_INVALID_ARGUMENT);
    if (NULL != init_options->implementation_identifier) {
        RMW_SET_ERROR_MSG ("expected zero-initialized init_options");
        return RMW_RET_INVALID_ARGUMENT;
    }
    init_options->instance_id = 0;
    init_options->implementation_identifier = eclipse_cyclonedds_identifier;
    init_options->allocator = allocator;
    init_options->impl = nullptr;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init_options_copy (const rmw_init_options_t *src, rmw_init_options_t *dst)
{
    RMW_CHECK_ARGUMENT_FOR_NULL (src, RMW_RET_INVALID_ARGUMENT);
    RMW_CHECK_ARGUMENT_FOR_NULL (dst, RMW_RET_INVALID_ARGUMENT);
    RMW_CHECK_TYPE_IDENTIFIERS_MATCH (
            src,
            src->implementation_identifier,
            eclipse_cyclonedds_identifier,
            return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
    if (NULL != dst->implementation_identifier) {
        RMW_SET_ERROR_MSG ("expected zero-initialized dst");
        return RMW_RET_INVALID_ARGUMENT;
    }
    *dst = *src;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init_options_fini (rmw_init_options_t *init_options)
{
    RMW_CHECK_ARGUMENT_FOR_NULL (init_options, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ALLOCATOR (&init_options->allocator, return RMW_RET_INVALID_ARGUMENT);
    RMW_CHECK_TYPE_IDENTIFIERS_MATCH (
            init_options,
            init_options->implementation_identifier,
            eclipse_cyclonedds_identifier,
            return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
    *init_options = rmw_get_zero_initialized_init_options ();
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init (const rmw_init_options_t *options __attribute__ ((unused)), rmw_context_t *context __attribute__ ((unused)))
{
    RCUTILS_CHECK_ARGUMENT_FOR_NULL (options, RMW_RET_INVALID_ARGUMENT);
    RCUTILS_CHECK_ARGUMENT_FOR_NULL (context, RMW_RET_INVALID_ARGUMENT);
    RMW_CHECK_TYPE_IDENTIFIERS_MATCH (
            options,
            options->implementation_identifier,
            eclipse_cyclonedds_identifier,
            return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
    context->instance_id = options->instance_id;
    context->implementation_identifier = eclipse_cyclonedds_identifier;
    context->impl = nullptr;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_shutdown (rmw_context_t *context)
{
    RCUTILS_CHECK_ARGUMENT_FOR_NULL (context, RMW_RET_INVALID_ARGUMENT);
    RMW_CHECK_TYPE_IDENTIFIERS_MATCH (
            context,
            context->implementation_identifier,
            eclipse_cyclonedds_identifier,
            return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
    // context impl is explicitly supposed to be nullptr for now, see rmw_init's code
    // RCUTILS_CHECK_ARGUMENT_FOR_NULL (context->impl, RMW_RET_INVALID_ARGUMENT);
    *context = rmw_get_zero_initialized_context ();
    return RMW_RET_OK;
}

static dds_entity_t ref_ppant ()
{
    std::lock_guard<std::mutex> lock (gcdds.lock);
    if (gcdds.refcount == 0) {
        if ((gcdds.ppant = dds_create_participant (DDS_DOMAIN_DEFAULT, nullptr, nullptr)) < 0) {
            RMW_SET_ERROR_MSG ("failed to create participant");
            return gcdds.ppant;
        }
    }
    gcdds.refcount++;
    return gcdds.ppant;
}

static void unref_ppant ()
{
    std::lock_guard<std::mutex> lock (gcdds.lock);
    if (--gcdds.refcount == 0) {
        dds_delete (gcdds.ppant);
        gcdds.ppant = 0;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    NODES                                                          ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_node_t *rmw_create_node (rmw_context_t *context __attribute__ ((unused)), const char *name, const char *namespace_, size_t domain_id, const rmw_node_security_options_t *security_options)
{
    RET_NULL_X (name, return nullptr);
    RET_NULL_X (namespace_, return nullptr);
    (void) domain_id;
    (void) security_options;

    dds_entity_t pp = ref_ppant ();
    if (pp < 0) {
        return nullptr;
    }
    auto *node_impl = new CddsNode ();
    rmw_node_t *node_handle = nullptr;
    RET_ALLOC_X (node_impl, goto fail_node_impl);
    rmw_guard_condition_t *graph_guard_condition;
    if (!(graph_guard_condition = rmw_create_guard_condition (context))) {
        goto fail_ggc;
    }
    node_impl->graph_guard_condition = graph_guard_condition;

    node_handle = rmw_node_allocate ();
    RET_ALLOC_X (node_handle, goto fail_node_handle);
    node_handle->implementation_identifier = eclipse_cyclonedds_identifier;
    node_handle->data = node_impl;

    node_handle->name = static_cast<const char *> (rmw_allocate (sizeof (char) * strlen (name) + 1));
    RET_ALLOC_X (node_handle->name, goto fail_node_handle_name);
    memcpy (const_cast<char *> (node_handle->name), name, strlen (name) + 1);

    node_handle->namespace_ = static_cast<const char *> (rmw_allocate (sizeof (char) * strlen (namespace_) + 1));
    RET_ALLOC_X (node_handle->namespace_, goto fail_node_handle_namespace);
    memcpy (const_cast<char *> (node_handle->namespace_), namespace_, strlen (namespace_) + 1);
    return node_handle;

#if 0
 fail_add_node:
    rmw_free (const_cast<char *> (node_handle->namespace_));
#endif
 fail_node_handle_namespace:
    rmw_free (const_cast<char *> (node_handle->name));
 fail_node_handle_name:
    rmw_node_free (node_handle);
 fail_node_handle:
    if (RMW_RET_OK != rmw_destroy_guard_condition (graph_guard_condition)) {
        RCUTILS_LOG_ERROR_NAMED ("rmw_cyclonedds_cpp", "failed to destroy guard condition during error handling");
    }
 fail_ggc:
    delete node_impl;
 fail_node_impl:
    unref_ppant ();
    return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_node (rmw_node_t *node)
{
    rmw_ret_t result_ret = RMW_RET_OK;
    RET_WRONG_IMPLID (node);
    auto node_impl = static_cast<CddsNode *> (node->data);
    RET_NULL (node_impl);
    rmw_free (const_cast<char *> (node->name));
    rmw_free (const_cast<char *> (node->namespace_));
    rmw_node_free (node);
    if (RMW_RET_OK != rmw_destroy_guard_condition (node_impl->graph_guard_condition)) {
        RMW_SET_ERROR_MSG ("failed to destroy graph guard condition");
        result_ret = RMW_RET_ERROR;
    }
    delete node_impl;
    unref_ppant ();
    return result_ret;
}

extern "C" const rmw_guard_condition_t *rmw_node_get_graph_guard_condition (const rmw_node_t *node)
{
    RET_WRONG_IMPLID_X (node, return nullptr);
    auto node_impl = static_cast<CddsNode *> (node->data);
    RET_NULL_X (node_impl, return nullptr);
    return node_impl->graph_guard_condition;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    (DE)SERIALIZATION                                              ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_serialize (const void *ros_message, const rosidl_message_type_support_t *type_support, rmw_serialized_message_t *serialized_message)
{
    (void) ros_message; (void) type_support; (void) serialized_message;
    RMW_SET_ERROR_MSG ("rmw_serialize not implemented");
    return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_deserialize (const rmw_serialized_message_t *serialized_message, const rosidl_message_type_support_t *type_support, void *ros_message)
{
    (void) ros_message; (void) type_support; (void) serialized_message;
    RMW_SET_ERROR_MSG ("rmw_deserialize not implemented");
    return RMW_RET_ERROR;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    PUBLICATIONS                                                   ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_publish (const rmw_publisher_t *publisher, const void *ros_message)
{
    RET_WRONG_IMPLID (publisher);
    RET_NULL (ros_message);
    auto pub = static_cast<CddsPublisher *> (publisher->data);
    assert (pub);
    if (dds_write (pub->pubh, ros_message) >= 0) {
        return RMW_RET_OK;
    } else {
        /* FIXME: what is the expected behavior when it times out? */
        RMW_SET_ERROR_MSG ("cannot publish data");
        //return RMW_RET_ERROR;
        return RMW_RET_OK;
    }
}

extern "C" rmw_ret_t rmw_publish_serialized_message (const rmw_publisher_t *publisher, const rmw_serialized_message_t *serialized_message)
{
    (void) publisher; (void) serialized_message;
    RMW_SET_ERROR_MSG ("rmw_publish_serialized_message not implemented");
    return RMW_RET_ERROR;
}

static const rosidl_message_type_support_t *get_typesupport (const rosidl_message_type_support_t *type_supports)
{
    const rosidl_message_type_support_t *ts;
    if ((ts = get_message_typesupport_handle (type_supports, rosidl_typesupport_introspection_c__identifier)) != nullptr) {
        return ts;
    } else if ((ts = get_message_typesupport_handle (type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr) {
        return ts;
    } else {
        RMW_SET_ERROR_MSG ("type support not from this implementation");
        return nullptr;
    }
}

static std::string make_fqtopic (const char *prefix, const char *topic_name, const char *suffix, bool avoid_ros_namespace_conventions)
{
    if (avoid_ros_namespace_conventions) {
        return std::string (topic_name) + "__" + std::string (suffix);
    } else {
        return std::string (prefix) + "/" + make_fqtopic (prefix, topic_name, suffix, true);
    }
}

static std::string make_fqtopic (const char *prefix, const char *topic_name, const char *suffix, const rmw_qos_profile_t *qos_policies)
{
    return make_fqtopic (prefix, topic_name, suffix, qos_policies->avoid_ros_namespace_conventions);
}

static dds_qos_t *create_readwrite_qos (const rmw_qos_profile_t *qos_policies, bool ignore_local_publications)
{
    dds_qos_t *qos = dds_create_qos ();
    switch (qos_policies->history) {
        case RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT:
        case RMW_QOS_POLICY_HISTORY_KEEP_LAST:
            if (qos_policies->depth > INT32_MAX) {
                RMW_SET_ERROR_MSG ("unsupported history depth");
                dds_delete_qos (qos);
                return nullptr;
            }
            dds_qset_history (qos, DDS_HISTORY_KEEP_LAST, static_cast<uint32_t> (qos_policies->depth));
            break;
        case RMW_QOS_POLICY_HISTORY_KEEP_ALL:
            dds_qset_history (qos, DDS_HISTORY_KEEP_ALL, DDS_LENGTH_UNLIMITED);
            break;
    }
    switch (qos_policies->reliability) {
        case RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT:
        case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
            dds_qset_reliability (qos, DDS_RELIABILITY_RELIABLE, DDS_SECS (1));
            break;
        case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
            dds_qset_reliability (qos, DDS_RELIABILITY_BEST_EFFORT, 0);
            break;
    }
    switch (qos_policies->durability) {
        case RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT:
        case RMW_QOS_POLICY_DURABILITY_VOLATILE:
            dds_qset_durability (qos, DDS_DURABILITY_VOLATILE);
            break;
        case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL:
            dds_qset_durability (qos, DDS_DURABILITY_TRANSIENT_LOCAL);
            break;
    }
    if (ignore_local_publications) {
        dds_qset_ignorelocal (qos, DDS_IGNORELOCAL_PARTICIPANT);
    }
    return qos;
}

static CddsPublisher *create_cdds_publisher (const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies)
{
    RET_WRONG_IMPLID_X (node, return nullptr);
    RET_NULL_OR_EMPTYSTR_X (topic_name, return nullptr);
    RET_NULL_X (qos_policies, return nullptr);
    auto node_impl = static_cast<CddsNode *> (node->data);
    RET_NULL_X (node_impl, return nullptr);
    const rosidl_message_type_support_t *type_support = get_typesupport (type_supports);
    RET_NULL_X (type_support, return nullptr);
    CddsPublisher *pub = new CddsPublisher ();
    dds_entity_t topic;
    dds_qos_t *qos;

    std::string fqtopic_name = make_fqtopic (ros_topic_prefix, topic_name, "", qos_policies);

    auto sertopic = create_sertopic (fqtopic_name.c_str (), type_support->typesupport_identifier, create_message_type_support (type_support->data, type_support->typesupport_identifier), false);
    if ((topic = dds_create_topic_arbitrary (gcdds.ppant, sertopic, nullptr, nullptr, nullptr)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create topic");
        goto fail_topic;
    }
    if ((qos = create_readwrite_qos (qos_policies, false)) == nullptr) {
        goto fail_qos;
    }
    if ((pub->pubh = dds_create_writer (gcdds.ppant, topic, qos, nullptr)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create writer");
        goto fail_writer;
    }
    if (dds_get_instance_handle (pub->pubh, &pub->pubiid) < 0) {
        RMW_SET_ERROR_MSG ("failed to get instance handle for writer");
        goto fail_instance_handle;
    }
    dds_delete_qos (qos);
    /* FIXME: leak the topic for now */
    return pub;

 fail_instance_handle:
    if (dds_delete (pub->pubh) < 0) {
        RCUTILS_LOG_ERROR_NAMED ("rmw_cyclonedds_cpp", "failed to destroy writer during error handling");
    }
 fail_writer:
    dds_delete_qos (qos);
 fail_qos:
    /* not deleting topic -- have to sort out proper topic handling & that requires fixing a few
       things in cyclone as well */
 fail_topic:
    delete pub;
    return nullptr;
}

extern "C" rmw_publisher_t *rmw_create_publisher (const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies)
{
    CddsPublisher *pub;
    rmw_publisher_t *rmw_publisher;
    if ((pub = create_cdds_publisher (node, type_supports, topic_name, qos_policies)) == nullptr) {
        goto fail_common_init;
    }
    rmw_publisher = rmw_publisher_allocate ();
    RET_ALLOC_X (rmw_publisher, goto fail_publisher);
    rmw_publisher->implementation_identifier = eclipse_cyclonedds_identifier;
    rmw_publisher->data = pub;
    rmw_publisher->topic_name = reinterpret_cast<char *> (rmw_allocate (strlen (topic_name) + 1));
    RET_ALLOC_X (rmw_publisher->topic_name, goto fail_topic_name);
    memcpy (const_cast<char *> (rmw_publisher->topic_name), topic_name, strlen (topic_name) + 1);
    return rmw_publisher;
 fail_topic_name:
    rmw_publisher_free (rmw_publisher);
 fail_publisher:
    if (dds_delete (pub->pubh) < 0) {
        RCUTILS_LOG_ERROR_NAMED ("rmw_cyclonedds_cpp", "failed to delete writer during error handling");
    }
    delete pub;
 fail_common_init:
    return nullptr;
}

extern "C" rmw_ret_t rmw_get_gid_for_publisher (const rmw_publisher_t *publisher, rmw_gid_t *gid)
{
    RET_WRONG_IMPLID (publisher);
    RET_NULL (gid);
    auto pub = static_cast<const CddsPublisher *> (publisher->data);
    RET_NULL (pub);
    gid->implementation_identifier = eclipse_cyclonedds_identifier;
    memset (gid->data, 0, sizeof (gid->data));
    assert (sizeof (pub->pubiid) <= sizeof (gid->data));
    memcpy (gid->data, &pub->pubiid, sizeof (pub->pubiid));
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_compare_gids_equal (const rmw_gid_t *gid1, const rmw_gid_t *gid2, bool *result)
{
    RET_WRONG_IMPLID (gid1);
    RET_WRONG_IMPLID (gid2);
    RET_NULL (result);
    /* alignment is potentially lost because of the translation to an array of bytes, so use
       memcmp instead of a simple integer comparison */
    *result = memcmp (gid1->data, gid2->data, sizeof (gid1->data)) == 0;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_publisher_count_matched_subscriptions (const rmw_publisher_t *publisher, size_t *subscription_count)
{
    (void) publisher;
    *subscription_count = 0;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_destroy_publisher (rmw_node_t *node, rmw_publisher_t *publisher)
{
    RET_WRONG_IMPLID (node);
    RET_WRONG_IMPLID (publisher);
    auto pub = static_cast<CddsPublisher *> (publisher->data);
    if (pub != nullptr) {
        if (dds_delete (pub->pubh) < 0) {
            RMW_SET_ERROR_MSG ("failed to delete writer");
        }
        delete pub;
    }
    rmw_free (const_cast<char *> (publisher->topic_name));
    publisher->topic_name = nullptr;
    rmw_publisher_free (publisher);
    return RMW_RET_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    SUBSCRIPTIONS                                                  ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

static CddsSubscription *create_cdds_subscription (const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies, bool ignore_local_publications)
{
    RET_WRONG_IMPLID_X (node, return nullptr);
    RET_NULL_OR_EMPTYSTR_X (topic_name, return nullptr);
    RET_NULL_X (qos_policies, return nullptr);
    auto node_impl = static_cast<CddsNode *> (node->data);
    RET_NULL_X (node_impl, return nullptr);
    const rosidl_message_type_support_t *type_support = get_typesupport (type_supports);
    RET_NULL_X (type_support, return nullptr);
    CddsSubscription *sub = new CddsSubscription ();
    dds_entity_t topic;
    dds_qos_t *qos;

    std::string fqtopic_name = make_fqtopic (ros_topic_prefix, topic_name, "", qos_policies);

    auto sertopic = create_sertopic (fqtopic_name.c_str (), type_support->typesupport_identifier, create_message_type_support (type_support->data, type_support->typesupport_identifier), false);
    if ((topic = dds_create_topic_arbitrary (gcdds.ppant, sertopic, nullptr, nullptr, nullptr)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create topic");
        goto fail_topic;
    }
    if ((qos = create_readwrite_qos (qos_policies, ignore_local_publications)) == nullptr) {
        goto fail_qos;
    }
    if ((sub->subh = dds_create_reader (gcdds.ppant, topic, qos, nullptr)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create reader");
        goto fail_reader;
    }
    if ((sub->rdcondh = dds_create_readcondition (sub->subh, DDS_ANY_STATE)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create readcondition");
        goto fail_readcond;
    }
    dds_delete_qos (qos);
    return sub;
 fail_readcond:
    if (dds_delete (sub->subh) < 0) {
        RCUTILS_LOG_ERROR_NAMED ("rmw_cyclonedds_cpp", "failed to delete reader during error handling");
    }
 fail_reader:
    dds_delete_qos (qos);
 fail_qos:
    /* FIXME: leak topic */
 fail_topic:
    delete sub;
    return nullptr;
}

extern "C" rmw_subscription_t *rmw_create_subscription (const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies, bool ignore_local_publications)
{
    CddsSubscription *sub;
    rmw_subscription_t *rmw_subscription;
    if ((sub = create_cdds_subscription (node, type_supports, topic_name, qos_policies, ignore_local_publications)) == nullptr) {
        goto fail_common_init;
    }
    rmw_subscription = rmw_subscription_allocate ();
    RET_ALLOC_X (rmw_subscription, goto fail_subscription);
    rmw_subscription->implementation_identifier = eclipse_cyclonedds_identifier;
    rmw_subscription->data = sub;
    rmw_subscription->topic_name = reinterpret_cast<const char *> (rmw_allocate (strlen (topic_name) + 1));
    RET_ALLOC_X (rmw_subscription->topic_name, goto fail_topic_name);
    memcpy (const_cast<char *> (rmw_subscription->topic_name), topic_name, strlen (topic_name) + 1);
    return rmw_subscription;
 fail_topic_name:
    rmw_subscription_free (rmw_subscription);
 fail_subscription:
    if (dds_delete (sub->rdcondh) < 0) {
        RCUTILS_LOG_ERROR_NAMED ("rmw_cyclonedds_cpp", "failed to delete readcondition during error handling");
    }
    if (dds_delete (sub->subh) < 0) {
        RCUTILS_LOG_ERROR_NAMED ("rmw_cyclonedds_cpp", "failed to delete reader during error handling");
    }
    delete sub;
 fail_common_init:
    return nullptr;
}

extern "C" rmw_ret_t rmw_subscription_count_matched_publishers (const rmw_subscription_t *subscription, size_t *publisher_count)
{
    (void) subscription;
    *publisher_count = 0;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_destroy_subscription (rmw_node_t *node, rmw_subscription_t *subscription)
{
    RET_WRONG_IMPLID (node);
    RET_WRONG_IMPLID (subscription);
    auto sub = static_cast<CddsSubscription *> (subscription->data);
    if (sub != nullptr) {
        clean_waitset_caches ();
        if (dds_delete (sub->rdcondh) < 0) {
            RMW_SET_ERROR_MSG ("failed to delete readcondition");
        }
        if (dds_delete (sub->subh) < 0) {
            RMW_SET_ERROR_MSG ("failed to delete reader");
        }
        delete sub;
    }
    rmw_free (const_cast<char *> (subscription->topic_name));
    subscription->topic_name = nullptr;
    rmw_subscription_free (subscription);
    return RMW_RET_OK;
}

static rmw_ret_t rmw_take_int (const rmw_subscription_t *subscription, void *ros_message, bool *taken, rmw_message_info_t *message_info)
{
    RET_NULL (taken);
    RET_NULL (ros_message);
    RET_WRONG_IMPLID (subscription);
    CddsSubscription *sub = static_cast<CddsSubscription *> (subscription->data);
    RET_NULL (sub);
    dds_sample_info_t info;
    while (dds_take (sub->subh, &ros_message, &info, 1, 1) == 1) {
        if (info.valid_data) {
            if (message_info) {
                message_info->publisher_gid.implementation_identifier = eclipse_cyclonedds_identifier;
                memset (message_info->publisher_gid.data, 0, sizeof (message_info->publisher_gid.data));
                assert (sizeof (info.publication_handle) <= sizeof (message_info->publisher_gid.data));
                memcpy (message_info->publisher_gid.data, &info.publication_handle, sizeof (info.publication_handle));
            }
            *taken = true;
            return RMW_RET_OK;
        }
    }
    *taken = false;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_take (const rmw_subscription_t *subscription, void *ros_message, bool *taken)
{
    return rmw_take_int (subscription, ros_message, taken, nullptr);
}

extern "C" rmw_ret_t rmw_take_with_info (const rmw_subscription_t *subscription, void *ros_message, bool *taken, rmw_message_info_t *message_info)
{
    return rmw_take_int (subscription, ros_message, taken, message_info);
}

extern "C" rmw_ret_t rmw_take_serialized_message (const rmw_subscription_t *subscription, rmw_serialized_message_t *serialized_message, bool *taken)
{
    (void) subscription; (void) serialized_message; (void) taken;
    RMW_SET_ERROR_MSG ("rmw_take_serialized_message not implemented");
    return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_take_serialized_message_with_info (const rmw_subscription_t *subscription, rmw_serialized_message_t *serialized_message, bool *taken, rmw_message_info_t *message_info)
{
    (void) subscription; (void) serialized_message; (void) taken; (void) message_info;
    RMW_SET_ERROR_MSG ("rmw_take_serialized_message_with_info not implemented");
    return RMW_RET_ERROR;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    GUARDS AND WAITSETS                                            ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_guard_condition_t *rmw_create_guard_condition (rmw_context_t *context __attribute__ ((unused)))
{
    rmw_guard_condition_t *guard_condition_handle;
    auto *gcond_impl = new CddsGuardCondition ();
    if (ref_ppant () < 0) {
        goto fail_ppant;
    }
    if ((gcond_impl->gcondh = dds_create_guardcondition (gcdds.ppant)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create guardcondition");
        goto fail_guardcond;
    }
    guard_condition_handle = new rmw_guard_condition_t;
    guard_condition_handle->implementation_identifier = eclipse_cyclonedds_identifier;
    guard_condition_handle->data = gcond_impl;
    return guard_condition_handle;

 fail_guardcond:
    unref_ppant ();
 fail_ppant:
    delete (gcond_impl);
    return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_guard_condition (rmw_guard_condition_t *guard_condition_handle)
{
    RET_NULL (guard_condition_handle);
    auto *gcond_impl = static_cast<CddsGuardCondition *> (guard_condition_handle->data);
    clean_waitset_caches ();
    dds_delete (gcond_impl->gcondh);
    unref_ppant ();
    delete gcond_impl;
    delete guard_condition_handle;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_trigger_guard_condition (const rmw_guard_condition_t *guard_condition_handle)
{
    RET_WRONG_IMPLID (guard_condition_handle);
    auto *gcond_impl = static_cast<CddsGuardCondition *> (guard_condition_handle->data);
    dds_set_guardcondition (gcond_impl->gcondh, true);
    return RMW_RET_OK;
}

extern "C" rmw_wait_set_t *rmw_create_wait_set (size_t max_conditions)
{
    (void) max_conditions;
    rmw_wait_set_t *wait_set = rmw_wait_set_allocate ();
    CddsWaitset *ws = nullptr;
    RET_ALLOC_X (wait_set, goto fail_alloc_wait_set);
    wait_set->implementation_identifier = eclipse_cyclonedds_identifier;
    wait_set->data = rmw_allocate (sizeof (CddsWaitset));
    RET_ALLOC_X (wait_set->data, goto fail_alloc_wait_set_data);
    // This should default-construct the fields of CddsWaitset
    ws = static_cast<CddsWaitset *> (wait_set->data);
    RMW_TRY_PLACEMENT_NEW (ws, ws, goto fail_placement_new, CddsWaitset, );
    if (!ws) {
        RMW_SET_ERROR_MSG ("failed to construct wait set info struct");
        goto fail_ws;
    }
    ws->inuse = false;
    if ((ws->waitseth = dds_create_waitset (gcdds.ppant)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create waitset");
        goto fail_waitset;
    }
    {
        std::lock_guard<std::mutex> lock (gcdds.lock);
        gcdds.waitsets.insert (ws);
    }
    return wait_set;

 fail_waitset:
 fail_ws:
    RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE (ws->~CddsWaitset (), ws);
 fail_placement_new:
    rmw_free (wait_set->data);
 fail_alloc_wait_set_data:
    rmw_wait_set_free (wait_set);
 fail_alloc_wait_set:
    return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_wait_set (rmw_wait_set_t *wait_set)
{
    RET_WRONG_IMPLID (wait_set);
    auto result = RMW_RET_OK;
    auto ws = static_cast<CddsWaitset *> (wait_set->data);
    RET_NULL (ws);
    dds_delete (ws->waitseth);
    {
        std::lock_guard<std::mutex> lock (gcdds.lock);
        gcdds.waitsets.erase (ws);
    }
    RMW_TRY_DESTRUCTOR (ws->~CddsWaitset (), ws, result = RMW_RET_ERROR);
    rmw_free (wait_set->data);
    rmw_wait_set_free (wait_set);
    return result;
}

template<typename T>
static bool require_reattach (const std::vector<T *>& cached, size_t count, void **ary)
{
    if (ary == nullptr || count == 0) {
        return (cached.size () != 0);
    } else if (count != cached.size ()) {
        return true;
    } else {
        return (memcmp (static_cast<const void *> (cached.data ()), static_cast<void *> (ary), count * sizeof (void *)) != 0);
    }
}

static void waitset_detach (CddsWaitset *ws)
{
    for (auto&& x : ws->subs) dds_waitset_detach (ws->waitseth, x->rdcondh);
    for (auto&& x : ws->gcs)  dds_waitset_detach (ws->waitseth, x->gcondh);
    for (auto&& x : ws->srvs) dds_waitset_detach (ws->waitseth, x->service.sub->rdcondh);
    for (auto&& x : ws->cls)  dds_waitset_detach (ws->waitseth, x->client.sub->rdcondh);
    ws->subs.resize (0);
    ws->gcs.resize (0);
    ws->srvs.resize (0);
    ws->cls.resize (0);
}

static void clean_waitset_caches ()
{
    /* Called whenever a subscriber, guard condition, service or client is deleted (as these may
       have been cached in a waitset), and drops all cached entities from all waitsets (just to keep
       life simple). I'm assuming one is not allowed to delete an entity while it is still being
       used ... */
    std::lock_guard<std::mutex> lock (gcdds.lock);
    for (auto&& ws : gcdds.waitsets) {
        std::lock_guard<std::mutex> lock (ws->lock);
        if (!ws->inuse) {
            waitset_detach (ws);
        }
    }
}

extern "C" rmw_ret_t rmw_wait (rmw_subscriptions_t *subs, rmw_guard_conditions_t *gcs, rmw_services_t *srvs, rmw_clients_t *cls, rmw_wait_set_t *wait_set, const rmw_time_t *wait_timeout)
{
    RET_NULL (wait_set);
    CddsWaitset *ws = static_cast<CddsWaitset *> (wait_set->data);
    RET_NULL (ws);

    {
        std::lock_guard<std::mutex> lock (ws->lock);
        if (ws->inuse) {
            RMW_SET_ERROR_MSG ("concurrent calls to rmw_wait on a single waitset is not supported");
            return RMW_RET_ERROR;
        }
        ws->inuse = true;
    }

    if (require_reattach (ws->subs, subs ? subs->subscriber_count : 0,     subs ? subs->subscribers : nullptr) ||
        require_reattach (ws->gcs,  gcs  ? gcs->guard_condition_count : 0, gcs  ? gcs->guard_conditions : nullptr) ||
        require_reattach (ws->srvs, srvs ? srvs->service_count : 0,        srvs ? srvs->services : nullptr) ||
        require_reattach (ws->cls,  cls  ? cls->client_count : 0,          cls  ? cls->clients : nullptr)) {
        size_t nelems = 0;
        waitset_detach (ws);
#define ATTACH(type, var, name, cond) do {                              \
            ws->var.resize (0);                                         \
            if (var) {                                                  \
                ws->var.reserve (var->name##_count);                    \
                for (size_t i = 0; i < var->name##_count; i++) {        \
                    auto x = static_cast<type *> (var->name##s[i]);     \
                    ws->var.push_back (x);                              \
                    dds_waitset_attach (ws->waitseth, x->cond, nelems); \
                    nelems++;                                           \
                }                                                       \
            }                                                           \
        } while (0)
        ATTACH (CddsSubscription, subs, subscriber, rdcondh);
        ATTACH (CddsGuardCondition, gcs, guard_condition, gcondh);
        ATTACH (CddsService, srvs, service, service.sub->rdcondh);
        ATTACH (CddsClient, cls, client, client.sub->rdcondh);
#undef ATTACH
        ws->trigs.reserve (nelems + 1);
    }

    const dds_duration_t timeout =
        (wait_timeout == NULL) ? DDS_NEVER : (dds_duration_t) wait_timeout->sec * 1000000000 + wait_timeout->nsec;
    const dds_return_t ntrig = dds_waitset_wait (ws->waitseth, ws->trigs.data (), ws->trigs.capacity (), timeout);
    ws->trigs.resize (ntrig);
    std::sort (ws->trigs.begin (), ws->trigs.end ());
    ws->trigs.push_back ((dds_attach_t) -1);

    {
        long trig_idx = 0;
        bool dummy;
        size_t nelems = 0;
#define DETACH(type, var, name, cond, on_triggered) do {                \
            if (var) {                                                  \
                for (size_t i = 0; i < var->name##_count; i++) {        \
                    auto x = static_cast<type *> (var->name##s[i]);     \
                    /*dds_waitset_detach (ws->waitseth, x->cond);*/     \
                    if (ws->trigs[trig_idx] == (long) nelems) {         \
                        on_triggered;                                   \
                        trig_idx++;                                     \
                    } else {                                            \
                        var->name##s[i] = nullptr;                      \
                    }                                                   \
                    nelems++;                                           \
                }                                                       \
            }                                                           \
        } while (0)
        DETACH (CddsSubscription, subs, subscriber, rdcondh, (void) x);
        DETACH (CddsGuardCondition, gcs, guard_condition, gcondh, dds_take_guardcondition (x->gcondh, &dummy));
        DETACH (CddsService, srvs, service, service.sub->rdcondh, (void) x);
        DETACH (CddsClient, cls, client, client.sub->rdcondh, (void) x);
#undef DETACH
    }

    {
        std::lock_guard<std::mutex> lock (ws->lock);
        ws->inuse = false;
    }

    return (ws->trigs.size () == 0) ? RMW_RET_TIMEOUT : RMW_RET_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    CLIENTS AND SERVERS                                            ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

static rmw_ret_t rmw_take_response_request (CddsCS *cs, rmw_request_id_t *request_header, void *ros_data, bool *taken, dds_instance_handle_t srcfilter)
{
    RET_NULL (taken);
    RET_NULL (ros_data);
    RET_NULL (request_header);
    cdds_request_wrapper_t wrap;
    dds_sample_info_t info;
    wrap.data = ros_data;
    void *wrap_ptr = static_cast<void *> (&wrap);
    while (dds_take (cs->sub->subh, &wrap_ptr, &info, 1, 1) == 1) {
        if (info.valid_data) {
            memset (request_header, 0, sizeof (wrap.header));
            assert (sizeof (wrap.header.guid) < sizeof (wrap.header.guid));
            memcpy (request_header->writer_guid, &wrap.header.guid, sizeof (wrap.header.guid));
            request_header->sequence_number = wrap.header.seq;
            if (srcfilter == 0 || srcfilter == wrap.header.guid) {
                *taken = true;
                return RMW_RET_OK;
            }
        }
    }
    *taken = false;
    return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_take_response (const rmw_client_t *client, rmw_request_id_t *request_header, void *ros_response, bool *taken)
{
    RET_WRONG_IMPLID (client);
    auto info = static_cast<CddsClient *> (client->data);
    return rmw_take_response_request (&info->client, request_header, ros_response, taken, info->client.pub->pubiid);
}

extern "C" rmw_ret_t rmw_take_request (const rmw_service_t *service, rmw_request_id_t *request_header, void *ros_request, bool *taken)
{
    RET_WRONG_IMPLID (service);
    auto info = static_cast<CddsService *> (service->data);
    return rmw_take_response_request (&info->service, request_header, ros_request, taken, 0);
}

static rmw_ret_t rmw_send_response_request (CddsCS *cs, const cdds_request_header_t& header, const void *ros_data)
{
    const cdds_request_wrapper_t wrap = { header, const_cast<void *> (ros_data) };
    if (dds_write (cs->pub->pubh, static_cast<const void *> (&wrap)) >= 0) {
        return RMW_RET_OK;
    } else {
        /* FIXME: what is the expected behavior when it times out? */
        RMW_SET_ERROR_MSG ("cannot publish data");
        //return RMW_RET_ERROR;
        return RMW_RET_OK;
    }
}

extern "C" rmw_ret_t rmw_send_response (const rmw_service_t *service, rmw_request_id_t *request_header, void *ros_response)
{
    RET_WRONG_IMPLID (service);
    RET_NULL (request_header);
    RET_NULL (ros_response);
    CddsService *info = static_cast<CddsService *> (service->data);
    cdds_request_header_t header;
    memcpy (&header.guid, request_header->writer_guid, sizeof (header.guid));
    header.seq = request_header->sequence_number;
    return rmw_send_response_request (&info->service, header, ros_response);
}

extern "C" rmw_ret_t rmw_send_request (const rmw_client_t *client, const void *ros_request, int64_t *sequence_id)
{
    static std::atomic_uint next_request_id;
    RET_WRONG_IMPLID (client);
    RET_NULL (ros_request);
    RET_NULL (sequence_id);
    auto info = static_cast<CddsClient *> (client->data);
    cdds_request_header_t header;
    header.guid = info->client.pub->pubiid;
    header.seq = *sequence_id = next_request_id++;
    return rmw_send_response_request (&info->client, header, ros_request);
}

static const rosidl_service_type_support_t *get_service_typesupport (const rosidl_service_type_support_t *type_supports)
{
    const rosidl_service_type_support_t *ts;
    if ((ts = get_service_typesupport_handle (type_supports, rosidl_typesupport_introspection_c__identifier)) != nullptr) {
        return ts;
    } else if ((ts = get_service_typesupport_handle (type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr) {
        return ts;
    } else {
        RMW_SET_ERROR_MSG ("service type support not from this implementation");
        return nullptr;
    }
}

static rmw_ret_t rmw_init_cs (CddsCS *cs, const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies, bool is_service)
{
    RET_WRONG_IMPLID (node);
    RET_NULL_OR_EMPTYSTR (service_name);
    RET_NULL (qos_policies);
    auto node_impl = static_cast<CddsNode *> (node->data);
    RET_NULL (node_impl);
    const rosidl_service_type_support_t *type_support = get_service_typesupport (type_supports);
    RET_NULL (type_support);

    auto pub = new CddsPublisher ();
    auto sub = new CddsSubscription ();
    std::string subtopic_name, pubtopic_name;
    void *pub_type_support, *sub_type_support;
    if (is_service) {
        sub_type_support = create_request_type_support (type_support->data, type_support->typesupport_identifier);
        pub_type_support = create_response_type_support (type_support->data, type_support->typesupport_identifier);
        subtopic_name = make_fqtopic (ros_service_requester_prefix, service_name, "_request", qos_policies);
        pubtopic_name = make_fqtopic (ros_service_response_prefix, service_name, "_reply", qos_policies);
    } else {
        pub_type_support = create_request_type_support (type_support->data, type_support->typesupport_identifier);
        sub_type_support = create_response_type_support (type_support->data, type_support->typesupport_identifier);
        pubtopic_name = make_fqtopic (ros_service_requester_prefix, service_name, "_request", qos_policies);
        subtopic_name = make_fqtopic (ros_service_response_prefix, service_name, "_reply", qos_policies);
    }

    RCUTILS_LOG_DEBUG_NAMED ("rmw_cyclonedds_cpp", "************ %s Details *********", is_service ? "Service" : "Client");
    RCUTILS_LOG_DEBUG_NAMED ("rmw_cyclonedds_cpp", "Sub Topic %s", subtopic_name.c_str ());
    RCUTILS_LOG_DEBUG_NAMED ("rmw_cyclonedds_cpp", "Pub Topic %s", pubtopic_name.c_str ());
    RCUTILS_LOG_DEBUG_NAMED ("rmw_cyclonedds_cpp", "***********");

    dds_entity_t pubtopic, subtopic;

    auto pub_st = create_sertopic (pubtopic_name.c_str (), type_support->typesupport_identifier, pub_type_support, true);
    auto sub_st = create_sertopic (subtopic_name.c_str (), type_support->typesupport_identifier, sub_type_support, true);

    dds_qos_t *qos;
    if ((pubtopic = dds_create_topic_arbitrary (gcdds.ppant, pub_st, nullptr, nullptr, nullptr)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create topic");
        goto fail_pubtopic;
    }
    if ((subtopic = dds_create_topic_arbitrary (gcdds.ppant, sub_st, nullptr, nullptr, nullptr)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create topic");
        goto fail_subtopic;
    }
    if ((qos = dds_create_qos ()) == nullptr) {
        goto fail_qos;
    }
    dds_qset_reliability (qos, DDS_RELIABILITY_RELIABLE, DDS_SECS (1));
    dds_qset_history (qos, DDS_HISTORY_KEEP_ALL, DDS_LENGTH_UNLIMITED);
    if ((pub->pubh = dds_create_writer (gcdds.ppant, pubtopic, qos, nullptr)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create writer");
        goto fail_writer;
    }
    if ((sub->subh = dds_create_reader (gcdds.ppant, subtopic, qos, nullptr)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create reader");
        goto fail_reader;
    }
    if ((sub->rdcondh = dds_create_readcondition (sub->subh, DDS_ANY_STATE)) < 0) {
        RMW_SET_ERROR_MSG ("failed to create readcondition");
        goto fail_readcond;
    }
    if (dds_get_instance_handle (pub->pubh, &pub->pubiid) < 0) {
        RMW_SET_ERROR_MSG ("failed to get instance handle for writer");
        goto fail_instance_handle;
    }
    dds_delete_qos (qos);

    cs->pub = pub;
    cs->sub = sub;
    return RMW_RET_OK;

 fail_instance_handle:
    dds_delete (sub->rdcondh);
 fail_readcond:
    dds_delete (sub->subh);
 fail_reader:
    dds_delete (pub->pubh);
 fail_writer:
    dds_delete_qos (qos);
 fail_qos:
    /* leak subtopic */
 fail_subtopic:
    /* leak pubtopic */
 fail_pubtopic:
    return RMW_RET_ERROR;
}

static void rmw_fini_cs (CddsCS *cs)
{
    dds_delete (cs->sub->rdcondh);
    dds_delete (cs->sub->subh);
    dds_delete (cs->pub->pubh);
}

extern "C" rmw_client_t *rmw_create_client (const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies)
{
    CddsClient *info = new CddsClient ();
    if (rmw_init_cs (&info->client, node, type_supports, service_name, qos_policies, false) != RMW_RET_OK) {
        delete (info);
        return nullptr;
    }
    rmw_client_t *rmw_client = rmw_client_allocate ();
    RET_NULL_X (rmw_client, goto fail_client);
    rmw_client->implementation_identifier = eclipse_cyclonedds_identifier;
    rmw_client->data = info;
    rmw_client->service_name = reinterpret_cast<const char *> (rmw_allocate (strlen (service_name) + 1));
    RET_NULL_X (rmw_client->service_name, goto fail_service_name);
    memcpy (const_cast<char *> (rmw_client->service_name), service_name, strlen (service_name) + 1);
    return rmw_client;
 fail_service_name:
    rmw_client_free (rmw_client);
 fail_client:
    rmw_fini_cs (&info->client);
    return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_client (rmw_node_t *node, rmw_client_t *client)
{
    RET_WRONG_IMPLID (node);
    RET_WRONG_IMPLID (client);
    auto info = static_cast<CddsClient *> (client->data);
    clean_waitset_caches ();
    rmw_fini_cs (&info->client);
    rmw_free (const_cast<char *> (client->service_name));
    rmw_client_free (client);
    return RMW_RET_OK;
}

extern "C" rmw_service_t *rmw_create_service (const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies)
{
    CddsService *info = new CddsService ();
    if (rmw_init_cs (&info->service, node, type_supports, service_name, qos_policies, true) != RMW_RET_OK) {
        delete (info);
        return nullptr;
    }
    rmw_service_t *rmw_service = rmw_service_allocate ();
    RET_NULL_X (rmw_service, goto fail_service);
    rmw_service->implementation_identifier = eclipse_cyclonedds_identifier;
    rmw_service->data = info;
    rmw_service->service_name = reinterpret_cast<const char *> (rmw_allocate (strlen (service_name) + 1));
    RET_NULL_X (rmw_service->service_name, goto fail_service_name);
    memcpy (const_cast<char *> (rmw_service->service_name), service_name, strlen (service_name) + 1);
    return rmw_service;
 fail_service_name:
    rmw_service_free (rmw_service);
 fail_service:
    rmw_fini_cs (&info->service);
    return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_service (rmw_node_t *node, rmw_service_t *service)
{
    RET_WRONG_IMPLID (node);
    RET_WRONG_IMPLID (service);
    auto info = static_cast<CddsService *> (service->data);
    clean_waitset_caches ();
    rmw_fini_cs (&info->service);
    rmw_free (const_cast<char *> (service->service_name));
    rmw_service_free (service);
    return RMW_RET_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    INTROSPECTION                                                  ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_get_node_names (const rmw_node_t *node, rcutils_string_array_t *node_names, rcutils_string_array_t *node_namespaces)
{
#if 0 // NIY
    RET_WRONG_IMPLID (node);
    if (rmw_check_zero_rmw_string_array (node_names) != RMW_RET_OK) {
        return RMW_RET_ERROR;
    }

    auto impl = static_cast<CddsNode *> (node->data);

    // FIXME: sorry, can't do it with current Zenoh
    auto participant_names = std::vector<std::string>{};
    rcutils_allocator_t allocator = rcutils_get_default_allocator ();
    rcutils_ret_t rcutils_ret =
        rcutils_string_array_init (node_names, participant_names.size (), &allocator);
    if (rcutils_ret != RCUTILS_RET_OK) {
        RMW_SET_ERROR_MSG (rcutils_get_error_string_safe ())
            return rmw_convert_rcutils_ret_to_rmw_ret (rcutils_ret);
    }
    for (size_t i = 0; i < participant_names.size (); ++i) {
        node_names->data[i] = rcutils_strdup (participant_names[i].c_str (), allocator);
        if (!node_names->data[i]) {
            RMW_SET_ERROR_MSG ("failed to allocate memory for node name")
                rcutils_ret = rcutils_string_array_fini (node_names);
            if (rcutils_ret != RCUTILS_RET_OK) {
                RCUTILS_LOG_ERROR_NAMED (
                        "rmw_cyclonedds_cpp",
                        "failed to cleanup during error handling: %s", rcutils_get_error_string_safe ());
            }
            return RMW_RET_BAD_ALLOC;
        }
    }
    return RMW_RET_OK;
#else
    (void) node; (void) node_names; (void) node_namespaces;
    return RMW_RET_TIMEOUT;
#endif
}

extern "C" rmw_ret_t rmw_get_topic_names_and_types (const rmw_node_t *node, rcutils_allocator_t *allocator, bool no_demangle, rmw_names_and_types_t *topic_names_and_types)
{
#if 0 // NIY
    RET_NULL (allocator);
    RET_WRONG_IMPLID (node);
    rmw_ret_t ret = rmw_names_and_types_check_zero (topic_names_and_types);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    auto impl = static_cast<CustomParticipantInfo *> (node->data);

    // Access the slave Listeners, which are the ones that have the topicnamesandtypes member
    // Get info from publisher and subscriber
    // Combined results from the two lists
    std::map<std::string, std::set<std::string>> topics;
    {
        ReaderInfo * slave_target = impl->secondarySubListener;
        slave_target->mapmutex.lock ();
        for (auto it : slave_target->topicNtypes) {
            if (!no_demangle && _get_ros_prefix_if_exists (it.first) != ros_topic_prefix) {
                // if we are demangling and this is not prefixed with rt/, skip it
                continue;
            }
            for (auto & itt : it.second) {
                topics[it.first].insert (itt);
            }
        }
        slave_target->mapmutex.unlock ();
    }
    {
        WriterInfo * slave_target = impl->secondaryPubListener;
        slave_target->mapmutex.lock ();
        for (auto it : slave_target->topicNtypes) {
            if (!no_demangle && _get_ros_prefix_if_exists (it.first) != ros_topic_prefix) {
                // if we are demangling and this is not prefixed with rt/, skip it
                continue;
            }
            for (auto & itt : it.second) {
                topics[it.first].insert (itt);
            }
        }
        slave_target->mapmutex.unlock ();
    }

    // Copy data to results handle
    if (topics.size () > 0) {
        // Setup string array to store names
        rmw_ret_t rmw_ret = rmw_names_and_types_init (topic_names_and_types, topics.size (), allocator);
        if (rmw_ret != RMW_RET_OK) {
            return rmw_ret;
        }
        // Setup cleanup function, in case of failure below
        auto fail_cleanup = [&topic_names_and_types]() {
                                rmw_ret_t rmw_ret = rmw_names_and_types_fini (topic_names_and_types);
                                if (rmw_ret != RMW_RET_OK) {
                                    RCUTILS_LOG_ERROR_NAMED (
                                            "rmw_cyclonedds_cpp",
                                            "error during report of error: %s", rmw_get_error_string_safe ());
                                }
                            };
        // Setup demangling functions based on no_demangle option
        auto demangle_topic = _demangle_if_ros_topic;
        auto demangle_type = _demangle_if_ros_type;
        if (no_demangle) {
            auto noop = [](const std::string & in) {
                            return in;
                        };
            demangle_topic = noop;
            demangle_type = noop;
        }
        // For each topic, store the name, initialize the string array for types, and store all types
        size_t index = 0;
        for (const auto & topic_n_types : topics) {
            // Duplicate and store the topic_name
            char * topic_name = rcutils_strdup (demangle_topic (topic_n_types.first).c_str (), *allocator);
            if (!topic_name) {
                RMW_SET_ERROR_MSG_ALLOC ("failed to allocate memory for topic name", *allocator);
                fail_cleanup ();
                return RMW_RET_BAD_ALLOC;
            }
            topic_names_and_types->names.data[index] = topic_name;
            // Setup storage for types
            {
                rcutils_ret_t rcutils_ret = rcutils_string_array_init (
                        &topic_names_and_types->types[index],
                        topic_n_types.second.size (),
                        allocator);
                if (rcutils_ret != RCUTILS_RET_OK) {
                    RMW_SET_ERROR_MSG (rcutils_get_error_string_safe ())
                        fail_cleanup ();
                    return rmw_convert_rcutils_ret_to_rmw_ret (rcutils_ret);
                }
            }
            // Duplicate and store each type for the topic
            size_t type_index = 0;
            for (const auto & type : topic_n_types.second) {
                char * type_name = rcutils_strdup (demangle_type (type).c_str (), *allocator);
                if (!type_name) {
                    RMW_SET_ERROR_MSG_ALLOC ("failed to allocate memory for type name", *allocator)
                        fail_cleanup ();
                    return RMW_RET_BAD_ALLOC;
                }
                topic_names_and_types->types[index].data[type_index] = type_name;
                ++type_index;
            }  // for each type
            ++index;
        }  // for each topic
    }
    return RMW_RET_OK;
#else
    (void) node; (void) allocator; (void) no_demangle; (void) topic_names_and_types;
    return RMW_RET_TIMEOUT;
#endif
}

extern "C" rmw_ret_t rmw_get_service_names_and_types (const rmw_node_t *node, rcutils_allocator_t *allocator, rmw_names_and_types_t *service_names_and_types)
{
#if 0 // NIY
    if (!allocator) {
        RMW_SET_ERROR_MSG ("allocator is null")
            return RMW_RET_INVALID_ARGUMENT;
    }
    if (!node) {
        RMW_SET_ERROR_MSG_ALLOC ("null node handle", *allocator)
            return RMW_RET_INVALID_ARGUMENT;
    }
    rmw_ret_t ret = rmw_names_and_types_check_zero (service_names_and_types);
    if (ret != RMW_RET_OK) {
        return ret;
    }

    // Get participant pointer from node
    if (node->implementation_identifier != eclipse_cyclonedds_identifier) {
        RMW_SET_ERROR_MSG_ALLOC ("node handle not from this implementation", *allocator);
        return RMW_RET_ERROR;
    }

    auto impl = static_cast<CustomParticipantInfo *> (node->data);

    // Access the slave Listeners, which are the ones that have the topicnamesandtypes member
    // Get info from publisher and subscriber
    // Combined results from the two lists
    std::map<std::string, std::set<std::string>> services;
    {
        ReaderInfo * slave_target = impl->secondarySubListener;
        slave_target->mapmutex.lock ();
        for (auto it : slave_target->topicNtypes) {
            std::string service_name = _demangle_service_from_topic (it.first);
            if (!service_name.length ()) {
                // not a service
                continue;
            }
            for (auto & itt : it.second) {
                std::string service_type = _demangle_service_type_only (itt);
                if (service_type.length ()) {
                    services[service_name].insert (service_type);
                }
            }
        }
        slave_target->mapmutex.unlock ();
    }
    {
        WriterInfo * slave_target = impl->secondaryPubListener;
        slave_target->mapmutex.lock ();
        for (auto it : slave_target->topicNtypes) {
            std::string service_name = _demangle_service_from_topic (it.first);
            if (!service_name.length ()) {
                // not a service
                continue;
            }
            for (auto & itt : it.second) {
                std::string service_type = _demangle_service_type_only (itt);
                if (service_type.length ()) {
                    services[service_name].insert (service_type);
                }
            }
        }
        slave_target->mapmutex.unlock ();
    }

    // Fill out service_names_and_types
    if (services.size ()) {
        // Setup string array to store names
        rmw_ret_t rmw_ret =
            rmw_names_and_types_init (service_names_and_types, services.size (), allocator);
        if (rmw_ret != RMW_RET_OK) {
            return rmw_ret;
        }
        // Setup cleanup function, in case of failure below
        auto fail_cleanup = [&service_names_and_types]() {
                                rmw_ret_t rmw_ret = rmw_names_and_types_fini (service_names_and_types);
                                if (rmw_ret != RMW_RET_OK) {
                                    RCUTILS_LOG_ERROR_NAMED (
                                            "rmw_cyclonedds_cpp",
                                            "error during report of error: %s", rmw_get_error_string_safe ());
                                        }
                            };
        // For each service, store the name, initialize the string array for types, and store all types
        size_t index = 0;
        for (const auto & service_n_types : services) {
            // Duplicate and store the service_name
            char * service_name = rcutils_strdup (service_n_types.first.c_str (), *allocator);
            if (!service_name) {
                RMW_SET_ERROR_MSG_ALLOC ("failed to allocate memory for service name", *allocator);
                fail_cleanup ();
                return RMW_RET_BAD_ALLOC;
            }
            service_names_and_types->names.data[index] = service_name;
            // Setup storage for types
            {
                rcutils_ret_t rcutils_ret = rcutils_string_array_init (
                        &service_names_and_types->types[index],
                        service_n_types.second.size (),
                        allocator);
                if (rcutils_ret != RCUTILS_RET_OK) {
                    RMW_SET_ERROR_MSG (rcutils_get_error_string_safe ())
                        fail_cleanup ();
                    return rmw_convert_rcutils_ret_to_rmw_ret (rcutils_ret);
                }
            }
            // Duplicate and store each type for the service
            size_t type_index = 0;
            for (const auto & type : service_n_types.second) {
                char * type_name = rcutils_strdup (type.c_str (), *allocator);
                if (!type_name) {
                    RMW_SET_ERROR_MSG_ALLOC ("failed to allocate memory for type name", *allocator)
                        fail_cleanup ();
                    return RMW_RET_BAD_ALLOC;
                }
                service_names_and_types->types[index].data[type_index] = type_name;
                ++type_index;
            }  // for each type
            ++index;
        }  // for each service
    }
    return RMW_RET_OK;
#else
    (void) node; (void) allocator; (void) service_names_and_types;
    return RMW_RET_TIMEOUT;
#endif
}

extern "C" rmw_ret_t rmw_service_server_is_available (const rmw_node_t * node, const rmw_client_t * client, bool * is_available)
{
#if 0 // NIY
    if (!node) {
        RMW_SET_ERROR_MSG ("node handle is null");
        return RMW_RET_ERROR;
    }

    RMW_CHECK_TYPE_IDENTIFIERS_MATCH (
            node handle,
            node->implementation_identifier, eclipse_cyclonedds_identifier,
            return RMW_RET_ERROR);

    if (!client) {
        RMW_SET_ERROR_MSG ("client handle is null");
        return RMW_RET_ERROR;
    }

    if (!is_available) {
        RMW_SET_ERROR_MSG ("is_available is null");
        return RMW_RET_ERROR;
    }

    auto client_info = static_cast<CustomClientInfo *> (client->data);
    if (!client_info) {
        RMW_SET_ERROR_MSG ("client info handle is null");
        return RMW_RET_ERROR;
    }

    auto pub_topic_name =
        client_info->request_publisher_->getAttributes ().topic.getTopicName ();
    auto pub_partitions =
        client_info->request_publisher_->getAttributes ().qos.m_partition.getNames ();
    // every rostopic has exactly 1 partition field set
    if (pub_partitions.size () != 1) {
        RCUTILS_LOG_ERROR_NAMED (
                "rmw_cyclonedds_cpp",
                "Topic %s is not a ros topic", pub_topic_name.c_str ())
            RMW_SET_ERROR_MSG ((std::string (pub_topic_name) + " is a non-ros topic\n").c_str ());
        return RMW_RET_ERROR;
    }
    auto pub_fqdn = pub_partitions[0] + "/" + pub_topic_name;
    pub_fqdn = _demangle_if_ros_topic (pub_fqdn);

    auto sub_topic_name =
        client_info->response_subscriber_->getAttributes ().topic.getTopicName ();
    auto sub_partitions =
        client_info->response_subscriber_->getAttributes ().qos.m_partition.getNames ();
    // every rostopic has exactly 1 partition field set
    if (sub_partitions.size () != 1) {
        RCUTILS_LOG_ERROR_NAMED (
                "rmw_cyclonedds_cpp",
                "Topic %s is not a ros topic", pub_topic_name.c_str ())
            RMW_SET_ERROR_MSG ((std::string (sub_topic_name) + " is a non-ros topic\n").c_str ());
        return RMW_RET_ERROR;
    }
    auto sub_fqdn = sub_partitions[0] + "/" + sub_topic_name;
    sub_fqdn = _demangle_if_ros_topic (sub_fqdn);

    *is_available = false;
    size_t number_of_request_subscribers = 0;
    rmw_ret_t ret = rmw_count_subscribers (
            node,
            pub_fqdn.c_str (),
            &number_of_request_subscribers);
    if (ret != RMW_RET_OK) {
        // error string already set
        return ret;
    }
    if (number_of_request_subscribers == 0) {
        // not ready
        return RMW_RET_OK;
    }

    size_t number_of_response_publishers = 0;
    ret = rmw_count_publishers (
            node,
            sub_fqdn.c_str (),
            &number_of_response_publishers);
    if (ret != RMW_RET_OK) {
        // error string already set
        return ret;
    }
    if (number_of_response_publishers == 0) {
        // not ready
        return RMW_RET_OK;
    }

    // all conditions met, there is a service server available
    *is_available = true;
    return RMW_RET_OK;
#else
    (void) node; (void) client; (void) is_available;
    return RMW_RET_TIMEOUT;
#endif
}

extern "C" rmw_ret_t rmw_count_publishers (const rmw_node_t *node, const char *topic_name, size_t *count)
{
#if 0
    // safechecks

    if (!node) {
        RMW_SET_ERROR_MSG ("null node handle");
        return RMW_RET_ERROR;
    }
    // Get participant pointer from node
    if (node->implementation_identifier != eclipse_cyclonedds_identifier) {
        RMW_SET_ERROR_MSG ("node handle not from this implementation");
        return RMW_RET_ERROR;
    }

    auto impl = static_cast<CustomParticipantInfo *> (node->data);

    WriterInfo * slave_target = impl->secondaryPubListener;
    slave_target->mapmutex.lock ();
    *count = 0;
    for (auto it : slave_target->topicNtypes) {
        auto topic_fqdn = _demangle_if_ros_topic (it.first);
        if (topic_fqdn == topic_name) {
            *count += it.second.size ();
        }
    }
    slave_target->mapmutex.unlock ();

    RCUTILS_LOG_DEBUG_NAMED (
            "rmw_fastrtps_cpp",
            "looking for subscriber topic: %s, number of matches: %zu",
            topic_name, *count);

    return RMW_RET_OK;
#else
    (void) node; (void) topic_name; (void) count;
    return RMW_RET_TIMEOUT;
#endif
}

extern "C" rmw_ret_t rmw_count_subscribers (const rmw_node_t *node, const char *topic_name, size_t *count)
{
#if 0
    // safechecks

    if (!node) {
        RMW_SET_ERROR_MSG ("null node handle");
        return RMW_RET_ERROR;
    }
    // Get participant pointer from node
    if (node->implementation_identifier != eclipse_cyclonedds_identifier) {
        RMW_SET_ERROR_MSG ("node handle not from this implementation");
        return RMW_RET_ERROR;
    }

    CustomParticipantInfo * impl = static_cast<CustomParticipantInfo *> (node->data);

    ReaderInfo * slave_target = impl->secondarySubListener;
    *count = 0;
    slave_target->mapmutex.lock ();
    for (auto it : slave_target->topicNtypes) {
        auto topic_fqdn = _demangle_if_ros_topic (it.first);
        if (topic_fqdn == topic_name) {
            *count += it.second.size ();
        }
    }
    slave_target->mapmutex.unlock ();

    RCUTILS_LOG_DEBUG_NAMED (
            "rmw_fastrtps_cpp",
            "looking for subscriber topic: %s, number of matches: %zu",
            topic_name, *count);

    return RMW_RET_OK;
#else
    (void) node; (void) topic_name; (void) count;
    return RMW_RET_TIMEOUT;
#endif
}

extern "C" rmw_ret_t rmw_get_subscriber_names_and_types_by_node (const rmw_node_t *node, rcutils_allocator_t *allocator, const char *node_name, const char *node_namespace, bool no_demangle, rmw_names_and_types_t *topic_names_and_types)
{
    (void) node; (void) allocator; (void) node_name; (void) node_namespace; (void) no_demangle; (void) topic_names_and_types;
    return RMW_RET_TIMEOUT;
}

extern "C" rmw_ret_t rmw_get_publisher_names_and_types_by_node (const rmw_node_t *node, rcutils_allocator_t *allocator, const char *node_name, const char *node_namespace, bool no_demangle, rmw_names_and_types_t *topic_names_and_types)
{
    (void) node; (void) allocator; (void) node_name; (void) node_namespace; (void) no_demangle; (void) topic_names_and_types;
    return RMW_RET_TIMEOUT;
}

extern "C" rmw_ret_t rmw_get_service_names_and_types_by_node (const rmw_node_t *node, rcutils_allocator_t *allocator, const char *node_name, const char *node_namespace, bool no_demangle, rmw_names_and_types_t *topic_names_and_types)
{
    (void) node; (void) allocator; (void) node_name; (void) node_namespace; (void) no_demangle; (void) topic_names_and_types;
    return RMW_RET_TIMEOUT;
}
