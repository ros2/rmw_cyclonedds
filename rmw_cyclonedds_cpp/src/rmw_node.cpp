// Copyright 2019 ADLINK Technology Limited.
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

#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <map>
#include <set>
#include <functional>
#include <atomic>
#include <memory>
#include <vector>
#include <string>
#include <tuple>
#include <utility>
#include <regex>
#include <limits>

#include "rcutils/filesystem.h"
#include "rcutils/format_string.h"
#include "rcutils/get_env.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"

#include "rmw/allocators.h"
#include "rmw/convert_rcutils_ret_to_rmw_ret.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/get_node_info_and_types.h"
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h"
#include "rmw/validate_node_name.h"

#include "fallthrough_macro.hpp"
#include "Serialization.hpp"
#include "rmw/impl/cpp/macros.hpp"
#include "rmw/impl/cpp/key_value.hpp"

#include "TypeSupport2.hpp"

#include "rmw_cyclonedds_cpp/rmw_version_test.hpp"
#include "rmw_cyclonedds_cpp/MessageTypeSupport.hpp"
#include "rmw_cyclonedds_cpp/ServiceTypeSupport.hpp"

#include "rmw/get_topic_endpoint_info.h"
#include "rmw/incompatible_qos_events_statuses.h"
#include "rmw/topic_endpoint_info_array.h"

#include "rmw_dds_common/context.hpp"
#include "rmw_dds_common/graph_cache.hpp"
#include "rmw_dds_common/msg/participant_entities_info.hpp"

#include "rosidl_typesupport_cpp/message_type_support.hpp"

#include "namespace_prefix.hpp"

#include "dds/dds.h"
#include "dds/ddsi/ddsi_sertopic.h"
#include "rmw_cyclonedds_cpp/serdes.hpp"
#include "serdata.hpp"
#include "demangle.hpp"

/* Security must be enabled when compiling and requires cyclone to support QOS property lists */
#if DDS_HAS_SECURITY && DDS_HAS_PROPERTY_LIST_QOS
#define RMW_SUPPORT_SECURITY 1
#else
#define RMW_SUPPORT_SECURITY 0
#endif

/* Set to > 0 for printing warnings to stderr for each messages that was taken more than this many
   ms after writing */
#define REPORT_LATE_MESSAGES 0

/* Set to != 0 for periodically printing requests that have been blocked for more than 1s */
#define REPORT_BLOCKED_REQUESTS 0

#define RET_ERR_X(msg, code) do {RMW_SET_ERROR_MSG(msg); code;} while (0)
#define RET_NULL_X(var, code) do {if (!var) {RET_ERR_X(#var " is null", code);}} while (0)
#define RET_ALLOC_X(var, code) do {if (!var) {RET_ERR_X("failed to allocate " #var, code);} \
} while (0)
#define RET_WRONG_IMPLID_X(var, code) do { \
    RET_NULL_X(var, code); \
    if ((var)->implementation_identifier != eclipse_cyclonedds_identifier) { \
      RET_ERR_X(#var " not from this implementation", code); \
    } \
} while (0)
#define RET_NULL_OR_EMPTYSTR_X(var, code) do { \
    if (!var || strlen(var) == 0) { \
      RET_ERR_X(#var " is null or empty string", code); \
    } \
} while (0)
#define RET_ERR(msg) RET_ERR_X(msg, return RMW_RET_ERROR)
#define RET_NULL(var) RET_NULL_X(var, return RMW_RET_ERROR)
#define RET_ALLOC(var) RET_ALLOC_X(var, return RMW_RET_ERROR)
#define RET_WRONG_IMPLID(var) RET_WRONG_IMPLID_X(var, return RMW_RET_ERROR)
#define RET_NULL_OR_EMPTYSTR(var) RET_NULL_OR_EMPTYSTR_X(var, return RMW_RET_ERROR)

using rmw_dds_common::msg::ParticipantEntitiesInfo;

const char * const eclipse_cyclonedds_identifier = "rmw_cyclonedds_cpp";
const char * const eclipse_cyclonedds_serialization_format = "cdr";

/* instance handles are unsigned 64-bit integers carefully constructed to be as close to uniformly
   distributed as possible for no other reason than making them near-perfect hash keys, hence we can
   improve over the default hash function */
struct dds_instance_handle_hash
{
public:
  std::size_t operator()(dds_instance_handle_t const & x) const noexcept
  {
    return static_cast<std::size_t>(x);
  }
};

bool operator<(dds_builtintopic_guid_t const & a, dds_builtintopic_guid_t const & b)
{
  return memcmp(&a, &b, sizeof(dds_builtintopic_guid_t)) < 0;
}

static rmw_ret_t discovery_thread_stop(rmw_dds_common::Context & context);
static bool dds_qos_to_rmw_qos(const dds_qos_t * dds_qos, rmw_qos_profile_t * qos_policies);

static rmw_publisher_t * create_publisher(
  dds_entity_t dds_ppant, dds_entity_t dds_pub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options
);
static rmw_ret_t destroy_publisher(rmw_publisher_t * publisher);

static rmw_subscription_t * create_subscription(
  dds_entity_t dds_ppant, dds_entity_t dds_pub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_subscription_options_t * subscription_options
);
static rmw_ret_t destroy_subscription(rmw_subscription_t * subscription);

static rmw_guard_condition_t * create_guard_condition();
static rmw_ret_t destroy_guard_condition(rmw_guard_condition_t * gc);

struct CddsDomain;
struct CddsWaitset;

struct Cdds
{
  std::mutex lock;

  /* Map of domain id to per-domain state, used by create/destroy node */
  std::mutex domains_lock;
  std::map<dds_domainid_t, CddsDomain> domains;

  /* special guard condition that gets attached to every waitset but that is never triggered:
     this way, we can avoid Cyclone's behaviour of always returning immediately when no
     entities are attached to a waitset */
  dds_entity_t gc_for_empty_waitset;

  /* set of waitsets protected by lock, used to invalidate all waitsets caches when an entity is
     deleted */
  std::unordered_set<CddsWaitset *> waitsets;

  Cdds()
  : gc_for_empty_waitset(0)
  {}
};

static Cdds gcdds;

struct CddsEntity
{
  dds_entity_t enth;
};

#if RMW_SUPPORT_SECURITY
struct dds_security_files_t
{
  char * identity_ca_cert = nullptr;
  char * cert = nullptr;
  char * key = nullptr;
  char * permissions_ca_cert = nullptr;
  char * governance_p7s = nullptr;
  char * permissions_p7s = nullptr;
};
#endif

struct CddsDomain
{
  /* This RMW implementation currently implements localhost-only by explicitly creating
     domains with a configuration that consists of: (1) a hard-coded selection of
     "localhost" as the network interface address; (2) followed by the contents of the
     CYCLONEDDS_URI environment variable:

     - the "localhost" hostname should resolve to 127.0.0.1 (or equivalent) for IPv4 and
       to ::1 for IPv6, so we don't have to worry about which of IPv4 or IPv6 is used (as
       would be the case with a numerical IP address), nor do we have to worry about the
       name of the loopback interface;

     - if the machine's configuration doesn't properly resolve "localhost", you can still
       override via $CYCLONEDDS_URI.

     The CddsDomain type is used to track which domains exist and how many nodes are in
     it.  Because the domain is instantiated with the first nodes created in that domain,
     the other nodes must have the same localhost-only setting.  (It bugs out if not.)
     Everything resets automatically when the last node in the domain is deleted.

     (It might be better still to for Cyclone to provide "loopback" or something similar
     as a generic alias for a loopback interface ...)

     There are a few issues with the current support for creating domains explicitly in
     Cyclone, fixing those might relax alter or relax some of the above. */

  bool localhost_only;
  uint32_t refcount;

  /* handle of the domain entity */
  dds_entity_t domain_handle;

  /* Default constructor so operator[] can be safely be used to look one up */
  CddsDomain()
  : localhost_only(false), refcount(0), domain_handle(0)
  {}

  ~CddsDomain()
  {}
};

struct rmw_context_impl_t
{
  rmw_dds_common::Context common;
  dds_domainid_t domain_id;
  dds_entity_t ppant;

  /* handles for built-in topic readers */
  dds_entity_t rd_participant;
  dds_entity_t rd_subscription;
  dds_entity_t rd_publication;

  /* DDS publisher, subscriber used for ROS2 publishers and subscriptions */
  dds_entity_t dds_pub;
  dds_entity_t dds_sub;

  /* Participant reference count*/
  size_t node_count{0};
  std::mutex initialization_mutex;

  rmw_context_impl_t()
  : common(), domain_id(UINT32_MAX), ppant(0)
  {
    /* destructor relies on these being initialized properly */
    common.thread_is_running.store(false);
    common.graph_guard_condition = nullptr;
    common.pub = nullptr;
    common.sub = nullptr;
  }

  // Initializes the participant, if it wasn't done already.
  // node_count is increased
  rmw_ret_t
  init(rmw_init_options_t * options);

  // Destroys the participant, when node_count reaches 0.
  rmw_ret_t
  fini();

  ~rmw_context_impl_t()
  {
    if (0u != this->node_count) {
      RCUTILS_SAFE_FWRITE_TO_STDERR(
        "Not all nodes were finished before finishing the context\n."
        "Ensure `rcl_node_fini` is called for all nodes before `rcl_context_fini`,"
        "to avoid leaking.\n");
    }
  }

private:
  void
  clean_up();
};

struct CddsNode
{
};

struct CddsPublisher : CddsEntity
{
  dds_instance_handle_t pubiid;
  rmw_gid_t gid;
  struct ddsi_sertopic * sertopic;
};

struct CddsSubscription : CddsEntity
{
  rmw_gid_t gid;
  dds_entity_t rdcondh;
};

struct CddsCS
{
  CddsPublisher * pub;
  CddsSubscription * sub;
};

struct CddsClient
{
  CddsCS client;

#if REPORT_BLOCKED_REQUESTS
  std::mutex lock;
  dds_time_t lastcheck;
  std::map<int64_t, dds_time_t> reqtime;
#endif
};

struct CddsService
{
  CddsCS service;
};

struct CddsGuardCondition
{
  dds_entity_t gcondh;
};

struct CddsEvent : CddsEntity
{
  rmw_event_type_t event_type;
};

struct CddsWaitset
{
  dds_entity_t waitseth;

  std::vector<dds_attach_t> trigs;
  size_t nelems;

  std::mutex lock;
  bool inuse;
  std::vector<CddsSubscription *> subs;
  std::vector<CddsGuardCondition *> gcs;
  std::vector<CddsClient *> cls;
  std::vector<CddsService *> srvs;
  std::vector<CddsEvent> evs;
};

static void clean_waitset_caches();
#if REPORT_BLOCKED_REQUESTS
static void check_for_blocked_requests(CddsClient & client);
#endif

#ifndef WIN32
/* TODO(allenh1): check for Clang */
#pragma GCC visibility push (default)
#endif

extern "C" const char * rmw_get_implementation_identifier()
{
  return eclipse_cyclonedds_identifier;
}

extern "C" const char * rmw_get_serialization_format()
{
  return eclipse_cyclonedds_serialization_format;
}

