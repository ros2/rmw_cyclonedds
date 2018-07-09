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

   - should serialize straight into serdata_t, instead of into a FastBuffer that then gets copied

   - topic creation: until cdds allows multiple calls to create_topic for the same topic, use
     create-or-else-find and leak

   - hash set of writer handles should be thread safe: no guarantee that no writers get
     added/deleted in parallel to each other or to takes

   - should use cdds read conditions, triggers & waitsets rather the local thing done here

   - introspection stuff not done yet (probably requires additions to cdds)

   - check / make sure a node remains valid while one of its subscriptions exists

   - writecdr/takecdr interface abuse is beyond redemption

   - service/client simply use the instance handle of its publisher as its GUID -- yikes! but it is
     actually only kinda wrong because the instance handles allocated by different instance of cdds
     are actually taken from uncorrelated (close to uncorrelated anyway) permutations of 64-bit
     unsigned integers

   - ... and many more things ...
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string>
#include <utility>
#include <vector>
#include <thread>
#include <unordered_set>

#include "rcutils/allocator.h"
#include "rcutils/filesystem.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"
#include "rcutils/types.h"

#include "rmw/allocators.h"
#include "rmw/convert_rcutils_ret_to_rmw_ret.h"
#include "rmw/error_handling.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h"

#include "rmw/impl/cpp/macros.hpp"

#include "namespace_prefix.hpp"
#include "fastcdr/FastBuffer.h"

#include "rmw_cyclonedds_cpp/MessageTypeSupport.hpp"
#include "rmw_cyclonedds_cpp/ServiceTypeSupport.hpp"

#include "ddsc/dds.h"
extern "C" {
    extern void ddsi_serdata_getblob (void **raw, size_t *sz, struct serdata *serdata);
    extern void ddsi_serdata_unref (struct serdata *serdata);
}

#include "rmw_cyclonedds_topic.h"

#define RET_ERR_X(msg, code) do { RMW_SET_ERROR_MSG(msg); code; } while(0)
#define RET_NULL_X(var, code) do { if (!var) RET_ERR_X(#var " is null", code); } while(0)
#define RET_ALLOC_X(var, code) do { if (!var) RET_ERR_X("failed to allocate " #var, code); } while(0)
#define RET_WRONG_IMPLID_X(var, code) do {                              \
        RET_NULL_X(var, code);                                          \
        if ((var)->implementation_identifier != adlink_cyclonedds_identifier) { \
            RET_ERR_X(#var " not from this implementation", code);      \
        }                                                               \
    } while(0)
#define RET_NULL_OR_EMPTYSTR_X(var, code) do {                  \
          if(!var || strlen(var) == 0) {                        \
              RET_ERR_X(#var " is null or empty string", code); \
          }                                                     \
      } while(0)
#define RET_ERR(msg) RET_ERR_X(msg, return RMW_RET_ERROR)
#define RET_NULL(var) RET_NULL_X(var, return RMW_RET_ERROR)
#define RET_ALLOC(var) RET_ALLOC_X(var, return RMW_RET_ERROR)
#define RET_WRONG_IMPLID(var) RET_WRONG_IMPLID_X(var, return RMW_RET_ERROR)
#define RET_NULL_OR_EMPTYSTR(var) RET_NULL_OR_EMPTYSTR_X(var, return RMW_RET_ERROR)

const char *const adlink_cyclonedds_identifier = "rmw_cyclonedds_cpp";

struct condition {
    std::mutex internalMutex_;
    std::atomic_uint triggerValue_;
    std::mutex *conditionMutex_;
    std::condition_variable *conditionVariable_;
    condition() : triggerValue_(0), conditionMutex_(nullptr), conditionVariable_(nullptr) { }
};

static void condition_set_trigger(struct condition *cond, unsigned value);
static void condition_add_trigger(struct condition *cond, int delta);

struct CddsTypeSupport {
    void *type_support_;
    const char *typesupport_identifier_;
};

/* this had better be compatible with the "guid" field in the rmw_request_id_t and the data field in rmw_gid_t */
typedef std::array<uint8_t,16> cdds_guid_t;
typedef std::array<uint8_t,RMW_GID_STORAGE_SIZE> cdds_gid_t;

/* instance handles are carefully constructed to be as close to uniformly distributed as possible
   for no other reason than making them near-perfect hash keys */
struct dds_instance_handle_hash {
public:
    std::size_t operator()(dds_instance_handle_t const& x) const noexcept {
        return static_cast<std::size_t>(x);
    }
};

struct CddsNode {
    dds_entity_t ppant;
    rmw_guard_condition_t *graph_guard_condition;
    std::unordered_set<dds_instance_handle_t, dds_instance_handle_hash> own_writers;
};

struct CddsPublisher {
    dds_entity_t pubh;
    dds_instance_handle_t pubiid;
    CddsTypeSupport ts;
};

struct CddsSubscription {
    typedef rmw_subscription_t rmw_type;
    typedef rmw_subscriptions_t rmw_set_type;
    dds_entity_t subh;
    CddsNode *node;
    bool ignore_local_publications;
    CddsTypeSupport ts;
    struct condition cond;
};

struct CddsCS {
    CddsPublisher *pub;
    CddsSubscription *sub;
};

struct CddsClient {
    typedef rmw_client_t rmw_type;
    typedef rmw_clients_t rmw_set_type;
    CddsCS client;
};

struct CddsService {
    typedef rmw_service_t rmw_type;
    typedef rmw_services_t rmw_set_type;
    CddsCS service;
};

struct CddsGuardCondition {
    struct condition cond;
};

/* iterators for sets of subscriptions, clients, services and guard conditions */
#define DEFITER1(const_, cddstype_, rmwtype_, name_)                    \
    static const_ Cdds##cddstype_ * const_ *begin(const_ rmw_##rmwtype_##_t& s) { \
        return (const_ Cdds##cddstype_ * const_ *)(&s.name_##s[0]);     \
    }                                                                   \
    static const_ Cdds##cddstype_ * const_ *end(const_ rmw_##rmwtype_##_t& s) { \
        return (const_ Cdds##cddstype_ * const_ *)(&s.name_##s[s.name_##_count]); \
    }
#define DEFITER(cddstype_, rmwtype_, name_)     \
    DEFITER1(, cddstype_, rmwtype_, name_)      \
    DEFITER1(const, cddstype_, rmwtype_, name_)
DEFITER(Subscription, subscriptions, subscriber)
DEFITER(Client, clients, client)
DEFITER(Service, services, service)
DEFITER(GuardCondition, guard_conditions, guard_condition)
#undef DEFITER
#undef DEFITER1

struct condition *get_condition(CddsSubscription *x) { return &x->cond; }
struct condition *get_condition(CddsClient *x) { return &x->client.sub->cond; }
struct condition *get_condition(CddsService *x) { return &x->service.sub->cond; }
struct condition *get_condition(CddsGuardCondition *x) { return &x->cond; }
template <typename T> const condition *get_condition(const T *x) { return get_condition(const_cast<T *>(x)); }

template<typename T> void condition_set_trigger(T *x, unsigned value) {
    condition_set_trigger(get_condition(x), value);
}
template<typename T> void condition_add_trigger(T *x, int delta) {
    condition_add_trigger(get_condition(x), delta);
}
template<typename T> bool condition_read(T *x) {
    return condition_read(get_condition(x));
}
template<typename T> bool condition_take(T *x) {
    return condition_take(get_condition(x));
}
template<typename T> void condition_attach(T *x, std::mutex *conditionMutex, std::condition_variable *conditionVariable) {
    condition_attach(get_condition(x), conditionMutex, conditionVariable);
}
template<typename T> void condition_detach(T *x) {
    condition_detach(get_condition(x));
}

typedef struct cdds_request_header {
    uint64_t guid;
    int64_t seq;
} cdds_request_header_t;


using MessageTypeSupport_c = rmw_cyclonedds_cpp::MessageTypeSupport<rosidl_typesupport_introspection_c__MessageMembers>;
using MessageTypeSupport_cpp = rmw_cyclonedds_cpp::MessageTypeSupport<rosidl_typesupport_introspection_cpp::MessageMembers>;
using RequestTypeSupport_c = rmw_cyclonedds_cpp::RequestTypeSupport<rosidl_typesupport_introspection_c__ServiceMembers, rosidl_typesupport_introspection_c__MessageMembers>;
using RequestTypeSupport_cpp = rmw_cyclonedds_cpp::RequestTypeSupport<rosidl_typesupport_introspection_cpp::ServiceMembers, rosidl_typesupport_introspection_cpp::MessageMembers>;
using ResponseTypeSupport_c = rmw_cyclonedds_cpp::ResponseTypeSupport<rosidl_typesupport_introspection_c__ServiceMembers, rosidl_typesupport_introspection_c__MessageMembers>;
using ResponseTypeSupport_cpp = rmw_cyclonedds_cpp::ResponseTypeSupport<rosidl_typesupport_introspection_cpp::ServiceMembers, rosidl_typesupport_introspection_cpp::MessageMembers>;