extern "C" rmw_ret_t rmw_set_log_severity(rmw_log_severity_t severity)
{
  uint32_t mask = 0;
  switch (severity) {
    default:
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("%s: Invalid log severity '%d'", __func__, severity);
      return RMW_RET_INVALID_ARGUMENT;
    case RMW_LOG_SEVERITY_DEBUG:
      mask |= DDS_LC_DISCOVERY | DDS_LC_THROTTLE | DDS_LC_CONFIG;
      FALLTHROUGH;
    case RMW_LOG_SEVERITY_INFO:
      mask |= DDS_LC_INFO;
      FALLTHROUGH;
    case RMW_LOG_SEVERITY_WARN:
      mask |= DDS_LC_WARNING;
      FALLTHROUGH;
    case RMW_LOG_SEVERITY_ERROR:
      mask |= DDS_LC_ERROR;
      FALLTHROUGH;
    case RMW_LOG_SEVERITY_FATAL:
      mask |= DDS_LC_FATAL;
  }
  dds_set_log_mask(mask);
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init_options_init(
  rmw_init_options_t * init_options,
  rcutils_allocator_t allocator)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ALLOCATOR(&allocator, return RMW_RET_INVALID_ARGUMENT);
  if (NULL != init_options->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }
  init_options->instance_id = 0;
  init_options->implementation_identifier = eclipse_cyclonedds_identifier;
  init_options->allocator = allocator;
  init_options->impl = nullptr;
  init_options->localhost_only = RMW_LOCALHOST_ONLY_DEFAULT;
  init_options->domain_id = RMW_DEFAULT_DOMAIN_ID;
  init_options->enclave = NULL;
  init_options->security_options = rmw_get_zero_initialized_security_options();
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init_options_copy(const rmw_init_options_t * src, rmw_init_options_t * dst)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(src, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(dst, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    src,
    src->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (NULL != dst->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized dst");
    return RMW_RET_INVALID_ARGUMENT;
  }

  const rcutils_allocator_t * allocator = &src->allocator;
  rmw_ret_t ret = RMW_RET_OK;

  allocator->deallocate(dst->enclave, allocator->state);
  *dst = *src;
  dst->enclave = NULL;
  dst->security_options = rmw_get_zero_initialized_security_options();

  dst->enclave = rcutils_strdup(src->enclave, *allocator);
  if (src->enclave && !dst->enclave) {
    ret = RMW_RET_BAD_ALLOC;
    goto fail;
  }
  return rmw_security_options_copy(&src->security_options, allocator, &dst->security_options);
fail:
  allocator->deallocate(dst->enclave, allocator->state);
  return ret;
}

extern "C" rmw_ret_t rmw_init_options_fini(rmw_init_options_t * init_options)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
  rcutils_allocator_t & allocator = init_options->allocator;
  RCUTILS_CHECK_ALLOCATOR(&allocator, return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    init_options,
    init_options->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  allocator.deallocate(init_options->enclave, allocator.state);
  rmw_security_options_fini(&init_options->security_options, &allocator);
  *init_options = rmw_get_zero_initialized_init_options();
  return RMW_RET_OK;
}

static void convert_guid_to_gid(const dds_guid_t & guid, rmw_gid_t & gid)
{
  static_assert(
    RMW_GID_STORAGE_SIZE >= sizeof(guid),
    "rmw_gid_t type too small for a Cyclone DDS GUID");
  memset(&gid, 0, sizeof(gid));
  gid.implementation_identifier = eclipse_cyclonedds_identifier;
  memcpy(gid.data, guid.v, sizeof(guid));
}

static void get_entity_gid(dds_entity_t h, rmw_gid_t & gid)
{
  dds_guid_t guid;
  dds_get_guid(h, &guid);
  convert_guid_to_gid(guid, gid);
}

static void handle_ParticipantEntitiesInfo(dds_entity_t reader, void * arg)
{
  static_cast<void>(reader);
  rmw_context_impl_t * impl = static_cast<rmw_context_impl_t *>(arg);
  ParticipantEntitiesInfo msg;
  bool taken;
  while (rmw_take(impl->common.sub, &msg, &taken, nullptr) == RMW_RET_OK && taken) {
    // locally published data is filtered because of the subscription QoS
    impl->common.graph_cache.update_participant_entities(msg);
  }
}

static void handle_DCPSParticipant(dds_entity_t reader, void * arg)
{
  rmw_context_impl_t * impl = static_cast<rmw_context_impl_t *>(arg);
  dds_sample_info_t si;
  void * raw = NULL;
  while (dds_take(reader, &raw, &si, 1, 1) == 1) {
    auto s = static_cast<const dds_builtintopic_participant_t *>(raw);
    rmw_gid_t gid;
    convert_guid_to_gid(s->key, gid);
    if (memcmp(&gid, &impl->common.gid, sizeof(gid)) == 0) {
      // ignore the local participant
    } else if (si.instance_state != DDS_ALIVE_INSTANCE_STATE) {
      impl->common.graph_cache.remove_participant(gid);
    } else if (si.valid_data) {
      void * ud;
      size_t udsz;
      if (dds_qget_userdata(s->qos, &ud, &udsz)) {
        std::vector<uint8_t> udvec(static_cast<uint8_t *>(ud), static_cast<uint8_t *>(ud) + udsz);
        dds_free(ud);
        auto map = rmw::impl::cpp::parse_key_value(udvec);
        auto name_found = map.find("enclave");
        if (name_found != map.end()) {
          auto enclave =
            std::string(name_found->second.begin(), name_found->second.end());
          impl->common.graph_cache.add_participant(gid, enclave);
        }
      }
    }
    dds_return_loan(reader, &raw, 1);
  }
}

static void handle_builtintopic_endpoint(
  dds_entity_t reader, rmw_context_impl_t * impl,
  bool is_reader)
{
  dds_sample_info_t si;
  void * raw = NULL;
  while (dds_take(reader, &raw, &si, 1, 1) == 1) {
    auto s = static_cast<const dds_builtintopic_endpoint_t *>(raw);
    rmw_gid_t gid;
    convert_guid_to_gid(s->key, gid);
    if (si.instance_state != DDS_ALIVE_INSTANCE_STATE) {
      impl->common.graph_cache.remove_entity(gid, is_reader);
    } else if (si.valid_data && strncmp(s->topic_name, "DCPS", 4) != 0) {
      rmw_qos_profile_t qos_profile = rmw_qos_profile_unknown;
      rmw_gid_t ppgid;
      dds_qos_to_rmw_qos(s->qos, &qos_profile);
      convert_guid_to_gid(s->participant_key, ppgid);
      impl->common.graph_cache.add_entity(
        gid,
        std::string(s->topic_name),
        std::string(s->type_name),
        ppgid,
        qos_profile,
        is_reader);
    }
    dds_return_loan(reader, &raw, 1);
  }
}

static void handle_DCPSSubscription(dds_entity_t reader, void * arg)
{
  rmw_context_impl_t * impl = static_cast<rmw_context_impl_t *>(arg);
  handle_builtintopic_endpoint(reader, impl, true);
}

static void handle_DCPSPublication(dds_entity_t reader, void * arg)
{
  rmw_context_impl_t * impl = static_cast<rmw_context_impl_t *>(arg);
  handle_builtintopic_endpoint(reader, impl, false);
}

static void discovery_thread(rmw_context_impl_t * impl)
{
  const CddsSubscription * sub = static_cast<const CddsSubscription *>(impl->common.sub->data);
  const CddsGuardCondition * gc =
    static_cast<const CddsGuardCondition *>(impl->common.listener_thread_gc->data);
  dds_entity_t ws;
  /* deleting ppant will delete waitset as well, so there is no real need to delete
     the waitset here on error, but it is more hygienic */
  if ((ws = dds_create_waitset(DDS_CYCLONEDDS_HANDLE)) < 0) {
    RCUTILS_SAFE_FWRITE_TO_STDERR(
      "ros discovery info listener thread: failed to create waitset, will shutdown ...\n");
    return;
  }
  /* I suppose I could attach lambda functions one way or another, which would
     definitely be more elegant, but this avoids having to deal with the C++
     freakishness that is involved and works, too. */
  std::vector<std::pair<dds_entity_t,
    std::function<void(dds_entity_t, rmw_context_impl_t *)>>> entries = {
    {gc->gcondh, nullptr},
    {sub->enth, handle_ParticipantEntitiesInfo},
    {impl->rd_participant, handle_DCPSParticipant},
    {impl->rd_subscription, handle_DCPSSubscription},
    {impl->rd_publication, handle_DCPSPublication},
  };
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].second != nullptr &&
      dds_set_status_mask(entries[i].first, DDS_DATA_AVAILABLE_STATUS) < 0)
    {
      RCUTILS_SAFE_FWRITE_TO_STDERR(
        "ros discovery info listener thread: failed to set reader status masks, "
        "will shutdown ...\n");
      return;
    }
    if (dds_waitset_attach(ws, entries[i].first, static_cast<dds_attach_t>(i)) < 0) {
      RCUTILS_SAFE_FWRITE_TO_STDERR(
        "ros discovery info listener thread: failed to attach entities to waitset, "
        "will shutdown ...\n");
      dds_delete(ws);
      return;
    }
  }
  std::vector<dds_attach_t> xs(5);
  while (impl->common.thread_is_running.load()) {
    dds_return_t n;
    if ((n = dds_waitset_wait(ws, xs.data(), xs.size(), DDS_INFINITY)) < 0) {
      RCUTILS_SAFE_FWRITE_TO_STDERR(
        "ros discovery info listener thread: wait failed, will shutdown ...\n");
      return;
    }
    for (int32_t i = 0; i < n; i++) {
      if (entries[xs[i]].second) {
        entries[xs[i]].second(entries[xs[i]].first, impl);
      }
    }
  }
  dds_delete(ws);
}

static rmw_ret_t discovery_thread_start(rmw_context_impl_t * impl)
{
  auto common_context = &impl->common;
  common_context->thread_is_running.store(true);
  common_context->listener_thread_gc = create_guard_condition();
  if (common_context->listener_thread_gc) {
    try {
      common_context->listener_thread = std::thread(discovery_thread, impl);
      return RMW_RET_OK;
    } catch (const std::exception & exc) {
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("Failed to create std::thread: %s", exc.what());
    } catch (...) {
      RMW_SET_ERROR_MSG("Failed to create std::thread");
    }
  } else {
    RMW_SET_ERROR_MSG("Failed to create guard condition");
  }
  common_context->thread_is_running.store(false);
  if (common_context->listener_thread_gc) {
    if (RMW_RET_OK != destroy_guard_condition(common_context->listener_thread_gc)) {
      RCUTILS_SAFE_FWRITE_TO_STDERR(
        RCUTILS_STRINGIFY(__FILE__) ":" RCUTILS_STRINGIFY(__function__) ":"
        RCUTILS_STRINGIFY(__LINE__) ": Failed to destroy guard condition");
    }
  }
  return RMW_RET_ERROR;
}

static rmw_ret_t discovery_thread_stop(rmw_dds_common::Context & common_context)
{
  if (common_context.thread_is_running.exchange(false)) {
    rmw_ret_t rmw_ret = rmw_trigger_guard_condition(common_context.listener_thread_gc);
    if (RMW_RET_OK != rmw_ret) {
      return rmw_ret;
    }
    try {
      common_context.listener_thread.join();
    } catch (const std::exception & exc) {
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("Failed to join std::thread: %s", exc.what());
      return RMW_RET_ERROR;
    } catch (...) {
      RMW_SET_ERROR_MSG("Failed to join std::thread");
      return RMW_RET_ERROR;
    }
    rmw_ret = destroy_guard_condition(common_context.listener_thread_gc);
    if (RMW_RET_OK != rmw_ret) {
      return rmw_ret;
    }
  }
  return RMW_RET_OK;
}

static bool check_create_domain(dds_domainid_t did, rmw_localhost_only_t localhost_only_option)
{
  const bool localhost_only = (localhost_only_option == RMW_LOCALHOST_ONLY_ENABLED);
  std::lock_guard<std::mutex> lock(gcdds.domains_lock);
  /* return true: n_nodes incremented, localhost_only set correctly, domain exists
     "      false: n_nodes unchanged, domain left intact if it already existed */
  CddsDomain & dom = gcdds.domains[did];
  if (dom.refcount != 0) {
    /* Localhost setting must match */
    if (localhost_only == dom.localhost_only) {
      dom.refcount++;
      return true;
    } else {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "rmw_create_node: attempt at creating localhost-only and non-localhost-only nodes "
        "in the same domain");
      return false;
    }
  } else {
    dom.refcount = 1;
    dom.localhost_only = localhost_only;

    /* Localhost-only: set network interface address (shortened form of config would be
       possible, too, but I think it is clearer to spell it out completely).  Empty
       configuration fragments are ignored, so it is safe to unconditionally append a
       comma. */
    std::string config =
      localhost_only ?
      "<CycloneDDS><Domain><General><NetworkInterfaceAddress>localhost</NetworkInterfaceAddress>"
      "</General></Domain></CycloneDDS>,"
      :
      "";

    /* Emulate default behaviour of Cyclone of reading CYCLONEDDS_URI */
    const char * get_env_error;
    const char * config_from_env;
    if ((get_env_error = rcutils_get_env("CYCLONEDDS_URI", &config_from_env)) == nullptr) {
      config += std::string(config_from_env);
    } else {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "rmw_create_node: failed to retrieve CYCLONEDDS_URI environment variable, error %s",
        get_env_error);
      gcdds.domains.erase(did);
      return false;
    }

    if ((dom.domain_handle = dds_create_domain(did, config.c_str())) < 0) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "rmw_create_node: failed to create domain, error %s", dds_strretcode(dom.domain_handle));
      gcdds.domains.erase(did);
      return false;
    } else {
      return true;
    }
  }
}

static
void
check_destroy_domain(dds_domainid_t domain_id)
{
  if (domain_id != UINT32_MAX) {
    std::lock_guard<std::mutex> lock(gcdds.domains_lock);
    CddsDomain & dom = gcdds.domains[domain_id];
    assert(dom.refcount > 0);
    if (--dom.refcount == 0) {
      static_cast<void>(dds_delete(dom.domain_handle));
      gcdds.domains.erase(domain_id);
    }
  }
}

#if RMW_SUPPORT_SECURITY
/*  Returns the full URI of a security file properly formatted for DDS  */
bool get_security_file_URI(
  char ** security_file, const char * security_filename, const char * node_secure_root,
  const rcutils_allocator_t allocator)
{
  *security_file = nullptr;
  char * file_path = rcutils_join_path(node_secure_root, security_filename, allocator);
  if (file_path != nullptr) {
    if (rcutils_is_readable(file_path)) {
      /*  Cyclone also supports a "data:" URI  */
      *security_file = rcutils_format_string(allocator, "file:%s", file_path);
      allocator.deallocate(file_path, allocator.state);
    } else {
      RCUTILS_LOG_INFO_NAMED(
        "rmw_cyclonedds_cpp", "get_security_file_URI: %s not found", file_path);
      allocator.deallocate(file_path, allocator.state);
    }
  }
  return *security_file != nullptr;
}

bool get_security_file_URIs(
  const rmw_security_options_t * security_options,
  dds_security_files_t & dds_security_files, rcutils_allocator_t allocator)
{
  bool ret = false;

  if (security_options->security_root_path != nullptr) {
    ret = (
      get_security_file_URI(
        &dds_security_files.identity_ca_cert, "identity_ca.cert.pem",
        security_options->security_root_path, allocator) &&
      get_security_file_URI(
        &dds_security_files.cert, "cert.pem",
        security_options->security_root_path, allocator) &&
      get_security_file_URI(
        &dds_security_files.key, "key.pem",
        security_options->security_root_path, allocator) &&
      get_security_file_URI(
        &dds_security_files.permissions_ca_cert, "permissions_ca.cert.pem",
        security_options->security_root_path, allocator) &&
      get_security_file_URI(
        &dds_security_files.governance_p7s, "governance.p7s",
        security_options->security_root_path, allocator) &&
      get_security_file_URI(
        &dds_security_files.permissions_p7s, "permissions.p7s",
        security_options->security_root_path, allocator));
  }
  return ret;
}

void finalize_security_file_URIs(
  dds_security_files_t dds_security_files, const rcutils_allocator_t allocator)
{
  allocator.deallocate(dds_security_files.identity_ca_cert, allocator.state);
  dds_security_files.identity_ca_cert = nullptr;
  allocator.deallocate(dds_security_files.cert, allocator.state);
  dds_security_files.cert = nullptr;
  allocator.deallocate(dds_security_files.key, allocator.state);
  dds_security_files.key = nullptr;
  allocator.deallocate(dds_security_files.permissions_ca_cert, allocator.state);
  dds_security_files.permissions_ca_cert = nullptr;
  allocator.deallocate(dds_security_files.governance_p7s, allocator.state);
  dds_security_files.governance_p7s = nullptr;
  allocator.deallocate(dds_security_files.permissions_p7s, allocator.state);
  dds_security_files.permissions_p7s = nullptr;
}
#endif  /* RMW_SUPPORT_SECURITY */

/* Attempt to set all the qos properties needed to enable DDS security */
static
rmw_ret_t configure_qos_for_security(
  dds_qos_t * qos,
  const rmw_security_options_t * security_options)
{
#if RMW_SUPPORT_SECURITY
  rmw_ret_t ret = RMW_RET_UNSUPPORTED;
  dds_security_files_t dds_security_files;
  rcutils_allocator_t allocator = rcutils_get_default_allocator();

  if (get_security_file_URIs(security_options, dds_security_files, allocator)) {
    dds_qset_prop(qos, "dds.sec.auth.identity_ca", dds_security_files.identity_ca_cert);
    dds_qset_prop(qos, "dds.sec.auth.identity_certificate", dds_security_files.cert);
    dds_qset_prop(qos, "dds.sec.auth.private_key", dds_security_files.key);
    dds_qset_prop(qos, "dds.sec.access.permissions_ca", dds_security_files.permissions_ca_cert);
    dds_qset_prop(qos, "dds.sec.access.governance", dds_security_files.governance_p7s);
    dds_qset_prop(qos, "dds.sec.access.permissions", dds_security_files.permissions_p7s);

    dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
    dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
    dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");

    dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
    dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");

    dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
    dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
    dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");

    ret = RMW_RET_OK;
  }
  finalize_security_file_URIs(dds_security_files, allocator);
  return ret;
#else
  (void) qos;
  if (security_options->enforce_security == RMW_SECURITY_ENFORCEMENT_ENFORCE) {
    RMW_SET_ERROR_MSG(
      "Security was requested but the Cyclone DDS being used does not have security "
      "support enabled. Recompile Cyclone DDS with the '-DENABLE_SECURITY=ON' "
      "CMake option");
  }
  return RMW_RET_UNSUPPORTED;
#endif
}

rmw_ret_t
rmw_context_impl_t::init(rmw_init_options_t * options)
{
  std::lock_guard<std::mutex> guard(initialization_mutex);
  if (0u != this->node_count) {
    // initialization has already been done
    this->node_count++;
    return RMW_RET_OK;
  }

  /* Take domains_lock and hold it until after the participant creation succeeded or
    failed: otherwise there is a race with rmw_destroy_node deleting the last participant
    and tearing down the domain for versions of Cyclone that implement the original
    version of dds_create_domain that doesn't return a handle.  */
  this->domain_id = static_cast<dds_domainid_t>(options->domain_id);
  if (!check_create_domain(this->domain_id, options->localhost_only)) {
    return RMW_RET_ERROR;
  }

  std::unique_ptr<dds_qos_t, std::function<void(dds_qos_t *)>>
  ppant_qos(dds_create_qos(), &dds_delete_qos);
  if (ppant_qos == nullptr) {
    this->clean_up();
    return RMW_RET_BAD_ALLOC;
  }
  std::string user_data = std::string("enclave=") + std::string(
    options->enclave) + std::string(";");
  dds_qset_userdata(ppant_qos.get(), user_data.c_str(), user_data.size());
  if (configure_qos_for_security(
      ppant_qos.get(),
      &options->security_options) != RMW_RET_OK)
  {
    if (RMW_SECURITY_ENFORCEMENT_ENFORCE == options->security_options.enforce_security) {
      this->clean_up();
      return RMW_RET_ERROR;
    }
  }

  this->ppant = dds_create_participant(this->domain_id, ppant_qos.get(), nullptr);
  if (this->ppant < 0) {
    this->clean_up();
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DDS participant");
    return RMW_RET_ERROR;
  }

  /* Create readers for DDS built-in topics for monitoring discovery */
  if ((this->rd_participant =
    dds_create_reader(this->ppant, DDS_BUILTIN_TOPIC_DCPSPARTICIPANT, nullptr, nullptr)) < 0)
  {
    this->clean_up();
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DCPSParticipant reader");
    return RMW_RET_ERROR;
  }
  if ((this->rd_subscription =
    dds_create_reader(this->ppant, DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION, nullptr, nullptr)) < 0)
  {
    this->clean_up();
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DCPSSubscription reader");
    return RMW_RET_ERROR;
  }
  if ((this->rd_publication =
    dds_create_reader(this->ppant, DDS_BUILTIN_TOPIC_DCPSPUBLICATION, nullptr, nullptr)) < 0)
  {
    this->clean_up();
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DCPSPublication reader");
    return RMW_RET_ERROR;
  }
  /* Create DDS publisher/subscriber objects that will be used for all DDS writers/readers
    to be created for RMW publishers/subscriptions. */
  if ((this->dds_pub = dds_create_publisher(this->ppant, nullptr, nullptr)) < 0) {
    this->clean_up();
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DDS publisher");
    return RMW_RET_ERROR;
  }
  if ((this->dds_sub = dds_create_subscriber(this->ppant, nullptr, nullptr)) < 0) {
    this->clean_up();
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DDS subscriber");
    return RMW_RET_ERROR;
  }

  rmw_qos_profile_t pubsub_qos = rmw_qos_profile_default;
  pubsub_qos.avoid_ros_namespace_conventions = true;
  pubsub_qos.history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
  pubsub_qos.depth = 1;
  pubsub_qos.durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
  pubsub_qos.reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;

  /* Create RMW publisher/subscription/guard condition used by rmw_dds_common
    discovery */
  rmw_publisher_options_t publisher_options = rmw_get_default_publisher_options();
  this->common.pub = create_publisher(
    this->ppant, this->dds_pub,
    rosidl_typesupport_cpp::get_message_type_support_handle<ParticipantEntitiesInfo>(),
    "ros_discovery_info",
    &pubsub_qos,
    &publisher_options);
  if (this->common.pub == nullptr) {
    this->clean_up();
    return RMW_RET_ERROR;
  }

  rmw_subscription_options_t subscription_options = rmw_get_default_subscription_options();
  subscription_options.ignore_local_publications = true;
  // FIXME: keyed topics => KEEP_LAST and depth 1.
  pubsub_qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
  this->common.sub = create_subscription(
    this->ppant, this->dds_sub,
    rosidl_typesupport_cpp::get_message_type_support_handle<ParticipantEntitiesInfo>(),
    "ros_discovery_info",
    &pubsub_qos,
    &subscription_options);
  if (this->common.sub == nullptr) {
    this->clean_up();
    return RMW_RET_ERROR;
  }

  this->common.graph_guard_condition = create_guard_condition();
  if (this->common.graph_guard_condition == nullptr) {
    this->clean_up();
    return RMW_RET_BAD_ALLOC;
  }

  this->common.graph_cache.set_on_change_callback(
    [guard_condition = this->common.graph_guard_condition]() {
      rmw_ret_t ret = rmw_trigger_guard_condition(guard_condition);
      if (ret != RMW_RET_OK) {
        RMW_SET_ERROR_MSG("graph cache on_change_callback failed to trigger guard condition");
      }
    });

  get_entity_gid(this->ppant, this->common.gid);
  this->common.graph_cache.add_participant(this->common.gid, options->enclave);

  // One could also use a set of listeners instead of a thread for maintaining the graph cache:
  // - Locally published samples shouldn't make it to the reader, so there shouldn't be a deadlock
  //   caused by the graph cache's mutex already having been locked by (e.g.) rmw_create_node.
  // - Whatever the graph cache implementation does, it shouldn't involve much more than local state
  //   updates and triggering a guard condition, and so that should be safe.
  // however, the graph cache updates could be expensive, and so performing those operations on
  // the thread receiving data from the network may not be wise.
  rmw_ret_t ret;
  if ((ret = discovery_thread_start(this)) != RMW_RET_OK) {
    this->clean_up();
    return ret;
  }
  ++this->node_count;
  return RMW_RET_OK;
}

void
rmw_context_impl_t::clean_up()
{
  discovery_thread_stop(common);
  common.graph_cache.clear_on_change_callback();
  if (common.graph_guard_condition) {
    destroy_guard_condition(common.graph_guard_condition);
  }
  if (common.pub) {
    destroy_publisher(common.pub);
  }
  if (common.sub) {
    destroy_subscription(common.sub);
  }
  if (ppant > 0 && dds_delete(ppant) < 0) {
    RCUTILS_SAFE_FWRITE_TO_STDERR(
      "Failed to destroy domain in destructor\n");
  }
  check_destroy_domain(domain_id);
}

rmw_ret_t
rmw_context_impl_t::fini()
{
  std::lock_guard<std::mutex> guard(initialization_mutex);
  if (0u != --this->node_count) {
    // destruction shouldn't happen yet
    return RMW_RET_OK;
  }
  this->clean_up();
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init(const rmw_init_options_t * options, rmw_context_t * context)
{
  rmw_ret_t ret;

  static_cast<void>(options);
  static_cast<void>(context);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    options,
    options->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  /* domain_id = UINT32_MAX = Cyclone DDS' "default domain id".*/
  if (options->domain_id >= UINT32_MAX) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: domain id out of range");
    return RMW_RET_INVALID_ARGUMENT;
  }

  context->instance_id = options->instance_id;
  context->implementation_identifier = eclipse_cyclonedds_identifier;
  context->impl = nullptr;

  if ((ret = rmw_init_options_copy(options, &context->options)) != RMW_RET_OK) {
    return ret;
  }

  rmw_context_impl_t * impl = new(std::nothrow) rmw_context_impl_t();
  if (nullptr == impl) {
    return RMW_RET_BAD_ALLOC;
  }

  context->impl = impl;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_shutdown(rmw_context_t * context)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  // Nothing to do here for now.
  // This is just the middleware's notification that shutdown was called.
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_context_fini(rmw_context_t * context)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  delete context->impl;
  *context = rmw_get_zero_initialized_context();
  return RMW_RET_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    NODES                                                          ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_node_t * rmw_create_node(
  rmw_context_t * context, const char * name,
  const char * namespace_, size_t domain_id,
  bool localhost_only)
{
  static_cast<void>(domain_id);
  static_cast<void>(localhost_only);
  RET_NULL_X(name, return nullptr);
  RET_NULL_X(namespace_, return nullptr);
  rmw_ret_t ret;
  int dummy_validation_result;
  size_t dummy_invalid_index;
  if ((ret =
    rmw_validate_node_name(name, &dummy_validation_result, &dummy_invalid_index)) != RMW_RET_OK)
  {
    return nullptr;
  }

  ret = context->impl->init(&context->options);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }

  auto * node_impl = new CddsNode();
  rmw_node_t * node_handle = nullptr;
  RET_ALLOC_X(node_impl, goto fail_node_impl);

  node_handle = rmw_node_allocate();
  RET_ALLOC_X(node_handle, goto fail_node_handle);
  node_handle->implementation_identifier = eclipse_cyclonedds_identifier;
  node_handle->data = node_impl;
  node_handle->context = context;

  node_handle->name = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(name) + 1));
  RET_ALLOC_X(node_handle->name, goto fail_node_handle_name);
  memcpy(const_cast<char *>(node_handle->name), name, strlen(name) + 1);

  node_handle->namespace_ =
    static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(namespace_) + 1));
  RET_ALLOC_X(node_handle->namespace_, goto fail_node_handle_namespace);
  memcpy(const_cast<char *>(node_handle->namespace_), namespace_, strlen(namespace_) + 1);

  {
    // Though graph_cache methods are thread safe, both cache update and publishing have to also
    // be atomic.
    // If not, the following race condition is possible:
    // node1-update-get-message / node2-update-get-message / node2-publish / node1-publish
    // In that case, the last message published is not accurate.
    auto common = &context->impl->common;
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo participant_msg =
      common->graph_cache.add_node(common->gid, name, namespace_);
    if (RMW_RET_OK != rmw_publish(
        common->pub,
        static_cast<void *>(&participant_msg),
        nullptr))
    {
      // If publishing the message failed, we don't have to publish an update
      // after removing it from the graph cache */
      static_cast<void>(common->graph_cache.remove_node(common->gid, name, namespace_));
      goto fail_pub_info;
    }
  }
  return node_handle;

fail_pub_info:
  rmw_free(const_cast<char *>(node_handle->namespace_));
fail_node_handle_namespace:
  rmw_free(const_cast<char *>(node_handle->name));
fail_node_handle_name:
  rmw_node_free(node_handle);
fail_node_handle:
  delete node_impl;
fail_node_impl:
  context->impl->fini();
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_node(rmw_node_t * node)
{
  rmw_ret_t result_ret = RMW_RET_OK;
  RET_WRONG_IMPLID(node);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL(node_impl);

  {
    // Though graph_cache methods are thread safe, both cache update and publishing have to also
    // be atomic.
    // If not, the following race condition is possible:
    // node1-update-get-message / node2-update-get-message / node2-publish / node1-publish
    // In that case, the last message published is not accurate.
    auto common = &node->context->impl->common;
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo participant_msg =
      common->graph_cache.add_node(common->gid, node->name, node->namespace_);
    result_ret = rmw_publish(
      common->pub, static_cast<void *>(&participant_msg), nullptr);
  }

  rmw_free(const_cast<char *>(node->name));
  rmw_free(const_cast<char *>(node->namespace_));
  node->context->impl->fini();
  rmw_node_free(node);
  delete node_impl;
  return result_ret;
}