static bool using_introspection_c_typesupport(const char *typesupport_identifier)
{
    return typesupport_identifier == rosidl_typesupport_introspection_c__identifier;
}

static bool using_introspection_cpp_typesupport(const char *typesupport_identifier)
{
    return typesupport_identifier == rosidl_typesupport_introspection_cpp::typesupport_identifier;
}

static void *create_message_type_support(const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(untyped_members);
        return new MessageTypeSupport_c(members);
    } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(untyped_members);
        return new MessageTypeSupport_cpp(members);
    }
    RMW_SET_ERROR_MSG("Unknown typesupport identifier");
    return nullptr;
}

static void *create_request_type_support(const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(untyped_members);
        return new RequestTypeSupport_c(members);
    } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *>(untyped_members);
        return new RequestTypeSupport_cpp(members);
    }
    RMW_SET_ERROR_MSG("Unknown typesupport identifier");
    return nullptr;
}

static void *create_response_type_support(const void *untyped_members, const char *typesupport_identifier)
{
    if (using_introspection_c_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_c__ServiceMembers *>(untyped_members);
        return new ResponseTypeSupport_c(members);
    } else if (using_introspection_cpp_typesupport(typesupport_identifier)) {
        auto members = static_cast<const rosidl_typesupport_introspection_cpp::ServiceMembers *>(untyped_members);
        return new ResponseTypeSupport_cpp(members);
    }
    RMW_SET_ERROR_MSG("Unknown typesupport identifier");
    return nullptr;
}

template<typename ServiceType> const void *get_request_ptr(const void *untyped_service_members)
{
    auto service_members = static_cast<const ServiceType *>(untyped_service_members);
    RET_NULL_X(service_members, return nullptr);
    return service_members->request_members_;
}

template<typename ServiceType> const void *get_response_ptr(const void *untyped_service_members)
{
    auto service_members = static_cast<const ServiceType *>(untyped_service_members);
    RET_NULL_X(service_members, return nullptr);
    return service_members->response_members_;
}

static bool sermsg(const void *ros_message, eprosima::fastcdr::Cdr& ser, std::function<void(eprosima::fastcdr::Cdr&)> prefix, const CddsTypeSupport& ts)
{
    if (using_introspection_c_typesupport(ts.typesupport_identifier_)) {
        auto typed_typesupport = static_cast<MessageTypeSupport_c *>(ts.type_support_);
        return typed_typesupport->serializeROSmessage(ros_message, ser, prefix);
    } else if (using_introspection_cpp_typesupport(ts.typesupport_identifier_)) {
        auto typed_typesupport = static_cast<MessageTypeSupport_cpp *>(ts.type_support_);
        return typed_typesupport->serializeROSmessage(ros_message, ser, prefix);
    }
    RMW_SET_ERROR_MSG("Unknown typesupport identifier");
    return false;
}

static bool desermsg(eprosima::fastcdr::Cdr& deser, void *ros_message, std::function<void(eprosima::fastcdr::Cdr&)> prefix, const CddsTypeSupport& ts)
{
  if (using_introspection_c_typesupport(ts.typesupport_identifier_)) {
    auto typed_typesupport = static_cast<MessageTypeSupport_c *>(ts.type_support_);
    return typed_typesupport->deserializeROSmessage(deser, ros_message, prefix);
  } else if (using_introspection_cpp_typesupport(ts.typesupport_identifier_)) {
    auto typed_typesupport = static_cast<MessageTypeSupport_cpp *>(ts.type_support_);
    return typed_typesupport->deserializeROSmessage(deser, ros_message, prefix);
  }
  RMW_SET_ERROR_MSG("Unknown typesupport identifier");
  return false;
}

extern "C"
{
#pragma GCC visibility push(default)

    const char *rmw_get_implementation_identifier()
    {
        return adlink_cyclonedds_identifier;
    }

    rmw_ret_t rmw_init()
    {
        return RMW_RET_OK;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    NODES                                                          ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////
  
    rmw_node_t *rmw_create_node(const char *name, const char *namespace_, size_t domain_id, const rmw_node_security_options_t *security_options)
    {
        RET_NULL_X(name, return nullptr);
        RET_NULL_X(namespace_, return nullptr);
        (void)domain_id;
        (void)security_options;

        auto *node_impl = new CddsNode();
        rmw_node_t *node_handle = nullptr;
        RET_ALLOC_X(node_impl, goto fail_node_impl);
        rmw_guard_condition_t *graph_guard_condition;
        if (!(graph_guard_condition = rmw_create_guard_condition())) {
            goto fail_ggc;
        }
        node_impl->graph_guard_condition = graph_guard_condition;

        if ((node_impl->ppant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL)) < 0) {
            RMW_SET_ERROR_MSG("failed to create participant");
            goto fail_ppant;
        }

        node_handle = rmw_node_allocate();
        RET_ALLOC_X(node_handle, goto fail_node_handle);
        node_handle->implementation_identifier = adlink_cyclonedds_identifier;
        node_handle->data = node_impl;
    
        node_handle->name = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(name) + 1));
        RET_ALLOC_X(node_handle->name, goto fail_node_handle_name);
        memcpy(const_cast<char *>(node_handle->name), name, strlen(name) + 1);
    
        node_handle->namespace_ = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(namespace_) + 1));
        RET_ALLOC_X(node_handle->namespace_, goto fail_node_handle_namespace);
        memcpy(const_cast<char *>(node_handle->namespace_), namespace_, strlen(namespace_) + 1);
        return node_handle;

#if 0
    fail_add_node:
        rmw_free(const_cast<char *>(node_handle->namespace_));
#endif
    fail_node_handle_namespace:
        rmw_free(const_cast<char *>(node_handle->name));
    fail_node_handle_name:
        rmw_node_free(node_handle);
    fail_node_handle:
        if (dds_delete(node_impl->ppant) < 0) {
            RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to delete participant during error handling");
        }
    fail_ppant:
        if (RMW_RET_OK != rmw_destroy_guard_condition(graph_guard_condition)) {
            RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to destroy guard condition during error handling");
        }
    fail_ggc:
        delete node_impl;
    fail_node_impl:
        return nullptr;
    }

    rmw_ret_t rmw_destroy_node(rmw_node_t *node)
    {
        rmw_ret_t result_ret = RMW_RET_OK;
        RET_WRONG_IMPLID(node);
        auto node_impl = static_cast<CddsNode *>(node->data);
        RET_NULL(node_impl);
        if (dds_delete(node_impl->ppant) < 0) {
            RMW_SET_ERROR_MSG("failed to delete participant");
            result_ret = RMW_RET_ERROR;
        }
        rmw_free(const_cast<char *>(node->name));
        rmw_free(const_cast<char *>(node->namespace_));
        rmw_node_free(node);
        if (RMW_RET_OK != rmw_destroy_guard_condition(node_impl->graph_guard_condition)) {
            RMW_SET_ERROR_MSG("failed to destroy graph guard condition");
            result_ret = RMW_RET_ERROR;
        }
        delete node_impl;
        return result_ret;
    }

    const rmw_guard_condition_t *rmw_node_get_graph_guard_condition(const rmw_node_t *node)
    {
        RET_WRONG_IMPLID_X(node, return nullptr);
        auto node_impl = static_cast<CddsNode *>(node->data);
        RET_NULL_X(node_impl, return nullptr);
        return node_impl->graph_guard_condition;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    PUBLICATIONS                                                   ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////
  
    static rmw_ret_t rmw_write_ser(dds_entity_t pubh, eprosima::fastcdr::Cdr& ser)
    {
        const size_t sz = ser.getSerializedDataLength();
        const void *raw = static_cast<void *>(ser.getBufferPointer());
        /* shifting by 4 bytes skips the CDR header -- it should be identical and the entire
           writecdr is a hack at the moment anyway */
        if (dds_writecdr(pubh, (char *)raw + 4, sz) >= 0) {
            return RMW_RET_OK;
        } else {
            /* FIXME: what is the expected behavior when it times out? */
            RMW_SET_ERROR_MSG("cannot publish data");
            //return RMW_RET_ERROR;
            return RMW_RET_OK;
        }
    }
    
    rmw_ret_t rmw_publish(const rmw_publisher_t *publisher, const void *ros_message)
    {
        RET_WRONG_IMPLID(publisher);
        RET_NULL(ros_message);
        auto pub = static_cast<CddsPublisher *>(publisher->data);
        assert(pub);
        eprosima::fastcdr::FastBuffer buffer;
        eprosima::fastcdr::Cdr ser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
        if (sermsg(ros_message, ser, nullptr, pub->ts)) {
            return rmw_write_ser(pub->pubh, ser);
        } else {
            RMW_SET_ERROR_MSG("cannot serialize data");
            return RMW_RET_ERROR;
        }
    }

    static const rosidl_message_type_support_t *get_typesupport(const rosidl_message_type_support_t *type_supports)
    {
        const rosidl_message_type_support_t *ts;
        if ((ts = get_message_typesupport_handle(type_supports, rosidl_typesupport_introspection_c__identifier)) != nullptr) {
            return ts;
        } else if ((ts = get_message_typesupport_handle(type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr) {
            return ts;
        } else {
            RMW_SET_ERROR_MSG("type support not from this implementation");
            return nullptr;
        }
    }

    static std::string make_fqtopic(const char *prefix, const char *topic_name, const char *suffix, bool avoid_ros_namespace_conventions)
    {
        if (avoid_ros_namespace_conventions) {
            return std::string(topic_name) + "__" + std::string(suffix);
        } else {
            return std::string(prefix) + "/" + make_fqtopic(prefix, topic_name, suffix, true);
        }
    }

    static std::string make_fqtopic(const char *prefix, const char *topic_name, const char *suffix, const rmw_qos_profile_t *qos_policies)
    {
        return make_fqtopic(prefix, topic_name, suffix, qos_policies->avoid_ros_namespace_conventions);
    }

    static dds_qos_t *create_readwrite_qos(const rmw_qos_profile_t *qos_policies)
    {
        dds_qos_t *qos = dds_qos_create();
        switch (qos_policies->history) {
            case RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT:
            case RMW_QOS_POLICY_HISTORY_KEEP_LAST:
                if (qos_policies->depth > INT32_MAX) {
                    RMW_SET_ERROR_MSG("unsupported history depth");
                    dds_qos_delete(qos);
                    return nullptr;
                }
                dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, static_cast<uint32_t>(qos_policies->depth));
                break;
            case RMW_QOS_POLICY_HISTORY_KEEP_ALL:
                dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, DDS_LENGTH_UNLIMITED);
                break;
        }
        switch (qos_policies->reliability) {
            case RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT:
            case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
                dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
                break;
            case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
                dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
                break;
        }
        switch (qos_policies->durability) {
            case RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT:
            case RMW_QOS_POLICY_DURABILITY_VOLATILE:
                dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
                break;
            case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL:
                dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
                break;
        }
        return qos;
    }        
    
    static CddsPublisher *create_cdds_publisher(const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies)
    {
        RET_WRONG_IMPLID_X(node, return nullptr);
        RET_NULL_OR_EMPTYSTR_X(topic_name, return nullptr);
        RET_NULL_X(qos_policies, return nullptr);
        auto node_impl = static_cast<CddsNode *>(node->data);
        RET_NULL_X(node_impl, return nullptr);
        const rosidl_message_type_support_t *type_support = get_typesupport(type_supports);
        RET_NULL_X(type_support, return nullptr);
        CddsPublisher *pub = new CddsPublisher();
        dds_entity_t topic;
        dds_qos_t *qos;

        pub->ts.typesupport_identifier_ = type_support->typesupport_identifier;
        pub->ts.type_support_ = create_message_type_support(type_support->data, pub->ts.typesupport_identifier_);
        std::string fqtopic_name = make_fqtopic(ros_topic_prefix, topic_name, "", qos_policies);

        /*FIXME: fix topic creation issues in CycloneDDS */
        if ((topic = dds_create_topic(node_impl->ppant, &rmw_cyclonedds_topic_desc, fqtopic_name.c_str(), NULL, NULL)) < 0) {
            if ((topic = dds_find_topic(node_impl->ppant, fqtopic_name.c_str())) < 0) {
                RMW_SET_ERROR_MSG("failed to create topic");
                goto fail_topic;
            }
        }
        if ((qos = create_readwrite_qos(qos_policies)) == nullptr) {
            goto fail_qos;
        }
        if ((pub->pubh = dds_create_writer(node_impl->ppant, topic, qos, NULL)) < 0) {
            RMW_SET_ERROR_MSG("failed to create writer");
            goto fail_writer;
        }
        dds_qos_delete(qos);
        if (dds_get_instance_handle(pub->pubh, &pub->pubiid) < 0) {
            RMW_SET_ERROR_MSG("failed to get instance handle for writer");
            goto fail_instance_handle;
        }
        node_impl->own_writers.insert(pub->pubiid);
        /* FIXME: leak the topic for now */
        return pub;

    fail_instance_handle:
        if (dds_delete(pub->pubh) < 0) {
            RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to destroy writer during error handling");
        }
    fail_writer:
        dds_qos_delete(qos);
    fail_qos:
        /* not deleting topic -- have to sort out proper topic handling & that requires fixing a few
           things in cyclone as well */
    fail_topic:
        delete pub;
        return nullptr;
    }

    rmw_publisher_t *rmw_create_publisher(const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies)
    {
        CddsPublisher *pub;
        rmw_publisher_t *rmw_publisher;
        auto node_impl = static_cast<CddsNode *>(node->data);
        if ((pub = create_cdds_publisher(node, type_supports, topic_name, qos_policies)) == nullptr) {
            goto fail_common_init;
        }
        rmw_publisher = rmw_publisher_allocate();
        RET_ALLOC_X(rmw_publisher, goto fail_publisher);
        rmw_publisher->implementation_identifier = adlink_cyclonedds_identifier;
        rmw_publisher->data = pub;
        rmw_publisher->topic_name = reinterpret_cast<char *>(rmw_allocate(strlen(topic_name) + 1));
        RET_ALLOC_X(rmw_publisher->topic_name, goto fail_topic_name);
        memcpy(const_cast<char *>(rmw_publisher->topic_name), topic_name, strlen(topic_name) + 1);
        return rmw_publisher;
    fail_topic_name:
        rmw_publisher_free(rmw_publisher);
    fail_publisher:
        node_impl->own_writers.erase(pub->pubiid);
        if (dds_delete(pub->pubh) < 0) {
            RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to delete writer during error handling");
        }
        delete pub;
    fail_common_init:
        return nullptr;
    }

    rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t *publisher, rmw_gid_t *gid)
    {
        RET_WRONG_IMPLID(publisher);
        RET_NULL(gid);
        auto pub = static_cast<const CddsPublisher *>(publisher->data);
        RET_NULL(pub);
        gid->implementation_identifier = adlink_cyclonedds_identifier;
        memset(gid->data, 0, sizeof(gid->data));
        assert(sizeof(pub->pubiid) <= sizeof(gid->data));
        memcpy(gid->data, &pub->pubiid, sizeof(pub->pubiid));
        return RMW_RET_OK;
    }

    rmw_ret_t rmw_compare_gids_equal(const rmw_gid_t *gid1, const rmw_gid_t *gid2, bool *result)
    {
        RET_WRONG_IMPLID(gid1);
        RET_WRONG_IMPLID(gid2);
        RET_NULL(result);
        /* alignment is potentially lost because of the translation to an array of bytes, so use
           memcmp instead of a simple integer comparison */
        *result = memcmp(gid1->data, gid2->data, sizeof(gid1->data)) == 0;
        return RMW_RET_OK;
    }

    rmw_ret_t rmw_destroy_publisher(rmw_node_t *node, rmw_publisher_t *publisher)
    {
        RET_WRONG_IMPLID(node);
        RET_WRONG_IMPLID(publisher);
        auto node_impl = static_cast<CddsNode *>(node->data);
        auto pub = static_cast<CddsPublisher *>(publisher->data);
        if (pub != nullptr) {
            node_impl->own_writers.erase(pub->pubiid);
            if (dds_delete(pub->pubh) < 0) {
                RMW_SET_ERROR_MSG("failed to delete writer");
            }
            delete pub;
        }
        rmw_free(const_cast<char *>(publisher->topic_name));
        publisher->topic_name = nullptr;
        rmw_publisher_free(publisher);
        return RMW_RET_OK;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    SUBSCRIPTIONS                                                  ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////

    static void subhandler(dds_entity_t rd, void *vsub)
    {
        CddsSubscription *sub = static_cast<CddsSubscription *>(vsub);
        (void)rd;
        condition_add_trigger(&sub->cond, 1);
    }

    static CddsSubscription *create_cdds_subscription(const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies, bool ignore_local_publications)
    {
        RET_WRONG_IMPLID_X(node, return nullptr);
        RET_NULL_OR_EMPTYSTR_X(topic_name, return nullptr);
        RET_NULL_X(qos_policies, return nullptr);
        auto node_impl = static_cast<CddsNode *>(node->data);
        RET_NULL_X(node_impl, return nullptr);
        const rosidl_message_type_support_t *type_support = get_typesupport(type_supports);
        RET_NULL_X(type_support, return nullptr);
        (void)ignore_local_publications;
        CddsSubscription *sub = new CddsSubscription();
        dds_entity_t topic;
        dds_qos_t *qos;
        dds_listener_t *listeners;

        sub->node = node_impl;
        sub->ignore_local_publications = ignore_local_publications;
        sub->ts.typesupport_identifier_ = type_support->typesupport_identifier;
        sub->ts.type_support_ = create_message_type_support(type_support->data, sub->ts.typesupport_identifier_);
        std::string fqtopic_name = make_fqtopic(ros_topic_prefix, topic_name, "", qos_policies);

        /*FIXME: fix topic creation issues in CycloneDDS */
        if ((topic = dds_create_topic(node_impl->ppant, &rmw_cyclonedds_topic_desc, fqtopic_name.c_str(), NULL, NULL)) < 0) {
            if ((topic = dds_find_topic(node_impl->ppant, fqtopic_name.c_str())) < 0) {
                RMW_SET_ERROR_MSG("failed to create topic");
                goto fail_topic;
            }
        }
        if ((qos = create_readwrite_qos(qos_policies)) == nullptr) {
            goto fail_qos;
        }
        if ((listeners = dds_listener_create(static_cast<void *>(sub))) == nullptr) {
            goto fail_listener;
        }
        dds_lset_data_available(listeners, subhandler);
        if ((sub->subh = dds_create_reader(node_impl->ppant, topic, qos, listeners)) < 0) {
            RMW_SET_ERROR_MSG("failed to create reader");
            goto fail_reader;
        }
        dds_qos_delete(qos);
        dds_listener_delete(listeners);
        return sub;

    fail_reader:
        dds_listener_delete(listeners);
    fail_listener:
        dds_qos_delete(qos);
    fail_qos:
        /* FIXME: leak topic */
    fail_topic:
        delete sub;
        return nullptr;
    }

    rmw_subscription_t *rmw_create_subscription(const rmw_node_t *node, const rosidl_message_type_support_t *type_supports, const char *topic_name, const rmw_qos_profile_t *qos_policies, bool ignore_local_publications)
    {
        CddsSubscription *sub;
        rmw_subscription_t *rmw_subscription;
        if ((sub = create_cdds_subscription(node, type_supports, topic_name, qos_policies, ignore_local_publications)) == nullptr) {
            goto fail_common_init;
        }
        rmw_subscription = rmw_subscription_allocate();
        RET_ALLOC_X(rmw_subscription, goto fail_subscription);
        rmw_subscription->implementation_identifier = adlink_cyclonedds_identifier;
        rmw_subscription->data = sub;
        rmw_subscription->topic_name = reinterpret_cast<const char *>(rmw_allocate(strlen(topic_name) + 1));
        RET_ALLOC_X(rmw_subscription->topic_name, goto fail_topic_name);
        memcpy(const_cast<char *>(rmw_subscription->topic_name), topic_name, strlen(topic_name) + 1);
        return rmw_subscription;
    fail_topic_name:
        rmw_subscription_free(rmw_subscription);
    fail_subscription:
        if (dds_delete(sub->subh) < 0) {
            RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to delete writer during error handling");
        }
        delete sub;
    fail_common_init:
        return nullptr;
    }

    rmw_ret_t rmw_destroy_subscription(rmw_node_t *node, rmw_subscription_t *subscription)
    {
        RET_WRONG_IMPLID(node);
        RET_WRONG_IMPLID(subscription);
        auto sub = static_cast<CddsSubscription *>(subscription->data);
        if (sub != nullptr) {
            if (dds_delete(sub->subh) < 0) {
                RMW_SET_ERROR_MSG("failed to delete reader");
            }
            delete sub;
        }
        rmw_free(const_cast<char *>(subscription->topic_name));
        subscription->topic_name = nullptr;
        rmw_subscription_free(subscription);
        return RMW_RET_OK;
    }

    static rmw_ret_t rmw_take_int(const rmw_subscription_t *subscription, void *ros_message, bool *taken, rmw_message_info_t *message_info)
    {
        RET_NULL(taken);
        RET_NULL(ros_message);
        RET_WRONG_IMPLID(subscription);
        CddsSubscription *sub = static_cast<CddsSubscription *>(subscription->data);
        RET_NULL(sub);
        struct serdata *sd;
        dds_sample_info_t info;
        while (dds_takecdr(sub->subh, &sd, 1, &info, DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE) == 1) {
            condition_add_trigger(&sub->cond, -1);
            if (info.valid_data && !(sub->ignore_local_publications && sub->node->own_writers.count(info.publication_handle))) {
                size_t sz;
                void *raw;
                ddsi_serdata_getblob(&raw, &sz, sd);
                eprosima::fastcdr::FastBuffer buffer(static_cast<char *>(raw), sz);
                eprosima::fastcdr::Cdr deser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
                desermsg(deser, ros_message, nullptr, sub->ts);
                ddsi_serdata_unref(sd);
                if (message_info) {
                    message_info->publisher_gid.implementation_identifier = adlink_cyclonedds_identifier;
                    memset(message_info->publisher_gid.data, 0, sizeof(message_info->publisher_gid.data));
                    assert(sizeof(info.publication_handle) <= sizeof(message_info->publisher_gid.data));
                    memcpy(message_info->publisher_gid.data, &info.publication_handle, sizeof(info.publication_handle));
                }
                *taken = true;
                return RMW_RET_OK;
            } else {
                ddsi_serdata_unref(sd);
            }
        }
        *taken = false;
        return RMW_RET_OK;
    }

    rmw_ret_t rmw_take(const rmw_subscription_t *subscription, void *ros_message, bool *taken)
    {
        return rmw_take_int(subscription, ros_message, taken, nullptr);
    }

    rmw_ret_t rmw_take_with_info(const rmw_subscription_t *subscription, void *ros_message, bool *taken, rmw_message_info_t *message_info)
    {
        return rmw_take_int(subscription, ros_message, taken, message_info);
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    GUARDS AND WAITSETS                                            ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////

    struct CddsWaitset {
        std::condition_variable condition;
        std::mutex condition_mutex;
    };

    static void condition_set_trigger(struct condition *cond, unsigned value)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        if (cond->conditionMutex_ != nullptr) {
            std::unique_lock<std::mutex> clock(*cond->conditionMutex_);
            // the change to triggerValue_ needs to be mutually exclusive with
            // rmw_wait() which checks hasTriggered() and decides if wait() needs to
            // be called
            const bool notify = (value > 0 && cond->triggerValue_ == 0);
            cond->triggerValue_ = value;
            clock.unlock();
            if (notify) {
                cond->conditionVariable_->notify_one();
            }
        } else {
            cond->triggerValue_ = value;
        }
    }

    static void condition_add_trigger(struct condition *cond, int delta)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        if (cond->conditionMutex_ != nullptr) {
            std::unique_lock<std::mutex> clock(*cond->conditionMutex_);
            // the change to triggerValue_ needs to be mutually exclusive with
            // rmw_wait() which checks hasTriggered() and decides if wait() needs to
            // be called
            const bool notify = (delta > 0 && cond->triggerValue_ == 0);
            assert(delta >= 0 || cond->triggerValue_ >= (unsigned)-delta);
            cond->triggerValue_ += delta;
            clock.unlock();
            if (notify) {
                cond->conditionVariable_->notify_one();
            }
        } else {
            assert(delta >= 0 || cond->triggerValue_ >= (unsigned)-delta);
            cond->triggerValue_ += delta;
        }
    }

    static void condition_attach(struct condition *cond, std::mutex *conditionMutex, std::condition_variable *conditionVariable)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        cond->conditionMutex_ = conditionMutex;
        cond->conditionVariable_ = conditionVariable;
    }

    static void condition_detach(struct condition *cond)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        cond->conditionMutex_ = nullptr;
        cond->conditionVariable_ = nullptr;
    }

    static bool condition_read(const struct condition *cond)
    {
        return cond->triggerValue_ > 0;
    }

    static bool condition_read(struct condition *cond)
    {
        return condition_read(const_cast<const struct condition *>(cond));
    }

    static bool condition_take(struct condition *cond)
    {
        std::lock_guard<std::mutex> lock(cond->internalMutex_);
        bool ret = cond->triggerValue_ > 0;
        cond->triggerValue_ = 0;
        return ret;
    }

    rmw_guard_condition_t *rmw_create_guard_condition()
    {
        rmw_guard_condition_t *guard_condition_handle = new rmw_guard_condition_t;
        guard_condition_handle->implementation_identifier = adlink_cyclonedds_identifier;
        guard_condition_handle->data = new CddsGuardCondition();
        return guard_condition_handle;
    }

    rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t *guard_condition)
    {
        RET_NULL(guard_condition);
        delete static_cast<CddsGuardCondition *>(guard_condition->data);
        delete guard_condition;
        return RMW_RET_OK;
    }

    rmw_ret_t rmw_trigger_guard_condition(const rmw_guard_condition_t *guard_condition_handle)
    {
        RET_WRONG_IMPLID(guard_condition_handle);
        condition_set_trigger(static_cast<CddsGuardCondition *>(guard_condition_handle->data), 1);
        return RMW_RET_OK;
    }

    rmw_wait_set_t *rmw_create_wait_set(size_t max_conditions)
    {
        (void)max_conditions;
        rmw_wait_set_t * wait_set = rmw_wait_set_allocate();
        CddsWaitset *ws = nullptr;
        RET_ALLOC_X(wait_set, goto fail_alloc_wait_set);
        wait_set->implementation_identifier = adlink_cyclonedds_identifier;
        wait_set->data = rmw_allocate(sizeof(CddsWaitset));
        RET_ALLOC_X(wait_set->data, goto fail_alloc_wait_set_data);
        // This should default-construct the fields of CddsWaitset
        ws = static_cast<CddsWaitset *>(wait_set->data);
        RMW_TRY_PLACEMENT_NEW(ws, ws, goto fail_placement_new, CddsWaitset, )
        if (!ws) {
            RMW_SET_ERROR_MSG("failed to construct wait set info struct");
            goto fail_ws;
        }
        return wait_set;

    fail_ws:
        RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE(ws->~CddsWaitset(), ws)
    fail_placement_new:
        rmw_free(wait_set->data);
    fail_alloc_wait_set_data:
        rmw_wait_set_free(wait_set);
    fail_alloc_wait_set:
        return nullptr;
    }

    rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t * wait_set)
    {
        RET_WRONG_IMPLID(wait_set);
        auto result = RMW_RET_OK;
        auto ws = static_cast<CddsWaitset *>(wait_set->data);
        RET_NULL(ws);
        std::mutex *conditionMutex = &ws->condition_mutex;
        RET_NULL(conditionMutex);
        RMW_TRY_DESTRUCTOR(ws->~CddsWaitset(), ws, result = RMW_RET_ERROR)
        rmw_free(wait_set->data);
        rmw_wait_set_free(wait_set);
        return result;
    }

    static bool check_wait_set_for_data(const rmw_subscriptions_t *subs, const rmw_guard_conditions_t *gcs, const rmw_services_t *srvs, const rmw_clients_t *cls)
    {
        if (subs) { for (auto&& x : *subs) { if (condition_read(x)) return true; } }
        if (cls)  { for (auto&& x : *cls)  { if (condition_read(x)) return true; } }
        if (srvs) { for (auto&& x : *srvs) { if (condition_read(x)) return true; } }
        if (gcs)  { for (auto&& x : *gcs)  { if (condition_read(x)) return true; } }
        return false;
    }

    rmw_ret_t rmw_wait(rmw_subscriptions_t *subs, rmw_guard_conditions_t *gcs, rmw_services_t *srvs, rmw_clients_t *cls, rmw_wait_set_t *wait_set, const rmw_time_t *wait_timeout)
    {
        RET_NULL(wait_set);
        CddsWaitset *ws = static_cast<CddsWaitset *>(wait_set->data);
        RET_NULL(ws);
        std::mutex *conditionMutex = &ws->condition_mutex;
        std::condition_variable *conditionVariable = &ws->condition;
        
        if (subs) { for (auto&& x : *subs) condition_attach(x, conditionMutex, conditionVariable); }
        if (cls)  { for (auto&& x : *cls)  condition_attach(x, conditionMutex, conditionVariable); }
        if (srvs) { for (auto&& x : *srvs) condition_attach(x, conditionMutex, conditionVariable); }
        if (gcs)  { for (auto&& x : *gcs)  condition_attach(x, conditionMutex, conditionVariable); }

        // This mutex prevents any of the listeners to change the internal state and notify the
        // condition between the call to hasData() / hasTriggered() and wait() otherwise the
        // decision to wait might be incorrect
        std::unique_lock<std::mutex> lock(*conditionMutex);

        // First check variables.
        // If wait_timeout is null, wait indefinitely (so we have to wait)
        // If wait_timeout is not null and either of its fields are nonzero, we have to wait
        bool timeout;
        if (check_wait_set_for_data(subs, gcs, srvs, cls)) {
            timeout = false;
        } else if (wait_timeout && wait_timeout->sec == 0 && wait_timeout->nsec == 0) {
            /* timeout = 0: no waiting required */
            timeout = true;
        } else {
            auto predicate = [subs, gcs, srvs, cls]() { return check_wait_set_for_data(subs, gcs, srvs, cls); };
            if (!wait_timeout) {
                conditionVariable->wait(lock, predicate);
                timeout = false;
            } else {
                auto n = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(wait_timeout->sec));
                n += std::chrono::nanoseconds(wait_timeout->nsec);
                timeout = !conditionVariable->wait_for(lock, n, predicate);
            }
        }

        // Unlock the condition variable mutex to prevent deadlocks that can occur if
        // a listener triggers while the condition variable is being detached.
        // Listeners will no longer be prevented from changing their internal state,
        // but that should not cause issues (if a listener has data / has triggered
        // after we check, it will be caught on the next call to this function).
        lock.unlock();

        if (subs) { for (auto&& x : *subs) { condition_detach(x); if (!condition_read(x)) x = nullptr; } }
        if (cls)  { for (auto&& x : *cls)  { condition_detach(x); if (!condition_read(x)) x = nullptr; } }
        if (srvs) { for (auto&& x : *srvs) { condition_detach(x); if (!condition_read(x)) x = nullptr; } }
        // guard conditions are auto-resetting, hence condition_take
        if (gcs)  { for (auto&& x : *gcs)  { condition_detach(x); if (!condition_take(x)) x = nullptr; } }

        return timeout ? RMW_RET_TIMEOUT : RMW_RET_OK;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    CLIENTS AND SERVERS                                            ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////

    static rmw_ret_t rmw_take_response_request(CddsCS *cs, rmw_request_id_t *request_header, void *ros_data, bool *taken, dds_instance_handle_t srcfilter)
    {
        RET_NULL(taken);
        RET_NULL(ros_data);
        RET_NULL(request_header);
        struct serdata *sd;
        dds_sample_info_t info;
        while (dds_takecdr(cs->sub->subh, &sd, 1, &info, DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE) == 1) {
            condition_add_trigger(&cs->sub->cond, -1);
            if (info.valid_data) {
                size_t sz;
                void *raw;
                ddsi_serdata_getblob(&raw, &sz, sd);
                eprosima::fastcdr::FastBuffer buffer(static_cast<char *>(raw), sz);
                eprosima::fastcdr::Cdr deser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
                cdds_request_header_t header;
                desermsg(deser, ros_data, [&header](eprosima::fastcdr::Cdr& ser) { ser >> header.guid; ser >> header.seq; }, cs->sub->ts);
                ddsi_serdata_unref(sd);
                memset(request_header, 0, sizeof(*request_header));
                assert(sizeof(header.guid) < sizeof(request_header->writer_guid));
                memcpy(request_header->writer_guid, &header.guid, sizeof(header.guid));
                request_header->sequence_number = header.seq;
                if (srcfilter == 0 || srcfilter == header.guid) {
                    *taken = true;
                    return RMW_RET_OK;
                }
            } else {
                ddsi_serdata_unref(sd);
            }
        }
        *taken = false;
        return RMW_RET_OK;
    }
    
    rmw_ret_t rmw_take_response(const rmw_client_t *client, rmw_request_id_t *request_header, void *ros_response, bool *taken)
    {
        RET_WRONG_IMPLID(client);
        auto info = static_cast<CddsClient *>(client->data);
        return rmw_take_response_request(&info->client, request_header, ros_response, taken, info->client.pub->pubiid);
    }
    
    rmw_ret_t rmw_take_request(const rmw_service_t *service, rmw_request_id_t *request_header, void *ros_request, bool *taken)
    {
        RET_WRONG_IMPLID(service);
        auto info = static_cast<CddsService *>(service->data);
        return rmw_take_response_request(&info->service, request_header, ros_request, taken, 0);
    }

    static rmw_ret_t rmw_send_response_request(CddsCS *cs, cdds_request_header_t *header, const void *ros_data)
    {
        eprosima::fastcdr::FastBuffer buffer;
        eprosima::fastcdr::Cdr ser(buffer, eprosima::fastcdr::Cdr::DEFAULT_ENDIAN, eprosima::fastcdr::Cdr::DDS_CDR);
        if (sermsg(ros_data, ser, [&header](eprosima::fastcdr::Cdr& ser) { ser << header->guid; ser << header->seq; }, cs->pub->ts)) {
            return rmw_write_ser(cs->pub->pubh, ser);
        } else {
            RMW_SET_ERROR_MSG("cannot serialize data");
            return RMW_RET_ERROR;
        }
    }

    rmw_ret_t rmw_send_response(const rmw_service_t *service, rmw_request_id_t *request_header, void *ros_response)
    {
        RET_WRONG_IMPLID(service);
        RET_NULL(request_header);
        RET_NULL(ros_response);
        CddsService *info = static_cast<CddsService *>(service->data);
        cdds_request_header_t header;
        memcpy(&header.guid, request_header->writer_guid, sizeof(header.guid));
        header.seq = request_header->sequence_number;
        return rmw_send_response_request(&info->service, &header, ros_response);
    }

    rmw_ret_t rmw_send_request(const rmw_client_t *client, const void *ros_request, int64_t *sequence_id)
    {
        static std::atomic_uint next_request_id;
        RET_WRONG_IMPLID(client);
        RET_NULL(ros_request);
        RET_NULL(sequence_id);
        auto info = static_cast<CddsClient *>(client->data);
        cdds_request_header_t header;
        header.guid = info->client.pub->pubiid;
        header.seq = *sequence_id = next_request_id++;
        return rmw_send_response_request(&info->client, &header, ros_request);
    }

    static const rosidl_service_type_support_t *get_service_typesupport(const rosidl_service_type_support_t *type_supports)
    {
        const rosidl_service_type_support_t *ts;
        if ((ts = get_service_typesupport_handle(type_supports, rosidl_typesupport_introspection_c__identifier)) != nullptr) {
            return ts;
        } else if ((ts = get_service_typesupport_handle(type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr) {
            return ts;
        } else {
            RMW_SET_ERROR_MSG("service type support not from this implementation");
            return nullptr;
        }
    }
    
    static rmw_ret_t rmw_init_cs(CddsCS *cs, const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies, bool is_service)
    {
        RET_WRONG_IMPLID(node);
        RET_NULL_OR_EMPTYSTR(service_name);
        RET_NULL(qos_policies);
        auto node_impl = static_cast<CddsNode *>(node->data);
        RET_NULL(node_impl);
        const rosidl_service_type_support_t *type_support = get_service_typesupport(type_supports);
        RET_NULL(type_support);

        auto pub = new CddsPublisher();
        auto sub = new CddsSubscription();
        std::string subtopic_name, pubtopic_name;
        pub->ts.typesupport_identifier_ = type_support->typesupport_identifier;
        sub->ts.typesupport_identifier_ = type_support->typesupport_identifier;
        if (is_service) {
            sub->ts.type_support_ = create_request_type_support(type_support->data, type_support->typesupport_identifier);
            pub->ts.type_support_ = create_response_type_support(type_support->data, type_support->typesupport_identifier);
            subtopic_name = make_fqtopic(ros_service_requester_prefix, service_name, "_request", qos_policies);
            pubtopic_name = make_fqtopic(ros_service_response_prefix, service_name, "_reply", qos_policies);
        } else {
            pub->ts.type_support_ = create_request_type_support(type_support->data, type_support->typesupport_identifier);
            sub->ts.type_support_ = create_response_type_support(type_support->data, type_support->typesupport_identifier);
            pubtopic_name = make_fqtopic(ros_service_requester_prefix, service_name, "_request", qos_policies);
            subtopic_name = make_fqtopic(ros_service_response_prefix, service_name, "_reply", qos_policies);
        }

        
        RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "************ %s Details *********", is_service ? "Service" : "Client")
        RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "Sub Topic %s", subtopic_name.c_str())
        RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "Pub Topic %s", pubtopic_name.c_str())
        RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "***********")

        dds_entity_t pubtopic, subtopic;
        dds_qos_t *qos;
        dds_listener_t *listeners;
        if ((pubtopic = dds_create_topic(node_impl->ppant, &rmw_cyclonedds_topic_desc, pubtopic_name.c_str(), NULL, NULL)) < 0) {
            if ((pubtopic = dds_find_topic(node_impl->ppant, pubtopic_name.c_str())) < 0) {
                RMW_SET_ERROR_MSG("failed to create topic");
                goto fail_pubtopic;
            }
        }
        if ((subtopic = dds_create_topic(node_impl->ppant, &rmw_cyclonedds_topic_desc, subtopic_name.c_str(), NULL, NULL)) < 0) {
            if ((subtopic = dds_find_topic(node_impl->ppant, subtopic_name.c_str())) < 0) {
                RMW_SET_ERROR_MSG("failed to create topic");
                goto fail_subtopic;
            }
        }
        if ((qos = dds_qos_create()) == nullptr) {
            goto fail_qos;
        }
        dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
        dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, DDS_LENGTH_UNLIMITED);
        if ((listeners = dds_listener_create(static_cast<void *>(sub))) == nullptr) {
            goto fail_listener;
        }
        dds_lset_data_available(listeners, subhandler);
        if ((pub->pubh = dds_create_writer(node_impl->ppant, pubtopic, qos, NULL)) < 0) {
            RMW_SET_ERROR_MSG("failed to create writer");
            goto fail_writer;
        }
        if ((sub->subh = dds_create_reader(node_impl->ppant, subtopic, qos, listeners)) < 0) {
            RMW_SET_ERROR_MSG("failed to create reader");
            goto fail_reader;
        }
        sub->node = node_impl;
        dds_qos_delete(qos);
        dds_listener_delete(listeners);

        if (dds_get_instance_handle(pub->pubh, &pub->pubiid) < 0) {
            RMW_SET_ERROR_MSG("failed to get instance handle for writer");
            goto fail_instance_handle;
        }
        node_impl->own_writers.insert(pub->pubiid);

        cs->pub = pub;
        cs->sub = sub;
        return RMW_RET_OK;

    fail_instance_handle:
        if (dds_delete(pub->pubh) < 0) {
            RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to destroy writer during error handling");
        }
    fail_reader:
        dds_delete(pub->pubh);
    fail_writer:
        dds_listener_delete(listeners);
    fail_listener:
        dds_qos_delete(qos);
    fail_qos:
        /* leak subtopic */
    fail_subtopic:
        /* leak pubtopic */
    fail_pubtopic:
        return RMW_RET_ERROR;
    }

    static void rmw_fini_cs(CddsCS *cs)
    {
        dds_delete(cs->sub->subh);
        dds_delete(cs->pub->pubh);
        cs->sub->node->own_writers.erase(cs->pub->pubiid);        
    }

    rmw_client_t *rmw_create_client(const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies)
    {
        CddsClient *info = new CddsClient();
        if (rmw_init_cs(&info->client, node, type_supports, service_name, qos_policies, false) != RMW_RET_OK) {
            delete(info);
            return nullptr;
        }
        rmw_client_t *rmw_client = rmw_client_allocate();
        RET_NULL_X(rmw_client, goto fail_client);
        rmw_client->implementation_identifier = adlink_cyclonedds_identifier;
        rmw_client->data = info;
        rmw_client->service_name = reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
        RET_NULL_X(rmw_client->service_name, goto fail_service_name);
        memcpy(const_cast<char *>(rmw_client->service_name), service_name, strlen(service_name) + 1);
        return rmw_client;
    fail_service_name:
        rmw_client_free(rmw_client);
    fail_client:
        rmw_fini_cs(&info->client);
        return nullptr;
    }

    rmw_ret_t rmw_destroy_client(rmw_node_t *node, rmw_client_t *client)
    {
        RET_WRONG_IMPLID(node);
        RET_WRONG_IMPLID(client);
        auto info = static_cast<CddsClient *>(client->data);
        rmw_fini_cs(&info->client);
        rmw_free(const_cast<char *>(client->service_name));
        rmw_client_free(client);
        return RMW_RET_OK;
    }

    rmw_service_t *rmw_create_service(const rmw_node_t *node, const rosidl_service_type_support_t *type_supports, const char *service_name, const rmw_qos_profile_t *qos_policies)
    {
        CddsService *info = new CddsService();
        if (rmw_init_cs(&info->service, node, type_supports, service_name, qos_policies, true) != RMW_RET_OK) {
            delete(info);
            return nullptr;
        }
        rmw_service_t *rmw_service = rmw_service_allocate();
        RET_NULL_X(rmw_service, goto fail_service);
        rmw_service->implementation_identifier = adlink_cyclonedds_identifier;
        rmw_service->data = info;
        rmw_service->service_name = reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
        RET_NULL_X(rmw_service->service_name, goto fail_service_name);
        memcpy(const_cast<char *>(rmw_service->service_name), service_name, strlen(service_name) + 1);
        return rmw_service;
    fail_service_name:
        rmw_service_free(rmw_service);
    fail_service:
        rmw_fini_cs(&info->service);
        return nullptr;
    }

    rmw_ret_t rmw_destroy_service(rmw_node_t *node, rmw_service_t *service)
    {
        RET_WRONG_IMPLID(node);
        RET_WRONG_IMPLID(service);
        auto info = static_cast<CddsService *>(service->data);
        rmw_fini_cs(&info->service);
        rmw_free(const_cast<char *>(service->service_name));
        rmw_service_free(service);
        return RMW_RET_OK;
    }

    /////////////////////////////////////////////////////////////////////////////////////////
    ///////////                                                                   ///////////
    ///////////    INTROSPECTION                                                  ///////////
    ///////////                                                                   ///////////
    /////////////////////////////////////////////////////////////////////////////////////////
  
    rmw_ret_t rmw_get_node_names(const rmw_node_t *node, rcutils_string_array_t *node_names)
    {
#if 0 // NIY
        RET_WRONG_IMPLID(node);
        if (rmw_check_zero_rmw_string_array(node_names) != RMW_RET_OK) {
            return RMW_RET_ERROR;
        }

        auto impl = static_cast<CddsNode *>(node->data);

        // FIXME: sorry, can't do it with current Zenoh
        auto participant_names = std::vector<std::string>{};
        rcutils_allocator_t allocator = rcutils_get_default_allocator();
        rcutils_ret_t rcutils_ret =
            rcutils_string_array_init(node_names, participant_names.size(), &allocator);
        if (rcutils_ret != RCUTILS_RET_OK) {
            RMW_SET_ERROR_MSG(rcutils_get_error_string_safe())
            return rmw_convert_rcutils_ret_to_rmw_ret(rcutils_ret);
        }
        for (size_t i = 0; i < participant_names.size(); ++i) {
            node_names->data[i] = rcutils_strdup(participant_names[i].c_str(), allocator);
            if (!node_names->data[i]) {
                RMW_SET_ERROR_MSG("failed to allocate memory for node name")
                    rcutils_ret = rcutils_string_array_fini(node_names);
                if (rcutils_ret != RCUTILS_RET_OK) {
                    RCUTILS_LOG_ERROR_NAMED(
                            "rmw_cyclonedds_cpp",
                            "failed to cleanup during error handling: %s", rcutils_get_error_string_safe())
                        }
                return RMW_RET_BAD_ALLOC;
            }
        }
        return RMW_RET_OK;
#else
        (void)node; (void)node_names;
        return RMW_RET_TIMEOUT;
#endif
    }

    rmw_ret_t rmw_get_topic_names_and_types(const rmw_node_t *node, rcutils_allocator_t *allocator, bool no_demangle, rmw_names_and_types_t *topic_names_and_types)
    {
#if 0 // NIY
        RET_NULL(allocator);
        RET_WRONG_IMPLID(node);
        rmw_ret_t ret = rmw_names_and_types_check_zero(topic_names_and_types);
        if (ret != RMW_RET_OK) {
            return ret;
        }

        auto impl = static_cast<CustomParticipantInfo *>(node->data);

        // Access the slave Listeners, which are the ones that have the topicnamesandtypes member
        // Get info from publisher and subscriber
        // Combined results from the two lists
        std::map<std::string, std::set<std::string>> topics;
        {
            ReaderInfo * slave_target = impl->secondarySubListener;
            slave_target->mapmutex.lock();
            for (auto it : slave_target->topicNtypes) {
                if (!no_demangle && _get_ros_prefix_if_exists(it.first) != ros_topic_prefix) {
                    // if we are demangling and this is not prefixed with rt/, skip it
                    continue;
                }
                for (auto & itt : it.second) {
                    topics[it.first].insert(itt);
                }
            }
            slave_target->mapmutex.unlock();
        }
        {
            WriterInfo * slave_target = impl->secondaryPubListener;
            slave_target->mapmutex.lock();
            for (auto it : slave_target->topicNtypes) {
                if (!no_demangle && _get_ros_prefix_if_exists(it.first) != ros_topic_prefix) {
                    // if we are demangling and this is not prefixed with rt/, skip it
                    continue;
                }
                for (auto & itt : it.second) {
                    topics[it.first].insert(itt);
                }
            }
            slave_target->mapmutex.unlock();
        }

        // Copy data to results handle
        if (topics.size() > 0) {
            // Setup string array to store names
            rmw_ret_t rmw_ret = rmw_names_and_types_init(topic_names_and_types, topics.size(), allocator);
            if (rmw_ret != RMW_RET_OK) {
                return rmw_ret;
            }
            // Setup cleanup function, in case of failure below
            auto fail_cleanup = [&topic_names_and_types]() {
                                    rmw_ret_t rmw_ret = rmw_names_and_types_fini(topic_names_and_types);
                                    if (rmw_ret != RMW_RET_OK) {
                                        RCUTILS_LOG_ERROR_NAMED(
                                                "rmw_cyclonedds_cpp",
                                                "error during report of error: %s", rmw_get_error_string_safe())
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
                char * topic_name = rcutils_strdup(demangle_topic(topic_n_types.first).c_str(), *allocator);
                if (!topic_name) {
                    RMW_SET_ERROR_MSG_ALLOC("failed to allocate memory for topic name", *allocator);
                    fail_cleanup();
                    return RMW_RET_BAD_ALLOC;
                }
                topic_names_and_types->names.data[index] = topic_name;
                // Setup storage for types
                {
                    rcutils_ret_t rcutils_ret = rcutils_string_array_init(
                            &topic_names_and_types->types[index],
                            topic_n_types.second.size(),
                            allocator);
                    if (rcutils_ret != RCUTILS_RET_OK) {
                        RMW_SET_ERROR_MSG(rcutils_get_error_string_safe())
                            fail_cleanup();
                        return rmw_convert_rcutils_ret_to_rmw_ret(rcutils_ret);
                    }
                }
                // Duplicate and store each type for the topic
                size_t type_index = 0;
                for (const auto & type : topic_n_types.second) {
                    char * type_name = rcutils_strdup(demangle_type(type).c_str(), *allocator);
                    if (!type_name) {
                        RMW_SET_ERROR_MSG_ALLOC("failed to allocate memory for type name", *allocator)
                            fail_cleanup();
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
        (void)node; (void)allocator; (void)no_demangle; (void)topic_names_and_types;
        return RMW_RET_TIMEOUT;
#endif
    }

    rmw_ret_t rmw_get_service_names_and_types(const rmw_node_t *node, rcutils_allocator_t *allocator, rmw_names_and_types_t *service_names_and_types)
    {
#if 0 // NIY
        if (!allocator) {
            RMW_SET_ERROR_MSG("allocator is null")
                return RMW_RET_INVALID_ARGUMENT;
        }
        if (!node) {
            RMW_SET_ERROR_MSG_ALLOC("null node handle", *allocator)
                return RMW_RET_INVALID_ARGUMENT;
        }
        rmw_ret_t ret = rmw_names_and_types_check_zero(service_names_and_types);
        if (ret != RMW_RET_OK) {
            return ret;
        }

        // Get participant pointer from node
        if (node->implementation_identifier != adlink_cyclonedds_identifier) {
            RMW_SET_ERROR_MSG_ALLOC("node handle not from this implementation", *allocator);
            return RMW_RET_ERROR;
        }

        auto impl = static_cast<CustomParticipantInfo *>(node->data);

        // Access the slave Listeners, which are the ones that have the topicnamesandtypes member
        // Get info from publisher and subscriber
        // Combined results from the two lists
        std::map<std::string, std::set<std::string>> services;
        {
            ReaderInfo * slave_target = impl->secondarySubListener;
            slave_target->mapmutex.lock();
            for (auto it : slave_target->topicNtypes) {
                std::string service_name = _demangle_service_from_topic(it.first);
                if (!service_name.length()) {
                    // not a service
                    continue;
                }
                for (auto & itt : it.second) {
                    std::string service_type = _demangle_service_type_only(itt);
                    if (service_type.length()) {
                        services[service_name].insert(service_type);
                    }
                }
            }
            slave_target->mapmutex.unlock();
        }
        {
            WriterInfo * slave_target = impl->secondaryPubListener;
            slave_target->mapmutex.lock();
            for (auto it : slave_target->topicNtypes) {
                std::string service_name = _demangle_service_from_topic(it.first);
                if (!service_name.length()) {
                    // not a service
                    continue;
                }
                for (auto & itt : it.second) {
                    std::string service_type = _demangle_service_type_only(itt);
                    if (service_type.length()) {
                        services[service_name].insert(service_type);
                    }
                }
            }
            slave_target->mapmutex.unlock();
        }

        // Fill out service_names_and_types
        if (services.size()) {
            // Setup string array to store names
            rmw_ret_t rmw_ret =
                rmw_names_and_types_init(service_names_and_types, services.size(), allocator);
            if (rmw_ret != RMW_RET_OK) {
                return rmw_ret;
            }
            // Setup cleanup function, in case of failure below
            auto fail_cleanup = [&service_names_and_types]() {
                                    rmw_ret_t rmw_ret = rmw_names_and_types_fini(service_names_and_types);
                                    if (rmw_ret != RMW_RET_OK) {
                                        RCUTILS_LOG_ERROR_NAMED(
                                                "rmw_cyclonedds_cpp",
                                                "error during report of error: %s", rmw_get_error_string_safe())
                                            }
                                };
            // For each service, store the name, initialize the string array for types, and store all types
            size_t index = 0;
            for (const auto & service_n_types : services) {
                // Duplicate and store the service_name
                char * service_name = rcutils_strdup(service_n_types.first.c_str(), *allocator);
                if (!service_name) {
                    RMW_SET_ERROR_MSG_ALLOC("failed to allocate memory for service name", *allocator);
                    fail_cleanup();
                    return RMW_RET_BAD_ALLOC;
                }
                service_names_and_types->names.data[index] = service_name;
                // Setup storage for types
                {
                    rcutils_ret_t rcutils_ret = rcutils_string_array_init(
                            &service_names_and_types->types[index],
                            service_n_types.second.size(),
                            allocator);
                    if (rcutils_ret != RCUTILS_RET_OK) {
                        RMW_SET_ERROR_MSG(rcutils_get_error_string_safe())
                            fail_cleanup();
                        return rmw_convert_rcutils_ret_to_rmw_ret(rcutils_ret);
                    }
                }
                // Duplicate and store each type for the service
                size_t type_index = 0;
                for (const auto & type : service_n_types.second) {
                    char * type_name = rcutils_strdup(type.c_str(), *allocator);
                    if (!type_name) {
                        RMW_SET_ERROR_MSG_ALLOC("failed to allocate memory for type name", *allocator)
                            fail_cleanup();
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
        (void)node; (void)allocator; (void)service_names_and_types;
        return RMW_RET_TIMEOUT;
#endif
    }

    rmw_ret_t rmw_service_server_is_available(const rmw_node_t * node, const rmw_client_t * client, bool * is_available)
    {
#if 0 // NIY
        if (!node) {
            RMW_SET_ERROR_MSG("node handle is null");
            return RMW_RET_ERROR;
        }

        RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
                node handle,
                node->implementation_identifier, adlink_cyclonedds_identifier,
                return RMW_RET_ERROR);

        if (!client) {
            RMW_SET_ERROR_MSG("client handle is null");
            return RMW_RET_ERROR;
        }

        if (!is_available) {
            RMW_SET_ERROR_MSG("is_available is null");
            return RMW_RET_ERROR;
        }

        auto client_info = static_cast<CustomClientInfo *>(client->data);
        if (!client_info) {
            RMW_SET_ERROR_MSG("client info handle is null");
            return RMW_RET_ERROR;
        }

        auto pub_topic_name =
            client_info->request_publisher_->getAttributes().topic.getTopicName();
        auto pub_partitions =
            client_info->request_publisher_->getAttributes().qos.m_partition.getNames();
        // every rostopic has exactly 1 partition field set
        if (pub_partitions.size() != 1) {
            RCUTILS_LOG_ERROR_NAMED(
                    "rmw_cyclonedds_cpp",
                    "Topic %s is not a ros topic", pub_topic_name.c_str())
                RMW_SET_ERROR_MSG((std::string(pub_topic_name) + " is a non-ros topic\n").c_str());
            return RMW_RET_ERROR;
        }
        auto pub_fqdn = pub_partitions[0] + "/" + pub_topic_name;
        pub_fqdn = _demangle_if_ros_topic(pub_fqdn);

        auto sub_topic_name =
            client_info->response_subscriber_->getAttributes().topic.getTopicName();
        auto sub_partitions =
            client_info->response_subscriber_->getAttributes().qos.m_partition.getNames();
        // every rostopic has exactly 1 partition field set
        if (sub_partitions.size() != 1) {
            RCUTILS_LOG_ERROR_NAMED(
                    "rmw_cyclonedds_cpp",
                    "Topic %s is not a ros topic", pub_topic_name.c_str())
                RMW_SET_ERROR_MSG((std::string(sub_topic_name) + " is a non-ros topic\n").c_str());
            return RMW_RET_ERROR;
        }
        auto sub_fqdn = sub_partitions[0] + "/" + sub_topic_name;
        sub_fqdn = _demangle_if_ros_topic(sub_fqdn);

        *is_available = false;
        size_t number_of_request_subscribers = 0;
        rmw_ret_t ret = rmw_count_subscribers(
                node,
                pub_fqdn.c_str(),
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
        ret = rmw_count_publishers(
                node,
                sub_fqdn.c_str(),
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
        (void)node; (void)client; (void)is_available;
        return RMW_RET_TIMEOUT;
#endif
    }
    
    rmw_ret_t rmw_count_publishers(const rmw_node_t *node, const char *topic_name, size_t *count)
    {
#if 0
        // safechecks

        if (!node) {
            RMW_SET_ERROR_MSG("null node handle");
            return RMW_RET_ERROR;
        }
        // Get participant pointer from node
        if (node->implementation_identifier != eprosima_fastrtps_identifier) {
            RMW_SET_ERROR_MSG("node handle not from this implementation");
            return RMW_RET_ERROR;
        }

        auto impl = static_cast<CustomParticipantInfo *>(node->data);

        WriterInfo * slave_target = impl->secondaryPubListener;
        slave_target->mapmutex.lock();
        *count = 0;
        for (auto it : slave_target->topicNtypes) {
            auto topic_fqdn = _demangle_if_ros_topic(it.first);
            if (topic_fqdn == topic_name) {
                *count += it.second.size();
            }
        }
        slave_target->mapmutex.unlock();

        RCUTILS_LOG_DEBUG_NAMED(
                "rmw_fastrtps_cpp",
                "looking for subscriber topic: %s, number of matches: %zu",
                topic_name, *count)

        return RMW_RET_OK;
#else
        (void)node; (void)topic_name; (void)count;
        return RMW_RET_TIMEOUT;
#endif
    }

    rmw_ret_t rmw_count_subscribers(const rmw_node_t *node, const char *topic_name, size_t *count)
    {
#if 0
        // safechecks

        if (!node) {
            RMW_SET_ERROR_MSG("null node handle");
            return RMW_RET_ERROR;
        }
        // Get participant pointer from node
        if (node->implementation_identifier != eprosima_fastrtps_identifier) {
            RMW_SET_ERROR_MSG("node handle not from this implementation");
            return RMW_RET_ERROR;
        }

        CustomParticipantInfo * impl = static_cast<CustomParticipantInfo *>(node->data);

        ReaderInfo * slave_target = impl->secondarySubListener;
        *count = 0;
        slave_target->mapmutex.lock();
        for (auto it : slave_target->topicNtypes) {
            auto topic_fqdn = _demangle_if_ros_topic(it.first);
            if (topic_fqdn == topic_name) {
                *count += it.second.size();
            }
        }
        slave_target->mapmutex.unlock();

        RCUTILS_LOG_DEBUG_NAMED(
                "rmw_fastrtps_cpp",
                "looking for subscriber topic: %s, number of matches: %zu",
                topic_name, *count)

        return RMW_RET_OK;
#else
        (void)node; (void)topic_name; (void)count;
        return RMW_RET_TIMEOUT;
#endif
    }
}  // extern "C"