extern "C" const rmw_guard_condition_t * rmw_node_get_graph_guard_condition(const rmw_node_t * node)
{
  RET_WRONG_IMPLID_X(node, return nullptr);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL_X(node_impl, return nullptr);
  return node->context->impl->common.graph_guard_condition;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    (DE)SERIALIZATION                                              ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

using MessageTypeSupport_c =
  rmw_cyclonedds_cpp::MessageTypeSupport<rosidl_typesupport_introspection_c__MessageMembers>;
using MessageTypeSupport_cpp =
  rmw_cyclonedds_cpp::MessageTypeSupport<rosidl_typesupport_introspection_cpp::MessageMembers>;

extern "C" rmw_ret_t rmw_get_serialized_message_size(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds, size_t * size)
{
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(size);

  RMW_SET_ERROR_MSG("rmw_get_serialized_message_size: unimplemented");
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_serialize(
  const void * ros_message,
  const rosidl_message_type_support_t * type_support,
  rmw_serialized_message_t * serialized_message)
{
  rmw_ret_t ret;
  try {
    auto writer = rmw_cyclonedds_cpp::make_cdr_writer(
      rmw_cyclonedds_cpp::make_message_value_type(type_support));

    auto size = writer->get_serialized_size(ros_message);
    if ((ret = rmw_serialized_message_resize(serialized_message, size) != RMW_RET_OK)) {
      RMW_SET_ERROR_MSG("rmw_serialize: failed to allocate space for message");
      return ret;
    }
    writer->serialize(serialized_message->buffer, ros_message);
    serialized_message->buffer_length = size;
    return RMW_RET_OK;
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("rmw_serialize: failed to serialize: %s", e.what());
    return RMW_RET_ERROR;
  }
}

extern "C" rmw_ret_t rmw_deserialize(
  const rmw_serialized_message_t * serialized_message,
  const rosidl_message_type_support_t * type_support,
  void * ros_message)
{
  bool ok;
  try {
    cycdeser sd(serialized_message->buffer, serialized_message->buffer_length);
    const rosidl_message_type_support_t * ts;
    if ((ts =
      get_message_typesupport_handle(
        type_support, rosidl_typesupport_introspection_c__identifier)) != nullptr)
    {
      auto members =
        static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(ts->data);
      MessageTypeSupport_c msgts(members);
      ok = msgts.deserializeROSmessage(sd, ros_message, nullptr);
    } else {
      if ((ts =
        get_message_typesupport_handle(
          type_support, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr)
      {
        auto members =
          static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(ts->data);
        MessageTypeSupport_cpp msgts(members);
        ok = msgts.deserializeROSmessage(sd, ros_message, nullptr);
      } else {
        RMW_SET_ERROR_MSG("rmw_serialize: type support trouble");
        return RMW_RET_ERROR;
      }
    }
  } catch (rmw_cyclonedds_cpp::Exception & e) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("rmw_serialize: %s", e.what());
    ok = false;
  }

  return ok ? RMW_RET_OK : RMW_RET_ERROR;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    TOPIC CREATION                                                 ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

/* Publications need the sertopic that DDSI uses for the topic when publishing a
   serialized message.  With the old ("arbitrary") interface of Cyclone, one doesn't know
   the sertopic that is actually used because that may be the one that was provided in the
   call to dds_create_topic_arbitrary(), but it may also be one that was introduced by a
   preceding call to create the same topic.

   There is no way of discovering which case it is, and there is no way of getting access
   to the correct sertopic.  The best one can do is to keep using one provided when
   creating the topic -- and fortunately using the wrong sertopic has surprisingly few
   nasty side-effects, but it still wrong.

   Because the caller retains ownership, so this is easy, but it does require dropping the
   reference when cleaning up.

   The new ("generic") interface instead takes over the ownership of the reference iff it
   succeeds and it returns a non-counted reference to the sertopic actually used.  The
   lifetime of the reference is at least as long as the lifetime of the DDS topic exists;
   and the topic's lifetime is at least that of the readers/writers using it.  This
   reference can therefore safely be used. */

static dds_entity_t create_topic(
  dds_entity_t pp, struct ddsi_sertopic * sertopic,
  struct ddsi_sertopic ** stact)
{
  dds_entity_t tp;
  tp = dds_create_topic_generic(pp, &sertopic, nullptr, nullptr, nullptr);
  if (tp < 0) {
    ddsi_sertopic_unref(sertopic);
  } else {
    if (stact) {
      *stact = sertopic;
    }
  }
  return tp;
}

static dds_entity_t create_topic(dds_entity_t pp, struct ddsi_sertopic * sertopic)
{
  dds_entity_t tp = create_topic(pp, sertopic, nullptr);
  return tp;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    PUBLICATIONS                                                   ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_publish(
  const rmw_publisher_t * publisher, const void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);    // unused
  RET_WRONG_IMPLID(publisher);
  RET_NULL(ros_message);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  assert(pub);
  if (dds_write(pub->enth, ros_message) >= 0) {
    return RMW_RET_OK;
  } else {
    RMW_SET_ERROR_MSG("failed to publish data");
    return RMW_RET_ERROR;
  }
}

extern "C" rmw_ret_t rmw_publish_serialized_message(
  const rmw_publisher_t * publisher,
  const rmw_serialized_message_t * serialized_message, rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);    // unused
  RET_WRONG_IMPLID(publisher);
  RET_NULL(serialized_message);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  struct ddsi_serdata * d = serdata_rmw_from_serialized_message(
    pub->sertopic, serialized_message->buffer, serialized_message->buffer_length);
  const bool ok = (dds_writecdr(pub->enth, d) >= 0);
  return ok ? RMW_RET_OK : RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_publish_loaned_message(
  const rmw_publisher_t * publisher,
  void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  (void) publisher;
  (void) ros_message;
  (void) allocation;

  RMW_SET_ERROR_MSG("rmw_publish_loaned_message not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
}

static const rosidl_message_type_support_t * get_typesupport(
  const rosidl_message_type_support_t * type_supports)
{
  const rosidl_message_type_support_t * ts;
  if ((ts =
    get_message_typesupport_handle(
      type_supports, rosidl_typesupport_introspection_c__identifier)) != nullptr)
  {
    return ts;
  } else {
    if ((ts =
      get_message_typesupport_handle(
        type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr)
    {
      return ts;
    } else {
      RMW_SET_ERROR_MSG("type support not from this implementation");
      return nullptr;
    }
  }
}

static std::string make_fqtopic(
  const char * prefix, const char * topic_name, const char * suffix,
  bool avoid_ros_namespace_conventions)
{
  if (avoid_ros_namespace_conventions) {
    return std::string(topic_name) + std::string(suffix);
  } else {
    return std::string(prefix) + std::string(topic_name) + std::string(suffix);
  }
}

static std::string make_fqtopic(
  const char * prefix, const char * topic_name, const char * suffix,
  const rmw_qos_profile_t * qos_policies)
{
  return make_fqtopic(prefix, topic_name, suffix, qos_policies->avoid_ros_namespace_conventions);
}

static dds_qos_t * create_readwrite_qos(
  const rmw_qos_profile_t * qos_policies,
  bool ignore_local_publications)
{
  dds_duration_t ldur;
  dds_qos_t * qos = dds_create_qos();
  dds_qset_writer_data_lifecycle(qos, false); /* disable autodispose */
  switch (qos_policies->history) {
    case RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_HISTORY_UNKNOWN:
    case RMW_QOS_POLICY_HISTORY_KEEP_LAST:
      if (qos_policies->depth == RMW_QOS_POLICY_DEPTH_SYSTEM_DEFAULT) {
        dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, 1);
      } else {
        if (qos_policies->depth < 1 || qos_policies->depth > INT32_MAX) {
          RMW_SET_ERROR_MSG("unsupported history depth");
          dds_delete_qos(qos);
          return nullptr;
        }
        dds_qset_history(qos, DDS_HISTORY_KEEP_LAST, static_cast<int32_t>(qos_policies->depth));
      }
      break;
    case RMW_QOS_POLICY_HISTORY_KEEP_ALL:
      dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, DDS_LENGTH_UNLIMITED);
      break;
    default:
      rmw_cyclonedds_cpp::unreachable();
  }
  switch (qos_policies->reliability) {
    case RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_RELIABILITY_UNKNOWN:
    case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
      dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_INFINITY);
      break;
    case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
      dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
      break;
    default:
      rmw_cyclonedds_cpp::unreachable();
  }
  switch (qos_policies->durability) {
    case RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_DURABILITY_UNKNOWN:
    case RMW_QOS_POLICY_DURABILITY_VOLATILE:
      dds_qset_durability(qos, DDS_DURABILITY_VOLATILE);
      break;
    case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL: {
        /* Cyclone uses durability service QoS for determining what to retain as historical data,
           separating the reliability window from the historical data; but that is somewhat unusual
           among DDS implementations ... */
        dds_history_kind_t hk;
        int32_t hd;
        dds_qget_history(qos, &hk, &hd);
        dds_qset_durability(qos, DDS_DURABILITY_TRANSIENT_LOCAL);
        dds_qset_durability_service(
          qos, DDS_SECS(0), hk, hd, DDS_LENGTH_UNLIMITED, DDS_LENGTH_UNLIMITED,
          DDS_LENGTH_UNLIMITED);
        break;
      }
    default:
      rmw_cyclonedds_cpp::unreachable();
  }
  if (qos_policies->lifespan.sec > 0 || qos_policies->lifespan.nsec > 0) {
    dds_qset_lifespan(qos, DDS_SECS(qos_policies->lifespan.sec) + qos_policies->lifespan.nsec);
  }
  if (qos_policies->deadline.sec > 0 || qos_policies->deadline.nsec > 0) {
    dds_qset_deadline(qos, DDS_SECS(qos_policies->deadline.sec) + qos_policies->deadline.nsec);
  }

  if (qos_policies->liveliness_lease_duration.sec == 0 &&
    qos_policies->liveliness_lease_duration.nsec == 0)
  {
    ldur = DDS_INFINITY;
  } else {
    ldur = DDS_SECS(qos_policies->liveliness_lease_duration.sec) +
      qos_policies->liveliness_lease_duration.nsec;
  }
  switch (qos_policies->liveliness) {
    case RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_LIVELINESS_AUTOMATIC:
    case RMW_QOS_POLICY_LIVELINESS_UNKNOWN:
      dds_qset_liveliness(qos, DDS_LIVELINESS_AUTOMATIC, ldur);
      break;
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC:
      dds_qset_liveliness(qos, DDS_LIVELINESS_MANUAL_BY_TOPIC, ldur);
      break;
    default:
      rmw_cyclonedds_cpp::unreachable();
  }
  if (ignore_local_publications) {
    dds_qset_ignorelocal(qos, DDS_IGNORELOCAL_PARTICIPANT);
  }
  return qos;
}

static rmw_qos_policy_kind_t dds_qos_policy_to_rmw_qos_policy(dds_qos_policy_id_t policy_id)
{
  switch (policy_id) {
    case DDS_DURABILITY_QOS_POLICY_ID:
      return RMW_QOS_POLICY_DURABILITY;
    case DDS_DEADLINE_QOS_POLICY_ID:
      return RMW_QOS_POLICY_DEADLINE;
    case DDS_LIVELINESS_QOS_POLICY_ID:
      return RMW_QOS_POLICY_LIVELINESS;
    case DDS_RELIABILITY_QOS_POLICY_ID:
      return RMW_QOS_POLICY_RELIABILITY;
    case DDS_HISTORY_QOS_POLICY_ID:
      return RMW_QOS_POLICY_HISTORY;
    case DDS_LIFESPAN_QOS_POLICY_ID:
      return RMW_QOS_POLICY_LIFESPAN;
    default:
      return RMW_QOS_POLICY_INVALID;
  }
}

static bool dds_qos_to_rmw_qos(const dds_qos_t * dds_qos, rmw_qos_profile_t * qos_policies)
{
  assert(dds_qos);
  assert(qos_policies);
  {
    dds_history_kind_t kind;
    int32_t depth;
    if (!dds_qget_history(dds_qos, &kind, &depth)) {
      RMW_SET_ERROR_MSG("get_readwrite_qos: history not set");
      return false;
    }
    switch (kind) {
      case DDS_HISTORY_KEEP_LAST:
        qos_policies->history = RMW_QOS_POLICY_HISTORY_KEEP_LAST;
        qos_policies->depth = (uint32_t) depth;
        break;
      case DDS_HISTORY_KEEP_ALL:
        qos_policies->history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
        qos_policies->depth = (uint32_t) depth;
        break;
      default:
        rmw_cyclonedds_cpp::unreachable();
    }
  }

  {
    dds_reliability_kind_t kind;
    dds_duration_t max_blocking_time;
    if (!dds_qget_reliability(dds_qos, &kind, &max_blocking_time)) {
      RMW_SET_ERROR_MSG("get_readwrite_qos: history not set");
      return false;
    }
    switch (kind) {
      case DDS_RELIABILITY_BEST_EFFORT:
        qos_policies->reliability = RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT;
        break;
      case DDS_RELIABILITY_RELIABLE:
        qos_policies->reliability = RMW_QOS_POLICY_RELIABILITY_RELIABLE;
        break;
      default:
        rmw_cyclonedds_cpp::unreachable();
    }
  }

  {
    dds_durability_kind_t kind;
    if (!dds_qget_durability(dds_qos, &kind)) {
      RMW_SET_ERROR_MSG("get_readwrite_qos: durability not set");
      return false;
    }
    switch (kind) {
      case DDS_DURABILITY_VOLATILE:
        qos_policies->durability = RMW_QOS_POLICY_DURABILITY_VOLATILE;
        break;
      case DDS_DURABILITY_TRANSIENT_LOCAL:
        qos_policies->durability = RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL;
        break;
      case DDS_DURABILITY_TRANSIENT:
      case DDS_DURABILITY_PERSISTENT:
        qos_policies->durability = RMW_QOS_POLICY_DURABILITY_UNKNOWN;
        break;
      default:
        rmw_cyclonedds_cpp::unreachable();
    }
  }

  {
    dds_duration_t deadline;
    if (!dds_qget_deadline(dds_qos, &deadline)) {
      RMW_SET_ERROR_MSG("get_readwrite_qos: deadline not set");
      return false;
    }
    qos_policies->deadline.sec = (uint64_t) deadline / 1000000000;
    qos_policies->deadline.nsec = (uint64_t) deadline % 1000000000;
  }

  {
    dds_duration_t lifespan;
    if (!dds_qget_lifespan(dds_qos, &lifespan)) {
      lifespan = DDS_INFINITY;
    }
    qos_policies->lifespan.sec = (uint64_t) lifespan / 1000000000;
    qos_policies->lifespan.nsec = (uint64_t) lifespan % 1000000000;
  }

  {
    dds_liveliness_kind_t kind;
    dds_duration_t lease_duration;
    if (!dds_qget_liveliness(dds_qos, &kind, &lease_duration)) {
      RMW_SET_ERROR_MSG("get_readwrite_qos: liveliness not set");
      return false;
    }
    switch (kind) {
      case DDS_LIVELINESS_AUTOMATIC:
        qos_policies->liveliness = RMW_QOS_POLICY_LIVELINESS_AUTOMATIC;
        break;
      case DDS_LIVELINESS_MANUAL_BY_PARTICIPANT:
        qos_policies->liveliness = RMW_QOS_POLICY_LIVELINESS_UNKNOWN;
        break;
      case DDS_LIVELINESS_MANUAL_BY_TOPIC:
        qos_policies->liveliness = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC;
        break;
      default:
        rmw_cyclonedds_cpp::unreachable();
    }
    qos_policies->liveliness_lease_duration.sec = (uint64_t) lease_duration / 1000000000;
    qos_policies->liveliness_lease_duration.nsec = (uint64_t) lease_duration % 1000000000;
  }

  return true;
}

static bool get_readwrite_qos(dds_entity_t handle, rmw_qos_profile_t * rmw_qos_policies)
{
  dds_qos_t * qos = dds_create_qos();
  dds_return_t ret = false;
  if (dds_get_qos(handle, qos) < 0) {
    RMW_SET_ERROR_MSG("get_readwrite_qos: invalid handle");
  } else {
    ret = dds_qos_to_rmw_qos(qos, rmw_qos_policies);
  }
  dds_delete_qos(qos);
  return ret;
}

static CddsPublisher * create_cdds_publisher(
  dds_entity_t dds_ppant, dds_entity_t dds_pub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name,
  const rmw_qos_profile_t * qos_policies)
{
  RET_NULL_OR_EMPTYSTR_X(topic_name, return nullptr);
  RET_NULL_X(qos_policies, return nullptr);
  const rosidl_message_type_support_t * type_support = get_typesupport(type_supports);
  RET_NULL_X(type_support, return nullptr);
  CddsPublisher * pub = new CddsPublisher();
  dds_entity_t topic;
  dds_qos_t * qos;

  std::string fqtopic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", qos_policies);

  auto sertopic = create_sertopic(
    fqtopic_name.c_str(), type_support->typesupport_identifier,
    create_message_type_support(type_support->data, type_support->typesupport_identifier), false,
    rmw_cyclonedds_cpp::make_message_value_type(type_supports));
  struct ddsi_sertopic * stact;
  topic = create_topic(dds_ppant, sertopic, &stact);
  if (topic < 0) {
    RMW_SET_ERROR_MSG("failed to create topic");
    goto fail_topic;
  }
  if ((qos = create_readwrite_qos(qos_policies, false)) == nullptr) {
    goto fail_qos;
  }
  if ((pub->enth = dds_create_writer(dds_pub, topic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create writer");
    goto fail_writer;
  }
  if (dds_get_instance_handle(pub->enth, &pub->pubiid) < 0) {
    RMW_SET_ERROR_MSG("failed to get instance handle for writer");
    goto fail_instance_handle;
  }
  get_entity_gid(pub->enth, pub->gid);
  pub->sertopic = stact;
  dds_delete_qos(qos);
  dds_delete(topic);
  return pub;

fail_instance_handle:
  if (dds_delete(pub->enth) < 0) {
    RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to destroy writer during error handling");
  }
fail_writer:
  dds_delete_qos(qos);
fail_qos:
  dds_delete(topic);
  ddsi_sertopic_unref(stact);
fail_topic:
  delete pub;
  return nullptr;
}

extern "C" rmw_ret_t rmw_init_publisher_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds, rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_init_publisher_allocation: unimplemented");
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_fini_publisher_allocation(rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_fini_publisher_allocation: unimplemented");
  return RMW_RET_ERROR;
}

static rmw_publisher_t * create_publisher(
  dds_entity_t dds_ppant, dds_entity_t dds_pub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options
)
{
  CddsPublisher * pub;
  rmw_publisher_t * rmw_publisher;
  if ((pub =
    create_cdds_publisher(
      dds_ppant, dds_pub, type_supports, topic_name,
      qos_policies)) == nullptr)
  {
    goto fail_common_init;
  }
  rmw_publisher = rmw_publisher_allocate();
  RET_ALLOC_X(rmw_publisher, goto fail_publisher);
  rmw_publisher->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_publisher->data = pub;
  rmw_publisher->topic_name = reinterpret_cast<char *>(rmw_allocate(strlen(topic_name) + 1));
  RET_ALLOC_X(rmw_publisher->topic_name, goto fail_topic_name);
  memcpy(const_cast<char *>(rmw_publisher->topic_name), topic_name, strlen(topic_name) + 1);
  rmw_publisher->options = *publisher_options;
  rmw_publisher->can_loan_messages = false;
  return rmw_publisher;
fail_topic_name:
  rmw_publisher_free(rmw_publisher);
fail_publisher:
  if (dds_delete(pub->enth) < 0) {
    RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to delete writer during error handling");
  }
  delete pub;
fail_common_init:
  return nullptr;
}

extern "C" rmw_publisher_t * rmw_create_publisher(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options
)
{
  RET_WRONG_IMPLID_X(node, return nullptr);
  rmw_publisher_t * pub = create_publisher(
    node->context->impl->ppant, node->context->impl->dds_pub,
    type_supports, topic_name, qos_policies,
    publisher_options);
  if (pub != nullptr) {
    // Update graph
    auto common = &node->context->impl->common;
    const auto cddspub = static_cast<const CddsPublisher *>(pub->data);
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.associate_writer(cddspub->gid, common->gid, node->name, node->namespace_);
    if (RMW_RET_OK != rmw_publish(
        common->pub,
        static_cast<void *>(&msg),
        nullptr))
    {
      static_cast<void>(common->graph_cache.dissociate_writer(
        cddspub->gid, common->gid, node->name, node->namespace_));
      static_cast<void>(destroy_publisher(pub));
      return nullptr;
    }
  }
  return pub;
}

extern "C" rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t * publisher, rmw_gid_t * gid)
{
  RET_WRONG_IMPLID(publisher);
  RET_NULL(gid);
  auto pub = static_cast<const CddsPublisher *>(publisher->data);
  RET_NULL(pub);
  gid->implementation_identifier = eclipse_cyclonedds_identifier;
  memset(gid->data, 0, sizeof(gid->data));
  assert(sizeof(pub->pubiid) <= sizeof(gid->data));
  memcpy(gid->data, &pub->pubiid, sizeof(pub->pubiid));
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_compare_gids_equal(
  const rmw_gid_t * gid1, const rmw_gid_t * gid2,
  bool * result)
{
  RET_WRONG_IMPLID(gid1);
  RET_WRONG_IMPLID(gid2);
  RET_NULL(result);
  /* alignment is potentially lost because of the translation to an array of bytes, so use
     memcmp instead of a simple integer comparison */
  *result = memcmp(gid1->data, gid2->data, sizeof(gid1->data)) == 0;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_publisher_count_matched_subscriptions(
  const rmw_publisher_t * publisher,
  size_t * subscription_count)
{
  RET_WRONG_IMPLID(publisher);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  dds_publication_matched_status_t status;
  if (dds_get_publication_matched_status(pub->enth, &status) < 0) {
    return RMW_RET_ERROR;
  } else {
    *subscription_count = status.current_count;
    return RMW_RET_OK;
  }
}

rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t * publisher)
{
  RET_WRONG_IMPLID(publisher);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (dds_assert_liveliness(pub->enth) < 0) {
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

rmw_ret_t rmw_publisher_get_actual_qos(const rmw_publisher_t * publisher, rmw_qos_profile_t * qos)
{
  RET_NULL(qos);
  RET_WRONG_IMPLID(publisher);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (get_readwrite_qos(pub->enth, qos)) {
    return RMW_RET_OK;
  } else {
    return RMW_RET_ERROR;
  }
}

extern "C" rmw_ret_t rmw_borrow_loaned_message(
  const rmw_publisher_t * publisher,
  const rosidl_message_type_support_t * type_support,
  void ** ros_message)
{
  (void) publisher;
  (void) type_support;
  (void) ros_message;
  RMW_SET_ERROR_MSG("rmw_borrow_loaned_message not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_return_loaned_message_from_publisher(
  const rmw_publisher_t * publisher,
  void * loaned_message)
{
  (void) publisher;
  (void) loaned_message;
  RMW_SET_ERROR_MSG(
    "rmw_return_loaned_message_from_publisher not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
}

static rmw_ret_t destroy_publisher(rmw_publisher_t * publisher)
{
  RET_WRONG_IMPLID(publisher);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (pub != nullptr) {
    if (dds_delete(pub->enth) < 0) {
      RMW_SET_ERROR_MSG("failed to delete writer");
    }
    delete pub;
  }
  rmw_free(const_cast<char *>(publisher->topic_name));
  publisher->topic_name = nullptr;
  rmw_publisher_free(publisher);
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_destroy_publisher(rmw_node_t * node, rmw_publisher_t * publisher)
{
  RET_WRONG_IMPLID(node);
  {
    auto common = &node->context->impl->common;
    const auto cddspub = static_cast<const CddsPublisher *>(publisher->data);
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.dissociate_writer(
      cddspub->gid, common->gid, node->name,
      node->namespace_);
    if (RMW_RET_OK != rmw_publish(common->pub, static_cast<void *>(&msg), nullptr)) {
      RMW_SET_ERROR_MSG(
        "failed to publish ParticipantEntitiesInfo message after dissociating writer");
    }
  }
  return destroy_publisher(publisher);
}


/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    SUBSCRIPTIONS                                                  ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

static CddsSubscription * create_cdds_subscription(
  dds_entity_t dds_ppant, dds_entity_t dds_sub,
  const rosidl_message_type_support_t * type_supports, const char * topic_name,
  const rmw_qos_profile_t * qos_policies, bool ignore_local_publications)
{
  RET_NULL_OR_EMPTYSTR_X(topic_name, return nullptr);
  RET_NULL_X(qos_policies, return nullptr);
  const rosidl_message_type_support_t * type_support = get_typesupport(type_supports);
  RET_NULL_X(type_support, return nullptr);
  CddsSubscription * sub = new CddsSubscription();
  dds_entity_t topic;
  dds_qos_t * qos;

  std::string fqtopic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", qos_policies);

  auto sertopic = create_sertopic(
    fqtopic_name.c_str(), type_support->typesupport_identifier,
    create_message_type_support(type_support->data, type_support->typesupport_identifier), false,
    rmw_cyclonedds_cpp::make_message_value_type(type_supports));
  topic = create_topic(dds_ppant, sertopic);
  if (topic < 0) {
    RMW_SET_ERROR_MSG("failed to create topic");
    goto fail_topic;
  }
  if ((qos = create_readwrite_qos(qos_policies, ignore_local_publications)) == nullptr) {
    goto fail_qos;
  }
  if ((sub->enth = dds_create_reader(dds_sub, topic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create reader");
    goto fail_reader;
  }
  get_entity_gid(sub->enth, sub->gid);
  if ((sub->rdcondh = dds_create_readcondition(sub->enth, DDS_ANY_STATE)) < 0) {
    RMW_SET_ERROR_MSG("failed to create readcondition");
    goto fail_readcond;
  }
  dds_delete_qos(qos);
  dds_delete(topic);
  return sub;
fail_readcond:
  if (dds_delete(sub->enth) < 0) {
    RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to delete reader during error handling");
  }
fail_reader:
  dds_delete_qos(qos);
fail_qos:
  dds_delete(topic);
fail_topic:
  delete sub;
  return nullptr;
}

extern "C" rmw_ret_t rmw_init_subscription_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_runtime_c__Sequence__bound * message_bounds,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(type_support);
  static_cast<void>(message_bounds);
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_init_subscription_allocation: unimplemented");
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_fini_subscription_allocation(rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_fini_subscription_allocation: unimplemented");
  return RMW_RET_ERROR;
}

static rmw_subscription_t * create_subscription(
  dds_entity_t dds_ppant, dds_entity_t dds_sub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_subscription_options_t * subscription_options)
{
  CddsSubscription * sub;
  rmw_subscription_t * rmw_subscription;
  if (
    (sub = create_cdds_subscription(
      dds_ppant, dds_sub, type_supports, topic_name, qos_policies,
      subscription_options->ignore_local_publications)) == nullptr)
  {
    goto fail_common_init;
  }
  rmw_subscription = rmw_subscription_allocate();
  RET_ALLOC_X(rmw_subscription, goto fail_subscription);
  rmw_subscription->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_subscription->data = sub;
  rmw_subscription->topic_name =
    reinterpret_cast<const char *>(rmw_allocate(strlen(topic_name) + 1));
  RET_ALLOC_X(rmw_subscription->topic_name, goto fail_topic_name);
  memcpy(const_cast<char *>(rmw_subscription->topic_name), topic_name, strlen(topic_name) + 1);
  rmw_subscription->options = *subscription_options;
  rmw_subscription->can_loan_messages = false;
  return rmw_subscription;
fail_topic_name:
  rmw_subscription_free(rmw_subscription);
fail_subscription:
  if (dds_delete(sub->rdcondh) < 0) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "failed to delete readcondition during error handling");
  }
  if (dds_delete(sub->enth) < 0) {
    RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to delete reader during error handling");
  }
  delete sub;
fail_common_init:
  return nullptr;
}

extern "C" rmw_subscription_t * rmw_create_subscription(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_subscription_options_t * subscription_options)
{
  RET_WRONG_IMPLID_X(node, return nullptr);
  rmw_subscription_t * sub = create_subscription(
    node->context->impl->ppant, node->context->impl->dds_sub,
    type_supports, topic_name, qos_policies,
    subscription_options);
  if (sub != nullptr) {
    // Update graph
    auto common = &node->context->impl->common;
    const auto cddssub = static_cast<const CddsSubscription *>(sub->data);
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.associate_reader(cddssub->gid, common->gid, node->name, node->namespace_);
    if (RMW_RET_OK != rmw_publish(
        common->pub,
        static_cast<void *>(&msg),
        nullptr))
    {
      static_cast<void>(common->graph_cache.dissociate_reader(
        cddssub->gid, common->gid, node->name, node->namespace_));
      static_cast<void>(destroy_subscription(sub));
      return nullptr;
    }
  }
  return sub;
}

extern "C" rmw_ret_t rmw_subscription_count_matched_publishers(
  const rmw_subscription_t * subscription, size_t * publisher_count)
{
  RET_WRONG_IMPLID(subscription);
  auto sub = static_cast<CddsSubscription *>(subscription->data);
  dds_subscription_matched_status_t status;
  if (dds_get_subscription_matched_status(sub->enth, &status) < 0) {
    return RMW_RET_ERROR;
  } else {
    *publisher_count = status.current_count;
    return RMW_RET_OK;
  }
}

extern "C" rmw_ret_t rmw_subscription_get_actual_qos(
  const rmw_subscription_t * subscription,
  rmw_qos_profile_t * qos)
{
  RET_NULL(qos);
  RET_WRONG_IMPLID(subscription);
  auto sub = static_cast<CddsSubscription *>(subscription->data);
  if (get_readwrite_qos(sub->enth, qos)) {
    return RMW_RET_OK;
  } else {
    return RMW_RET_ERROR;
  }
}

static rmw_ret_t destroy_subscription(rmw_subscription_t * subscription)
{
  RET_WRONG_IMPLID(subscription);
  auto sub = static_cast<CddsSubscription *>(subscription->data);
  if (sub != nullptr) {
    clean_waitset_caches();
    if (dds_delete(sub->rdcondh) < 0) {
      RMW_SET_ERROR_MSG("failed to delete readcondition");
    }
    if (dds_delete(sub->enth) < 0) {
      RMW_SET_ERROR_MSG("failed to delete reader");
    }
    delete sub;
  }
  rmw_free(const_cast<char *>(subscription->topic_name));
  subscription->topic_name = nullptr;
  rmw_subscription_free(subscription);
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_destroy_subscription(rmw_node_t * node, rmw_subscription_t * subscription)
{
  RET_WRONG_IMPLID(node);
  {
    auto common = &node->context->impl->common;
    const auto cddssub = static_cast<const CddsSubscription *>(subscription->data);
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.dissociate_writer(
      cddssub->gid, common->gid, node->name,
      node->namespace_);
    if (RMW_RET_OK != rmw_publish(common->pub, static_cast<void *>(&msg), nullptr)) {
      RMW_SET_ERROR_MSG(
        "failed to publish ParticipantEntitiesInfo message after dissociating reader");
    }
  }
  return destroy_subscription(subscription);
}

static rmw_ret_t rmw_take_int(
  const rmw_subscription_t * subscription, void * ros_message,
  bool * taken, rmw_message_info_t * message_info)
{
  RET_NULL(taken);
  RET_NULL(ros_message);
  RET_WRONG_IMPLID(subscription);
  CddsSubscription * sub = static_cast<CddsSubscription *>(subscription->data);
  RET_NULL(sub);
  dds_sample_info_t info;
  while (dds_take(sub->enth, &ros_message, &info, 1, 1) == 1) {
    if (info.valid_data) {
      *taken = true;
      if (message_info) {
        message_info->publisher_gid.implementation_identifier = eclipse_cyclonedds_identifier;
        memset(message_info->publisher_gid.data, 0, sizeof(message_info->publisher_gid.data));
        assert(sizeof(info.publication_handle) <= sizeof(message_info->publisher_gid.data));
        memcpy(
          message_info->publisher_gid.data, &info.publication_handle,
          sizeof(info.publication_handle));
        message_info->source_timestamp = info.source_timestamp;
        // TODO(iluetkeb) add received timestamp, when implemented by Cyclone
        message_info->received_timestamp = 0;
      }
#if REPORT_LATE_MESSAGES > 0
      dds_time_t tnow = dds_time();
      dds_time_t dt = tnow - info.source_timestamp;
      if (dt >= DDS_MSECS(REPORT_LATE_MESSAGES)) {
        fprintf(stderr, "** sample in history for %.fms\n", static_cast<double>(dt) / 1e6);
      }
#endif
      return RMW_RET_OK;
    }
  }
  *taken = false;
  return RMW_RET_OK;
}

static rmw_ret_t rmw_take_seq(
  const rmw_subscription_t * subscription,
  size_t count,
  rmw_message_sequence_t * message_sequence,
  rmw_message_info_sequence_t * message_info_sequence,
  size_t * taken)
{
  RET_NULL(taken);
  RET_NULL(message_sequence);
  RET_NULL(message_info_sequence);
  RET_WRONG_IMPLID(subscription);

  if (count > message_sequence->capacity) {
    RMW_SET_ERROR_MSG("Insuffient capacity in message_sequence");
    return RMW_RET_ERROR;
  }

  if (count > message_info_sequence->capacity) {
    RMW_SET_ERROR_MSG("Insuffient capacity in message_info_sequence");
    return RMW_RET_ERROR;
  }

  if (count > (std::numeric_limits<uint32_t>::max)()) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
      "Cannot take %ld samples at once, limit is %d",
      count, (std::numeric_limits<uint32_t>::max)());
    return RMW_RET_ERROR;
  }

  CddsSubscription * sub = static_cast<CddsSubscription *>(subscription->data);
  RET_NULL(sub);

  std::vector<dds_sample_info_t> infos(count);
  auto maxsamples = static_cast<uint32_t>(count);
  auto ret = dds_take(sub->enth, message_sequence->data, infos.data(), count, maxsamples);

  // Returning 0 should not be an error, as it just indicates that no messages were available.
  if (ret < 0) {
    return RMW_RET_ERROR;
  }

  // Keep track of taken/not taken to reorder sequence with valid messages at the front
  std::vector<void *> taken_msg;
  std::vector<void *> not_taken_msg;
  *taken = 0u;

  for (int ii = 0; ii < ret; ++ii) {
    const dds_sample_info_t & info = infos[ii];

    void * message = &message_sequence->data[ii];
    rmw_message_info_t * message_info = &message_info_sequence->data[*taken];

    if (info.valid_data) {
      taken_msg.push_back(message);
      (*taken)++;
      if (message_info) {
        message_info->publisher_gid.implementation_identifier = eclipse_cyclonedds_identifier;
        memset(message_info->publisher_gid.data, 0, sizeof(message_info->publisher_gid.data));
        assert(sizeof(info.publication_handle) <= sizeof(message_info->publisher_gid.data));
        memcpy(
          message_info->publisher_gid.data, &info.publication_handle,
          sizeof(info.publication_handle));
      }
    } else {
      not_taken_msg.push_back(message);
    }
  }

  for (size_t ii = 0; ii < taken_msg.size(); ++ii) {
    message_sequence->data[ii] = taken_msg[ii];
  }

  for (size_t ii = 0; ii < not_taken_msg.size(); ++ii) {
    message_sequence->data[ii + taken_msg.size()] = not_taken_msg[ii];
  }

  message_sequence->size = *taken;
  message_info_sequence->size = *taken;

  return RMW_RET_OK;
}

static rmw_ret_t rmw_take_ser_int(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message, bool * taken,
  rmw_message_info_t * message_info)
{
  RET_NULL(taken);
  RET_NULL(serialized_message);
  RET_WRONG_IMPLID(subscription);
  CddsSubscription * sub = static_cast<CddsSubscription *>(subscription->data);
  RET_NULL(sub);
  dds_sample_info_t info;
  struct ddsi_serdata * dcmn;
  while (dds_takecdr(sub->enth, &dcmn, 1, &info, DDS_ANY_STATE) == 1) {
    if (info.valid_data) {
      if (message_info) {
        message_info->publisher_gid.implementation_identifier = eclipse_cyclonedds_identifier;
        memset(message_info->publisher_gid.data, 0, sizeof(message_info->publisher_gid.data));
        assert(sizeof(info.publication_handle) <= sizeof(message_info->publisher_gid.data));
        memcpy(
          message_info->publisher_gid.data, &info.publication_handle,
          sizeof(info.publication_handle));
      }
      auto d = static_cast<serdata_rmw *>(dcmn);
      /* FIXME: what about the header - should be included or not? */
      if (rmw_serialized_message_resize(serialized_message, d->size()) != RMW_RET_OK) {
        ddsi_serdata_unref(dcmn);
        *taken = false;
        return RMW_RET_ERROR;
      }
      memcpy(serialized_message->buffer, d->data(), d->size());
      serialized_message->buffer_length = d->size();
      ddsi_serdata_unref(dcmn);
      *taken = true;
      return RMW_RET_OK;
    }
    ddsi_serdata_unref(dcmn);
  }
  *taken = false;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_take(
  const rmw_subscription_t * subscription, void * ros_message,
  bool * taken, rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  return rmw_take_int(subscription, ros_message, taken, nullptr);
}

extern "C" rmw_ret_t rmw_take_with_info(
  const rmw_subscription_t * subscription, void * ros_message,
  bool * taken, rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  return rmw_take_int(subscription, ros_message, taken, message_info);
}

extern "C" rmw_ret_t rmw_take_sequence(
  const rmw_subscription_t * subscription, size_t count,
  rmw_message_sequence_t * message_sequence,
  rmw_message_info_sequence_t * message_info_sequence,
  size_t * taken, rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  return rmw_take_seq(subscription, count, message_sequence, message_info_sequence, taken);
}

extern "C" rmw_ret_t rmw_take_serialized_message(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  return rmw_take_ser_int(subscription, serialized_message, taken, nullptr);
}

extern "C" rmw_ret_t rmw_take_serialized_message_with_info(
  const rmw_subscription_t * subscription,
  rmw_serialized_message_t * serialized_message, bool * taken, rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  return rmw_take_ser_int(subscription, serialized_message, taken, message_info);
}

extern "C" rmw_ret_t rmw_take_loaned_message(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  (void) subscription;
  (void) loaned_message;
  (void) taken;
  (void) allocation;
  RMW_SET_ERROR_MSG("rmw_take_loaned_message not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_loaned_message_with_info(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  (void) subscription;
  (void) loaned_message;
  (void) taken;
  (void) message_info;
  (void) allocation;
  RMW_SET_ERROR_MSG("rmw_take_loaned_message_with_info not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_return_loaned_message_from_subscription(
  const rmw_subscription_t * subscription,
  void * loaned_message)
{
  (void) subscription;
  (void) loaned_message;
  RMW_SET_ERROR_MSG(
    "rmw_return_loaned_message_from_subscription not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    EVENTS                                                         ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

/// mapping of RMW_EVENT to the corresponding DDS status
static const std::unordered_map<rmw_event_type_t, uint32_t> mask_map{
  {RMW_EVENT_LIVELINESS_CHANGED, DDS_LIVELINESS_CHANGED_STATUS},
  {RMW_EVENT_REQUESTED_DEADLINE_MISSED, DDS_REQUESTED_DEADLINE_MISSED_STATUS},
  {RMW_EVENT_LIVELINESS_LOST, DDS_LIVELINESS_LOST_STATUS},
  {RMW_EVENT_OFFERED_DEADLINE_MISSED, DDS_OFFERED_DEADLINE_MISSED_STATUS},
  {RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE, DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS},
  {RMW_EVENT_OFFERED_QOS_INCOMPATIBLE, DDS_OFFERED_INCOMPATIBLE_QOS_STATUS},
};

static bool is_event_supported(const rmw_event_type_t event_t)
{
  return mask_map.count(event_t) == 1;
}

static uint32_t get_status_kind_from_rmw(const rmw_event_type_t event_t)
{
  return mask_map.at(event_t);
}

static rmw_ret_t init_rmw_event(
  rmw_event_t * rmw_event, const char * topic_endpoint_impl_identifier, void * data,
  rmw_event_type_t event_type)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(rmw_event, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_endpoint_impl_identifier, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(data, RMW_RET_INVALID_ARGUMENT);
  if (!is_event_supported(event_type)) {
    RMW_SET_ERROR_MSG("provided event_type is not supported by rmw_cyclonedds_cpp");
    return RMW_RET_UNSUPPORTED;
  }
  rmw_event->implementation_identifier = topic_endpoint_impl_identifier;
  rmw_event->data = data;
  rmw_event->event_type = event_type;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_publisher_event_init(
  rmw_event_t * rmw_event, const rmw_publisher_t * publisher, rmw_event_type_t event_type)
{
  RET_WRONG_IMPLID_X(publisher, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  return init_rmw_event(
    rmw_event,
    publisher->implementation_identifier,
    publisher->data,
    event_type);
}

extern "C" rmw_ret_t rmw_subscription_event_init(
  rmw_event_t * rmw_event, const rmw_subscription_t * subscription, rmw_event_type_t event_type)
{
  RET_WRONG_IMPLID_X(subscription, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  return init_rmw_event(
    rmw_event,
    subscription->implementation_identifier,
    subscription->data,
    event_type);
}

extern "C" rmw_ret_t rmw_take_event(
  const rmw_event_t * event_handle, void * event_info,
  bool * taken)
{
  RET_WRONG_IMPLID(event_handle);
  RET_NULL(taken);
  RET_NULL(event_info);
  switch (event_handle->event_type) {
    case RMW_EVENT_LIVELINESS_CHANGED: {
        auto ei = static_cast<rmw_liveliness_changed_status_t *>(event_info);
        auto sub = static_cast<CddsSubscription *>(event_handle->data);
        dds_liveliness_changed_status_t st;
        if (dds_get_liveliness_changed_status(sub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        } else {
          ei->alive_count = static_cast<int32_t>(st.alive_count);
          ei->not_alive_count = static_cast<int32_t>(st.not_alive_count);
          ei->alive_count_change = st.alive_count_change;
          ei->not_alive_count_change = st.not_alive_count_change;
          *taken = true;
          return RMW_RET_OK;
        }
      }

    case RMW_EVENT_REQUESTED_DEADLINE_MISSED: {
        auto ei = static_cast<rmw_requested_deadline_missed_status_t *>(event_info);
        auto sub = static_cast<CddsSubscription *>(event_handle->data);
        dds_requested_deadline_missed_status_t st;
        if (dds_get_requested_deadline_missed_status(sub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        } else {
          ei->total_count = static_cast<int32_t>(st.total_count);
          ei->total_count_change = st.total_count_change;
          *taken = true;
          return RMW_RET_OK;
        }
      }

    case RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE: {
        auto ei = static_cast<rmw_requested_qos_incompatible_event_status_t *>(event_info);
        auto sub = static_cast<CddsSubscription *>(event_handle->data);
        dds_requested_incompatible_qos_status_t st;
        if (dds_get_requested_incompatible_qos_status(sub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        } else {
          ei->total_count = static_cast<int32_t>(st.total_count);
          ei->total_count_change = st.total_count_change;
          ei->last_policy_kind = dds_qos_policy_to_rmw_qos_policy(
            static_cast<dds_qos_policy_id_t>(st.last_policy_id));
          *taken = true;
          return RMW_RET_OK;
        }
      }

    case RMW_EVENT_LIVELINESS_LOST: {
        auto ei = static_cast<rmw_liveliness_lost_status_t *>(event_info);
        auto pub = static_cast<CddsPublisher *>(event_handle->data);
        dds_liveliness_lost_status_t st;
        if (dds_get_liveliness_lost_status(pub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        } else {
          ei->total_count = static_cast<int32_t>(st.total_count);
          ei->total_count_change = st.total_count_change;
          *taken = true;
          return RMW_RET_OK;
        }
      }

    case RMW_EVENT_OFFERED_DEADLINE_MISSED: {
        auto ei = static_cast<rmw_offered_deadline_missed_status_t *>(event_info);
        auto pub = static_cast<CddsPublisher *>(event_handle->data);
        dds_offered_deadline_missed_status_t st;
        if (dds_get_offered_deadline_missed_status(pub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        } else {
          ei->total_count = static_cast<int32_t>(st.total_count);
          ei->total_count_change = st.total_count_change;
          *taken = true;
          return RMW_RET_OK;
        }
      }

    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE: {
        auto ei = static_cast<rmw_offered_qos_incompatible_event_status_t *>(event_info);
        auto pub = static_cast<CddsPublisher *>(event_handle->data);
        dds_offered_incompatible_qos_status_t st;
        if (dds_get_offered_incompatible_qos_status(pub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        } else {
          ei->total_count = static_cast<int32_t>(st.total_count);
          ei->total_count_change = st.total_count_change;
          ei->last_policy_kind = dds_qos_policy_to_rmw_qos_policy(
            static_cast<dds_qos_policy_id_t>(st.last_policy_id));
          *taken = true;
          return RMW_RET_OK;
        }
      }

    case RMW_EVENT_INVALID: {
        break;
      }

    default:
      rmw_cyclonedds_cpp::unreachable();
  }
  *taken = false;
  return RMW_RET_ERROR;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    GUARDS AND WAITSETS                                            ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

static rmw_guard_condition_t * create_guard_condition()
{
  rmw_guard_condition_t * guard_condition_handle;
  auto * gcond_impl = new CddsGuardCondition();
  if ((gcond_impl->gcondh = dds_create_guardcondition(DDS_CYCLONEDDS_HANDLE)) < 0) {
    RMW_SET_ERROR_MSG("failed to create guardcondition");
    goto fail_guardcond;
  }
  guard_condition_handle = new rmw_guard_condition_t;
  guard_condition_handle->implementation_identifier = eclipse_cyclonedds_identifier;
  guard_condition_handle->data = gcond_impl;
  return guard_condition_handle;

fail_guardcond:
  delete (gcond_impl);
  return nullptr;
}

extern "C" rmw_guard_condition_t * rmw_create_guard_condition(rmw_context_t * context)
{
  (void)context;
  return create_guard_condition();
}

static rmw_ret_t destroy_guard_condition(rmw_guard_condition_t * guard_condition_handle)
{
  RET_NULL(guard_condition_handle);
  auto * gcond_impl = static_cast<CddsGuardCondition *>(guard_condition_handle->data);
  clean_waitset_caches();
  dds_delete(gcond_impl->gcondh);
  delete gcond_impl;
  delete guard_condition_handle;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t * guard_condition_handle)
{
  return destroy_guard_condition(guard_condition_handle);
}

extern "C" rmw_ret_t rmw_trigger_guard_condition(
  const rmw_guard_condition_t * guard_condition_handle)
{
  RET_WRONG_IMPLID(guard_condition_handle);
  auto * gcond_impl = static_cast<CddsGuardCondition *>(guard_condition_handle->data);
  dds_set_guardcondition(gcond_impl->gcondh, true);
  return RMW_RET_OK;
}

extern "C" rmw_wait_set_t * rmw_create_wait_set(rmw_context_t * context, size_t max_conditions)
{
  (void)context;
  (void)max_conditions;
  rmw_wait_set_t * wait_set = rmw_wait_set_allocate();
  CddsWaitset * ws = nullptr;
  RET_ALLOC_X(wait_set, goto fail_alloc_wait_set);
  wait_set->implementation_identifier = eclipse_cyclonedds_identifier;
  wait_set->data = rmw_allocate(sizeof(CddsWaitset));
  RET_ALLOC_X(wait_set->data, goto fail_alloc_wait_set_data);
  // This should default-construct the fields of CddsWaitset
  ws = static_cast<CddsWaitset *>(wait_set->data);
  // cppcheck-suppress syntaxError
  RMW_TRY_PLACEMENT_NEW(ws, ws, goto fail_placement_new, CddsWaitset, );
  if (!ws) {
    RMW_SET_ERROR_MSG("failed to construct wait set info struct");
    goto fail_ws;
  }
  ws->inuse = false;
  ws->nelems = 0;

  if ((ws->waitseth = dds_create_waitset(DDS_CYCLONEDDS_HANDLE)) < 0) {
    RMW_SET_ERROR_MSG("failed to create waitset");
    goto fail_waitset;
  }

  {
    std::lock_guard<std::mutex> lock(gcdds.lock);
    // Lazily create dummy guard condition
    if (gcdds.waitsets.size() == 0) {
      if ((gcdds.gc_for_empty_waitset = dds_create_guardcondition(DDS_CYCLONEDDS_HANDLE)) < 0) {
        RMW_SET_ERROR_MSG("failed to create guardcondition for handling empty waitsets");
        goto fail_create_dummy;
      }
    }
    // Attach never-triggered guard condition.  As it will never be triggered, it will never be
    // included in the result of dds_waitset_wait
    if (dds_waitset_attach(ws->waitseth, gcdds.gc_for_empty_waitset, INTPTR_MAX) < 0) {
      RMW_SET_ERROR_MSG("failed to attach dummy guard condition for blocking on empty waitset");
      goto fail_attach_dummy;
    }
    gcdds.waitsets.insert(ws);
  }

  return wait_set;

fail_attach_dummy:
fail_create_dummy:
  dds_delete(ws->waitseth);
fail_waitset:
fail_ws:
  RMW_TRY_DESTRUCTOR_FROM_WITHIN_FAILURE(ws->~CddsWaitset(), ws);
fail_placement_new:
  rmw_free(wait_set->data);
fail_alloc_wait_set_data:
  rmw_wait_set_free(wait_set);
fail_alloc_wait_set:
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t * wait_set)
{
  RET_WRONG_IMPLID(wait_set);
  auto result = RMW_RET_OK;
  auto ws = static_cast<CddsWaitset *>(wait_set->data);
  RET_NULL(ws);
  dds_delete(ws->waitseth);
  {
    std::lock_guard<std::mutex> lock(gcdds.lock);
    gcdds.waitsets.erase(ws);
    if (gcdds.waitsets.size() == 0) {
      dds_delete(gcdds.gc_for_empty_waitset);
      gcdds.gc_for_empty_waitset = 0;
    }
  }
  RMW_TRY_DESTRUCTOR(ws->~CddsWaitset(), ws, result = RMW_RET_ERROR);
  rmw_free(wait_set->data);
  rmw_wait_set_free(wait_set);
  return result;
}

template<typename T>
static bool require_reattach(const std::vector<T *> & cached, size_t count, void ** ary)
{
  if (ary == nullptr || count == 0) {
    return cached.size() != 0;
  } else if (count != cached.size()) {
    return true;
  } else {
    return memcmp(
      static_cast<const void *>(cached.data()), static_cast<void *>(ary),
      count * sizeof(void *)) != 0;
  }
}

static bool require_reattach(
  const std::vector<CddsEvent> & cached, rmw_events_t * events)
{
  if (events == nullptr || events->event_count == 0) {
    return cached.size() != 0;
  } else if (events->event_count != cached.size()) {
    return true;
  } else {
    for (size_t i = 0; i < events->event_count; ++i) {
      rmw_event_t * current_event = static_cast<rmw_event_t *>(events->events[i]);
      CddsEvent c = cached.at(i);
      if (c.enth != static_cast<CddsEntity *>(current_event->data)->enth ||
        c.event_type != current_event->event_type)
      {
        return true;
      }
    }
    return false;
  }
}

static void waitset_detach(CddsWaitset * ws)
{
  for (auto && x : ws->subs) {
    dds_waitset_detach(ws->waitseth, x->rdcondh);
  }
  for (auto && x : ws->gcs) {
    dds_waitset_detach(ws->waitseth, x->gcondh);
  }
  for (auto && x : ws->srvs) {
    dds_waitset_detach(ws->waitseth, x->service.sub->rdcondh);
  }
  for (auto && x : ws->cls) {
    dds_waitset_detach(ws->waitseth, x->client.sub->rdcondh);
  }
  ws->subs.resize(0);
  ws->gcs.resize(0);
  ws->srvs.resize(0);
  ws->cls.resize(0);
  ws->nelems = 0;
}

static void clean_waitset_caches()
{
  /* Called whenever a subscriber, guard condition, service or client is deleted (as these may
     have been cached in a waitset), and drops all cached entities from all waitsets (just to keep
     life simple). I'm assuming one is not allowed to delete an entity while it is still being
     used ... */
  std::lock_guard<std::mutex> lock(gcdds.lock);
  for (auto && ws : gcdds.waitsets) {
    std::lock_guard<std::mutex> wslock(ws->lock);
    if (!ws->inuse) {
      waitset_detach(ws);
    }
  }
}

static rmw_ret_t gather_event_entities(
  const rmw_events_t * events,
  std::unordered_set<dds_entity_t> & entities)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(events, RMW_RET_INVALID_ARGUMENT);

  std::unordered_map<dds_entity_t, uint32_t> status_mask_map;

  for (size_t i = 0; i < events->event_count; ++i) {
    rmw_event_t * current_event = static_cast<rmw_event_t *>(events->events[i]);
    dds_entity_t dds_entity = static_cast<CddsEntity *>(current_event->data)->enth;
    if (dds_entity <= 0) {
      RMW_SET_ERROR_MSG("Event entity handle is invalid");
      return RMW_RET_ERROR;
    }

    if (is_event_supported(current_event->event_type)) {
      if (status_mask_map.find(dds_entity) == status_mask_map.end()) {
        status_mask_map[dds_entity] = 0;
      }
      status_mask_map[dds_entity] |= get_status_kind_from_rmw(current_event->event_type);
    }
  }
  for (auto & pair : status_mask_map) {
    // set the status condition's mask with the supported type
    dds_set_status_mask(pair.first, pair.second);
    entities.insert(pair.first);
  }

  return RMW_RET_OK;
}

static rmw_ret_t handle_active_events(rmw_events_t * events)
{
  if (events) {
    for (size_t i = 0; i < events->event_count; ++i) {
      rmw_event_t * current_event = static_cast<rmw_event_t *>(events->events[i]);
      dds_entity_t dds_entity = static_cast<CddsEntity *>(current_event->data)->enth;
      if (dds_entity <= 0) {
        RMW_SET_ERROR_MSG("Event entity handle is invalid");
        return RMW_RET_ERROR;
      }

      uint32_t status_mask;
      dds_get_status_changes(dds_entity, &status_mask);
      if (!is_event_supported(current_event->event_type) ||
        !static_cast<bool>(status_mask & get_status_kind_from_rmw(current_event->event_type)))
      {
        events->events[i] = nullptr;
      }
    }
  }
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_wait(
  rmw_subscriptions_t * subs, rmw_guard_conditions_t * gcs,
  rmw_services_t * srvs, rmw_clients_t * cls, rmw_events_t * evs,
  rmw_wait_set_t * wait_set, const rmw_time_t * wait_timeout)
{
  RET_NULL(wait_set);
  CddsWaitset * ws = static_cast<CddsWaitset *>(wait_set->data);
  RET_NULL(ws);

  {
    std::lock_guard<std::mutex> lock(ws->lock);
    if (ws->inuse) {
      RMW_SET_ERROR_MSG("concurrent calls to rmw_wait on a single waitset is not supported");
      return RMW_RET_ERROR;
    }
    ws->inuse = true;
  }

  if (require_reattach(
      ws->subs, subs ? subs->subscriber_count : 0,
      subs ? subs->subscribers : nullptr) ||
    require_reattach(
      ws->gcs, gcs ? gcs->guard_condition_count : 0,
      gcs ? gcs->guard_conditions : nullptr) ||
    require_reattach(ws->srvs, srvs ? srvs->service_count : 0, srvs ? srvs->services : nullptr) ||
    require_reattach(ws->cls, cls ? cls->client_count : 0, cls ? cls->clients : nullptr) ||
    require_reattach(ws->evs, evs))
  {
    size_t nelems = 0;
    waitset_detach(ws);
#define ATTACH(type, var, name, cond) do { \
    ws->var.resize(0); \
    if (var) { \
      ws->var.reserve(var->name ## _count); \
      for (size_t i = 0; i < var->name ## _count; i++) { \
        auto x = static_cast<type *>(var->name ## s[i]); \
        ws->var.push_back(x); \
        dds_waitset_attach(ws->waitseth, x->cond, nelems); \
        nelems++; \
      } \
    } \
} \
  while (0)
    ATTACH(CddsSubscription, subs, subscriber, rdcondh);
    ATTACH(CddsGuardCondition, gcs, guard_condition, gcondh);
    ATTACH(CddsService, srvs, service, service.sub->rdcondh);
    ATTACH(CddsClient, cls, client, client.sub->rdcondh);
#undef ATTACH

    ws->evs.resize(0);
    if (evs) {
      std::unordered_set<dds_entity_t> event_entities;
      rmw_ret_t ret_code = gather_event_entities(evs, event_entities);
      if (ret_code != RMW_RET_OK) {
        return ret_code;
      }
      for (auto e : event_entities) {
        dds_waitset_attach(ws->waitseth, e, nelems);
        nelems++;
      }
      ws->evs.reserve(evs->event_count);
      for (size_t i = 0; i < evs->event_count; i++) {
        auto current_event = static_cast<rmw_event_t *>(evs->events[i]);
        CddsEvent ev;
        ev.enth = static_cast<CddsEntity *>(current_event->data)->enth;
        ev.event_type = current_event->event_type;
        ws->evs.push_back(ev);
      }
    }

    ws->nelems = nelems;
  }

  ws->trigs.resize(ws->nelems + 1);
  const dds_time_t timeout =
    (wait_timeout == NULL) ?
    DDS_NEVER :
    (dds_time_t) wait_timeout->sec * 1000000000 + wait_timeout->nsec;
  ws->trigs.resize(ws->nelems + 1);
  const dds_return_t ntrig = dds_waitset_wait(
    ws->waitseth, ws->trigs.data(),
    ws->trigs.size(), timeout);
  ws->trigs.resize(ntrig);
  std::sort(ws->trigs.begin(), ws->trigs.end());
  ws->trigs.push_back((dds_attach_t) -1);

  {
    dds_attach_t trig_idx = 0;
    bool dummy;
    size_t nelems = 0;
#define DETACH(type, var, name, cond, on_triggered) do { \
    if (var) { \
      for (size_t i = 0; i < var->name ## _count; i++) { \
        auto x = static_cast<type *>(var->name ## s[i]); \
        if (ws->trigs[trig_idx] == static_cast<dds_attach_t>(nelems)) { \
          on_triggered; \
          trig_idx++; \
        } else { \
          var->name ## s[i] = nullptr; \
        } \
        nelems++; \
      } \
    } \
} while (0)
    DETACH(CddsSubscription, subs, subscriber, rdcondh, (void) x);
    DETACH(
      CddsGuardCondition, gcs, guard_condition, gcondh,
      dds_take_guardcondition(x->gcondh, &dummy));
    DETACH(CddsService, srvs, service, service.sub->rdcondh, (void) x);
    DETACH(CddsClient, cls, client, client.sub->rdcondh, (void) x);
#undef DETACH
    handle_active_events(evs);
  }

#if REPORT_BLOCKED_REQUESTS
  for (auto const & c : ws->cls) {
    check_for_blocked_requests(*c);
  }
#endif

  {
    std::lock_guard<std::mutex> lock(ws->lock);
    ws->inuse = false;
  }

  return (ws->trigs.size() == 1) ? RMW_RET_TIMEOUT : RMW_RET_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    CLIENTS AND SERVERS                                            ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

static rmw_ret_t rmw_take_response_request(
  CddsCS * cs, rmw_service_info_t * request_header,
  void * ros_data, bool * taken, dds_time_t * source_timestamp,
  dds_instance_handle_t srcfilter)
{
  RET_NULL(taken);
  RET_NULL(ros_data);
  RET_NULL(request_header);
  cdds_request_wrapper_t wrap;
  dds_sample_info_t info;
  wrap.data = ros_data;
  void * wrap_ptr = static_cast<void *>(&wrap);
  while (dds_take(cs->sub->enth, &wrap_ptr, &info, 1, 1) == 1) {
    if (info.valid_data) {
      memset(request_header, 0, sizeof(wrap.header));
      assert(sizeof(wrap.header.guid) <= sizeof(request_header->request_id.writer_guid));
      memcpy(request_header->request_id.writer_guid, &wrap.header.guid, sizeof(wrap.header.guid));
      request_header->request_id.sequence_number = wrap.header.seq;
      request_header->source_timestamp = info.source_timestamp;
      // TODO(iluetkeb) replace with real received timestamp when available in cyclone
      request_header->received_timestamp = 0;
      if (source_timestamp) {
        *source_timestamp = info.source_timestamp;
      }
      if (srcfilter == 0 || srcfilter == wrap.header.guid) {
        *taken = true;
        return RMW_RET_OK;
      }
    }
  }
  *taken = false;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_take_response(
  const rmw_client_t * client,
  rmw_service_info_t * request_header, void * ros_response,
  bool * taken)
{
  RET_WRONG_IMPLID(client);
  auto info = static_cast<CddsClient *>(client->data);
  dds_time_t source_timestamp;
  rmw_ret_t ret = rmw_take_response_request(
    &info->client, request_header, ros_response, taken,
    &source_timestamp, info->client.pub->pubiid);

#if REPORT_BLOCKED_REQUESTS
  if (ret == RMW_RET_OK && *taken) {
    std::lock_guard<std::mutex> lock(info->lock);
    uint64_t seq = request_header->sequence_number;
    dds_time_t tnow = dds_time();
    dds_time_t dtresp = tnow - source_timestamp;
    dds_time_t dtreq = tnow - info->reqtime[seq];
    if (dtreq > DDS_MSECS(REPORT_LATE_MESSAGES) || dtresp > DDS_MSECS(REPORT_LATE_MESSAGES)) {
      fprintf(
        stderr, "** response time %.fms; response in history for %.fms\n",
        static_cast<double>(dtreq) / 1e6, static_cast<double>(dtresp) / 1e6);
    }
    info->reqtime.erase(seq);
  }
#endif
  return ret;
}

#if REPORT_BLOCKED_REQUESTS
static void check_for_blocked_requests(CddsClient & client)
{
  dds_time_t tnow = dds_time();
  std::lock_guard<std::mutex> lock(client.lock);
  if (tnow > client.lastcheck + DDS_SECS(1)) {
    client.lastcheck = tnow;
    for (auto const & r : client.reqtime) {
      dds_time_t dt = tnow - r.second;
      if (dt > DDS_SECS(1)) {
        fprintf(stderr, "** already waiting for %.fms\n", static_cast<double>(dt) / 1e6);
      }
    }
  }
}
#endif

extern "C" rmw_ret_t rmw_take_request(
  const rmw_service_t * service,
  rmw_service_info_t * request_header, void * ros_request,
  bool * taken)
{
  RET_WRONG_IMPLID(service);
  auto info = static_cast<CddsService *>(service->data);
  return rmw_take_response_request(&info->service, request_header, ros_request, taken, nullptr, 0);
}

static rmw_ret_t rmw_send_response_request(
  CddsCS * cs, const cdds_request_header_t & header,
  const void * ros_data)
{
  const cdds_request_wrapper_t wrap = {header, const_cast<void *>(ros_data)};
  if (dds_write(cs->pub->enth, static_cast<const void *>(&wrap)) >= 0) {
    return RMW_RET_OK;
  } else {
    RMW_SET_ERROR_MSG("cannot publish data");
    return RMW_RET_ERROR;
  }
}

extern "C" rmw_ret_t rmw_send_response(
  const rmw_service_t * service,
  rmw_request_id_t * request_header, void * ros_response)
{
  RET_WRONG_IMPLID(service);
  RET_NULL(request_header);
  RET_NULL(ros_response);
  CddsService * info = static_cast<CddsService *>(service->data);
  cdds_request_header_t header;
  memcpy(&header.guid, request_header->writer_guid, sizeof(header.guid));
  header.seq = request_header->sequence_number;
  return rmw_send_response_request(&info->service, header, ros_response);
}

extern "C" rmw_ret_t rmw_send_request(
  const rmw_client_t * client, const void * ros_request,
  int64_t * sequence_id)
{
  static std::atomic_uint next_request_id;
  RET_WRONG_IMPLID(client);
  RET_NULL(ros_request);
  RET_NULL(sequence_id);
  auto info = static_cast<CddsClient *>(client->data);
  cdds_request_header_t header;
  header.guid = info->client.pub->pubiid;
  header.seq = *sequence_id = ++next_request_id;

#if REPORT_BLOCKED_REQUESTS
  {
    std::lock_guard<std::mutex> lock(info->lock);
    info->reqtime[header.seq] = dds_time();
  }
#endif

  return rmw_send_response_request(&info->client, header, ros_request);
}

static const rosidl_service_type_support_t * get_service_typesupport(
  const rosidl_service_type_support_t * type_supports)
{
  const rosidl_service_type_support_t * ts;
  if ((ts =
    get_service_typesupport_handle(
      type_supports, rosidl_typesupport_introspection_c__identifier)) != nullptr)
  {
    return ts;
  } else {
    if ((ts =
      get_service_typesupport_handle(
        type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr)
    {
      return ts;
    } else {
      RMW_SET_ERROR_MSG("service type support not from this implementation");
      return nullptr;
    }
  }
}

static rmw_ret_t rmw_init_cs(
  CddsCS * cs, const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name, const rmw_qos_profile_t * qos_policies,
  bool is_service)
{
  RET_WRONG_IMPLID(node);
  RET_NULL_OR_EMPTYSTR(service_name);
  RET_NULL(qos_policies);
  const rosidl_service_type_support_t * type_support = get_service_typesupport(type_supports);
  RET_NULL(type_support);

  auto pub = new CddsPublisher();
  auto sub = new CddsSubscription();
  std::string subtopic_name, pubtopic_name;
  void * pub_type_support, * sub_type_support;

  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> pub_msg_ts, sub_msg_ts;

  if (is_service) {
    std::tie(sub_msg_ts, pub_msg_ts) =
      rmw_cyclonedds_cpp::make_request_response_value_types(type_supports);

    sub_type_support = create_request_type_support(
      type_support->data, type_support->typesupport_identifier);
    pub_type_support = create_response_type_support(
      type_support->data, type_support->typesupport_identifier);
    subtopic_name =
      make_fqtopic(ROS_SERVICE_REQUESTER_PREFIX, service_name, "Request", qos_policies);
    pubtopic_name = make_fqtopic(ROS_SERVICE_RESPONSE_PREFIX, service_name, "Reply", qos_policies);
  } else {
    std::tie(pub_msg_ts, sub_msg_ts) =
      rmw_cyclonedds_cpp::make_request_response_value_types(type_supports);

    pub_type_support = create_request_type_support(
      type_support->data, type_support->typesupport_identifier);
    sub_type_support = create_response_type_support(
      type_support->data, type_support->typesupport_identifier);
    pubtopic_name =
      make_fqtopic(ROS_SERVICE_REQUESTER_PREFIX, service_name, "Request", qos_policies);
    subtopic_name = make_fqtopic(ROS_SERVICE_RESPONSE_PREFIX, service_name, "Reply", qos_policies);
  }

  RCUTILS_LOG_DEBUG_NAMED(
    "rmw_cyclonedds_cpp", "************ %s Details *********",
    is_service ? "Service" : "Client");
  RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "Sub Topic %s", subtopic_name.c_str());
  RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "Pub Topic %s", pubtopic_name.c_str());
  RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "***********");

  dds_entity_t pubtopic, subtopic;
  struct sertopic_rmw * pub_st, * sub_st;

  pub_st = create_sertopic(
    pubtopic_name.c_str(), type_support->typesupport_identifier, pub_type_support, true,
    std::move(pub_msg_ts));
  struct ddsi_sertopic * pub_stact;
  pubtopic = create_topic(node->context->impl->ppant, pub_st, &pub_stact);
  if (pubtopic < 0) {
    RMW_SET_ERROR_MSG("failed to create topic");
    goto fail_pubtopic;
  }

  sub_st = create_sertopic(
    subtopic_name.c_str(), type_support->typesupport_identifier, sub_type_support, true,
    std::move(sub_msg_ts));
  subtopic = create_topic(node->context->impl->ppant, sub_st);
  if (subtopic < 0) {
    RMW_SET_ERROR_MSG("failed to create topic");
    goto fail_subtopic;
  }
  dds_qos_t * qos;
  if ((qos = dds_create_qos()) == nullptr) {
    goto fail_qos;
  }
  dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_SECS(1));
  dds_qset_history(qos, DDS_HISTORY_KEEP_ALL, DDS_LENGTH_UNLIMITED);
  if ((pub->enth = dds_create_writer(node->context->impl->dds_pub, pubtopic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create writer");
    goto fail_writer;
  }
  get_entity_gid(pub->enth, pub->gid);
  pub->sertopic = pub_stact;
  if ((sub->enth = dds_create_reader(node->context->impl->dds_sub, subtopic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create reader");
    goto fail_reader;
  }
  get_entity_gid(sub->enth, sub->gid);
  if ((sub->rdcondh = dds_create_readcondition(sub->enth, DDS_ANY_STATE)) < 0) {
    RMW_SET_ERROR_MSG("failed to create readcondition");
    goto fail_readcond;
  }
  if (dds_get_instance_handle(pub->enth, &pub->pubiid) < 0) {
    RMW_SET_ERROR_MSG("failed to get instance handle for writer");
    goto fail_instance_handle;
  }
  dds_delete_qos(qos);
  dds_delete(subtopic);
  dds_delete(pubtopic);

  cs->pub = pub;
  cs->sub = sub;
  return RMW_RET_OK;

fail_instance_handle:
  dds_delete(sub->rdcondh);
fail_readcond:
  dds_delete(sub->enth);
fail_reader:
  dds_delete(pub->enth);
fail_writer:
  dds_delete_qos(qos);
fail_qos:
  dds_delete(subtopic);
fail_subtopic:
  dds_delete(pubtopic);
fail_pubtopic:
  return RMW_RET_ERROR;
}

static void rmw_fini_cs(CddsCS * cs)
{
  dds_delete(cs->sub->rdcondh);
  dds_delete(cs->sub->enth);
  dds_delete(cs->pub->enth);
}

static rmw_ret_t destroy_client(const rmw_node_t * node, rmw_client_t * client)
{
  RET_WRONG_IMPLID(node);
  RET_WRONG_IMPLID(client);
  auto info = static_cast<CddsClient *>(client->data);
  clean_waitset_caches();

  {
    // Update graph
    auto common = &node->context->impl->common;
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    static_cast<void>(common->graph_cache.dissociate_writer(
      info->client.pub->gid, common->gid,
      node->name, node->namespace_));
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.dissociate_reader(
      info->client.sub->gid, common->gid, node->name,
      node->namespace_);
    if (RMW_RET_OK != rmw_publish(
        common->pub,
        static_cast<void *>(&msg),
        nullptr))
    {
      RMW_SET_ERROR_MSG("failed to publish ParticipantEntitiesInfo when destroying service");
    }
  }

  rmw_fini_cs(&info->client);
  rmw_free(const_cast<char *>(client->service_name));
  rmw_client_free(client);
  return RMW_RET_OK;
}

extern "C" rmw_client_t * rmw_create_client(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name,
  const rmw_qos_profile_t * qos_policies)
{
  CddsClient * info = new CddsClient();
#if REPORT_BLOCKED_REQUESTS
  info->lastcheck = 0;
#endif
  if (
    rmw_init_cs(
      &info->client, node, type_supports, service_name, qos_policies, false) != RMW_RET_OK)
  {
    delete (info);
    return nullptr;
  }
  rmw_client_t * rmw_client = rmw_client_allocate();
  RET_NULL_X(rmw_client, goto fail_client);
  rmw_client->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_client->data = info;
  rmw_client->service_name = reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
  RET_NULL_X(rmw_client->service_name, goto fail_service_name);
  memcpy(const_cast<char *>(rmw_client->service_name), service_name, strlen(service_name) + 1);

  {
    // Update graph
    auto common = &node->context->impl->common;
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    static_cast<void>(common->graph_cache.associate_writer(
      info->client.pub->gid, common->gid,
      node->name, node->namespace_));
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.associate_reader(
      info->client.sub->gid, common->gid, node->name,
      node->namespace_);
    if (RMW_RET_OK != rmw_publish(
        common->pub,
        static_cast<void *>(&msg),
        nullptr))
    {
      static_cast<void>(destroy_client(node, rmw_client));
      return nullptr;
    }
  }

  return rmw_client;
fail_service_name:
  rmw_client_free(rmw_client);
fail_client:
  rmw_fini_cs(&info->client);
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_client(rmw_node_t * node, rmw_client_t * client)
{
  return destroy_client(node, client);
}

static rmw_ret_t destroy_service(const rmw_node_t * node, rmw_service_t * service)
{
  RET_WRONG_IMPLID(node);
  RET_WRONG_IMPLID(service);
  auto info = static_cast<CddsService *>(service->data);
  clean_waitset_caches();

  {
    // Update graph
    auto common = &node->context->impl->common;
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    static_cast<void>(common->graph_cache.dissociate_writer(
      info->service.pub->gid, common->gid,
      node->name, node->namespace_));
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.dissociate_reader(
      info->service.sub->gid, common->gid, node->name,
      node->namespace_);
    if (RMW_RET_OK != rmw_publish(
        common->pub,
        static_cast<void *>(&msg),
        nullptr))
    {
      RMW_SET_ERROR_MSG("failed to publish ParticipantEntitiesInfo when destroying service");
    }
  }

  rmw_fini_cs(&info->service);
  rmw_free(const_cast<char *>(service->service_name));
  rmw_service_free(service);
  return RMW_RET_OK;
}

extern "C" rmw_service_t * rmw_create_service(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name,
  const rmw_qos_profile_t * qos_policies)
{
  CddsService * info = new CddsService();
  if (
    rmw_init_cs(
      &info->service, node, type_supports, service_name, qos_policies, true) != RMW_RET_OK)
  {
    delete (info);
    return nullptr;
  }
  rmw_service_t * rmw_service = rmw_service_allocate();
  RET_NULL_X(rmw_service, goto fail_service);
  rmw_service->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_service->data = info;
  rmw_service->service_name =
    reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
  RET_NULL_X(rmw_service->service_name, goto fail_service_name);
  memcpy(const_cast<char *>(rmw_service->service_name), service_name, strlen(service_name) + 1);

  {
    // Update graph
    auto common = &node->context->impl->common;
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    static_cast<void>(common->graph_cache.associate_writer(
      info->service.pub->gid, common->gid,
      node->name, node->namespace_));
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.associate_reader(
      info->service.sub->gid, common->gid, node->name,
      node->namespace_);
    if (RMW_RET_OK != rmw_publish(
        common->pub,
        static_cast<void *>(&msg),
        nullptr))
    {
      static_cast<void>(destroy_service(node, rmw_service));
      return nullptr;
    }
  }

  return rmw_service;
fail_service_name:
  rmw_service_free(rmw_service);
fail_service:
  rmw_fini_cs(&info->service);
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_service(rmw_node_t * node, rmw_service_t * service)
{
  return destroy_service(node, service);
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    INTROSPECTION                                                  ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_get_node_names(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces)
{
  RET_WRONG_IMPLID(node);
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_names) ||
    RMW_RET_OK != rmw_check_zero_rmw_string_array(node_namespaces))
  {
    return RMW_RET_ERROR;
  }

  auto common_context = &node->context->impl->common;
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  return common_context->graph_cache.get_node_names(
    node_names,
    node_namespaces,
    nullptr,
    &allocator);
}

extern "C" rmw_ret_t rmw_get_node_names_with_enclaves(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces,
  rcutils_string_array_t * enclaves)
{
  RET_WRONG_IMPLID(node);
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_names) ||
    RMW_RET_OK != rmw_check_zero_rmw_string_array(node_namespaces))
  {
    return RMW_RET_ERROR;
  }

  auto common_context = &node->context->impl->common;
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  return common_context->graph_cache.get_node_names(
    node_names,
    node_namespaces,
    enclaves,
    &allocator);
}

extern "C" rmw_ret_t rmw_get_topic_names_and_types(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  bool no_demangle, rmw_names_and_types_t * tptyp)
{
  RET_WRONG_IMPLID(node);
  RET_NULL(allocator);
  rmw_ret_t ret = rmw_names_and_types_check_zero(tptyp);
  if (ret != RMW_RET_OK) {
    return ret;
  }

  DemangleFunction demangle_topic = _demangle_ros_topic_from_topic;
  DemangleFunction demangle_type = _demangle_if_ros_type;
  if (no_demangle) {
    demangle_topic = _identity_demangle;
    demangle_type = _identity_demangle;
  }
  auto common_context = &node->context->impl->common;
  return common_context->graph_cache.get_names_and_types(
    demangle_topic,
    demangle_type,
    allocator,
    tptyp);
}

extern "C" rmw_ret_t rmw_get_service_names_and_types(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  rmw_names_and_types_t * sntyp)
{
  auto common_context = &node->context->impl->common;
  return common_context->graph_cache.get_names_and_types(
    _demangle_service_from_topic,
    _demangle_service_type_only,
    allocator,
    sntyp);
}

static rmw_ret_t get_topic_name(dds_entity_t endpoint_handle, std::string & name)
{
  /* dds_get_name needs a bit of TLC ... */
  std::vector<char> tmp(128);
  do {
    if (dds_get_name(dds_get_topic(endpoint_handle), tmp.data(), tmp.size()) < 0) {
      return RMW_RET_ERROR;
    }
    auto end = std::find(tmp.begin(), tmp.end(), 0);
    if (end != tmp.end()) {
      name = std::string(tmp.begin(), end);
      return RMW_RET_OK;
    }
    tmp.resize(2 * tmp.size());
  } while (true);
}

extern "C" rmw_ret_t rmw_service_server_is_available(
  const rmw_node_t * node,
  const rmw_client_t * client,
  bool * is_available)
{
  RET_WRONG_IMPLID(node);
  RET_WRONG_IMPLID(client);
  RET_NULL(is_available);
  *is_available = false;

  auto info = static_cast<CddsClient *>(client->data);
  auto common_context = &node->context->impl->common;

  std::string sub_topic_name, pub_topic_name;
  if (get_topic_name(info->client.pub->enth, pub_topic_name) < 0 ||
    get_topic_name(info->client.sub->enth, sub_topic_name) < 0)
  {
    RMW_SET_ERROR_MSG("rmw_service_server_is_available: failed to get topic names");
    return RMW_RET_ERROR;
  }

  size_t number_of_request_subscribers = 0;
  rmw_ret_t ret =
    common_context->graph_cache.get_reader_count(pub_topic_name, &number_of_request_subscribers);
  if (ret != RMW_RET_OK || 0 == number_of_request_subscribers) {
    return ret;
  }
  size_t number_of_response_publishers = 0;
  ret =
    common_context->graph_cache.get_writer_count(sub_topic_name, &number_of_response_publishers);
  if (ret != RMW_RET_OK || 0 == number_of_response_publishers) {
    // error
    return ret;
  }
  dds_publication_matched_status_t ps;
  dds_subscription_matched_status_t cs;
  if (dds_get_publication_matched_status(info->client.pub->enth, &ps) < 0 ||
    dds_get_subscription_matched_status(info->client.sub->enth, &cs) < 0)
  {
    RMW_SET_ERROR_MSG("rmw_service_server_is_available: get_..._matched_status failed");
  }
  // all conditions met, there is a service server available
  *is_available = true;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_count_publishers(
  const rmw_node_t * node, const char * topic_name,
  size_t * count)
{
  auto common_context = &node->context->impl->common;
  const std::string mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
  return common_context->graph_cache.get_writer_count(mangled_topic_name, count);
}

extern "C" rmw_ret_t rmw_count_subscribers(
  const rmw_node_t * node, const char * topic_name,
  size_t * count)
{
  auto common_context = &node->context->impl->common;
  const std::string mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
  return common_context->graph_cache.get_reader_count(mangled_topic_name, count);
}

using GetNamesAndTypesByNodeFunction = rmw_ret_t (*)(
  rmw_dds_common::Context *,
  const std::string &,
  const std::string &,
  DemangleFunction,
  DemangleFunction,
  rcutils_allocator_t *,
  rmw_names_and_types_t *);

static rmw_ret_t get_topic_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  DemangleFunction demangle_topic,
  DemangleFunction demangle_type,
  bool no_demangle,
  GetNamesAndTypesByNodeFunction get_names_and_types_by_node,
  rmw_names_and_types_t * topic_names_and_types)
{
  RET_WRONG_IMPLID(node);
  RET_NULL(allocator);
  RET_NULL(node_name);
  RET_NULL(node_namespace);
  RET_NULL(topic_names_and_types);
  auto common_context = &node->context->impl->common;
  if (no_demangle) {
    demangle_topic = _identity_demangle;
    demangle_type = _identity_demangle;
  }
  return get_names_and_types_by_node(
    common_context,
    node_name,
    node_namespace,
    demangle_topic,
    demangle_type,
    allocator,
    topic_names_and_types);
}

static rmw_ret_t get_reader_names_and_types_by_node(
  rmw_dds_common::Context * common_context,
  const std::string & node_name,
  const std::string & node_namespace,
  DemangleFunction demangle_topic,
  DemangleFunction demangle_type,
  rcutils_allocator_t * allocator,
  rmw_names_and_types_t * topic_names_and_types)
{
  return common_context->graph_cache.get_reader_names_and_types_by_node(
    node_name,
    node_namespace,
    demangle_topic,
    demangle_type,
    allocator,
    topic_names_and_types);
}

static rmw_ret_t get_writer_names_and_types_by_node(
  rmw_dds_common::Context * common_context,
  const std::string & node_name,
  const std::string & node_namespace,
  DemangleFunction demangle_topic,
  DemangleFunction demangle_type,
  rcutils_allocator_t * allocator,
  rmw_names_and_types_t * topic_names_and_types)
{
  return common_context->graph_cache.get_writer_names_and_types_by_node(
    node_name,
    node_namespace,
    demangle_topic,
    demangle_type,
    allocator,
    topic_names_and_types);
}

extern "C" rmw_ret_t rmw_get_subscriber_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  bool no_demangle,
  rmw_names_and_types_t * tptyp)
{
  return get_topic_names_and_types_by_node(
    node, allocator, node_name, node_namespace,
    _demangle_ros_topic_from_topic, _demangle_if_ros_type,
    no_demangle, get_reader_names_and_types_by_node, tptyp);
}

extern "C" rmw_ret_t rmw_get_publisher_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  bool no_demangle,
  rmw_names_and_types_t * tptyp)
{
  return get_topic_names_and_types_by_node(
    node, allocator, node_name, node_namespace,
    _demangle_ros_topic_from_topic, _demangle_if_ros_type,
    no_demangle, get_writer_names_and_types_by_node, tptyp);
}

extern "C" rmw_ret_t rmw_get_service_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  rmw_names_and_types_t * sntyp)
{
  return get_topic_names_and_types_by_node(
    node,
    allocator,
    node_name,
    node_namespace,
    _demangle_service_request_from_topic,
    _demangle_service_type_only,
    false,
    get_reader_names_and_types_by_node,
    sntyp);
}

extern "C" rmw_ret_t rmw_get_client_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  rmw_names_and_types_t * sntyp)
{
  return get_topic_names_and_types_by_node(
    node,
    allocator,
    node_name,
    node_namespace,
    _demangle_service_reply_from_topic,
    _demangle_service_type_only,
    false,
    get_reader_names_and_types_by_node,
    sntyp);
}

extern "C" rmw_ret_t rmw_get_publishers_info_by_topic(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * topic_name,
  bool no_mangle,
  rmw_topic_endpoint_info_array_t * publishers_info)
{
  RET_WRONG_IMPLID(node);
  RET_NULL(allocator);
  RET_NULL(topic_name);
  RET_NULL(publishers_info);
  auto common_context = &node->context->impl->common;
  std::string mangled_topic_name = topic_name;
  DemangleFunction demangle_type = _identity_demangle;
  if (!no_mangle) {
    mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
    demangle_type = _demangle_if_ros_type;
  }
  return common_context->graph_cache.get_writers_info_by_topic(
    mangled_topic_name,
    demangle_type,
    allocator,
    publishers_info);
}

extern "C" rmw_ret_t rmw_get_subscriptions_info_by_topic(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * topic_name,
  bool no_mangle,
  rmw_topic_endpoint_info_array_t * subscriptions_info)
{
  RET_WRONG_IMPLID(node);
  RET_NULL(allocator);
  RET_NULL(topic_name);
  RET_NULL(subscriptions_info);
  auto common_context = &node->context->impl->common;
  std::string mangled_topic_name = topic_name;
  DemangleFunction demangle_type = _identity_demangle;
  if (!no_mangle) {
    mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
    demangle_type = _demangle_if_ros_type;
  }
  return common_context->graph_cache.get_readers_info_by_topic(
    mangled_topic_name,
    demangle_type,
    allocator,
    subscriptions_info);
}
