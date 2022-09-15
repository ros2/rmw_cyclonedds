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

#include <cassert>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <iomanip>
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

#include "rcutils/env.h"
#include "rcutils/filesystem.h"
#include "rcutils/format_string.h"
#include "rcutils/logging_macros.h"
#include "rcutils/strdup.h"

#include "rmw/allocators.h"
#include "rmw/convert_rcutils_ret_to_rmw_ret.h"
#include "rmw/error_handling.h"
#include "rmw/event.h"
#include "rmw/features.h"
#include "rmw/get_node_info_and_types.h"
#include "rmw/get_service_names_and_types.h"
#include "rmw/get_topic_names_and_types.h"
#include "rmw/event_callback_type.h"
#include "rmw/names_and_types.h"
#include "rmw/rmw.h"
#include "rmw/sanity_checks.h"
#include "rmw/validate_namespace.h"
#include "rmw/validate_node_name.h"

#include "fallthrough_macro.hpp"
#include "Serialization.hpp"
#include "rcpputils/scope_exit.hpp"
#include "rmw/impl/cpp/macros.hpp"
#include "rmw/impl/cpp/key_value.hpp"

#include "TypeSupport2.hpp"

#include "rmw_version_test.hpp"
#include "MessageTypeSupport.hpp"
#include "ServiceTypeSupport.hpp"

#include "rmw/get_topic_endpoint_info.h"
#include "rmw/incompatible_qos_events_statuses.h"
#include "rmw/topic_endpoint_info_array.h"

#include "rmw_dds_common/context.hpp"
#include "rmw_dds_common/graph_cache.hpp"
#include "rmw_dds_common/msg/participant_entities_info.hpp"
#include "rmw_dds_common/qos.hpp"
#include "rmw_dds_common/security.hpp"

#include "rosidl_typesupport_cpp/message_type_support.hpp"

#include "tracetools/tracetools.h"

#include "namespace_prefix.hpp"

#include "dds/dds.h"
#include "dds/ddsc/dds_data_allocator.h"
#include "dds/ddsc/dds_loan_api.h"
#include "serdes.hpp"
#include "serdata.hpp"
#include "demangle.hpp"

using namespace std::literals::chrono_literals;

/* Security must be enabled when compiling and requires cyclone to support QOS property lists */
#if DDS_HAS_SECURITY && DDS_HAS_PROPERTY_LIST_QOS
#define RMW_SUPPORT_SECURITY 1
#else
#define RMW_SUPPORT_SECURITY 0
#endif

#if !DDS_HAS_DDSI_SERTYPE
#define ddsi_sertype_unref(x) ddsi_sertopic_unref(x)
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
#define RET_WRONG_IMPLID(var) RET_WRONG_IMPLID_X(var, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION)
#define RET_NULL_OR_EMPTYSTR(var) RET_NULL_OR_EMPTYSTR_X(var, return RMW_RET_ERROR)
#define RET_EXPECTED(func, expected_ret, error_msg, code) do { \
    if ((expected_ret) != (func)) \
    { \
      RET_ERR_X(error_msg, code); \
    } \
} while (0)

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

/* Use construct-on-first-use for the global state rather than a plain global variable to
   prevent its destructor from running prior to last use by some other component in the
   system.  E.g., some rclcpp tests (at the time of this commit) drop a guard condition in
   a global destructor, but (at least) on Windows the Cyclone RMW global dtors run before
   the global dtors of that test, resulting in rmw_destroy_guard_condition() attempting to
   use the already destroyed "Cdds::waitsets".

   The memory leak this causes is minor (an empty map of domains and an empty set of
   waitsets) and by definition only once.  The alternative of elimating it altogether or
   tying its existence to init/shutdown is problematic because this state is used across
   domains and contexts.

   The only practical alternative I see is to extend Cyclone's run-time state (which is
   managed correctly for these situations), but it is not Cyclone's responsibility to work
   around C++ global destructor limitations. */
static Cdds & gcdds()
{
  static Cdds * x = new Cdds();
  return *x;
}

struct CddsEntity
{
  dds_entity_t enth;
};

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

// Definition of struct rmw_context_impl_s as declared in rmw/init.h
struct rmw_context_impl_s
{
  rmw_dds_common::Context common;
  dds_domainid_t domain_id;
  dds_entity_t ppant;
  rmw_gid_t ppant_gid;

  /* handles for built-in topic readers */
  dds_entity_t rd_participant;
  dds_entity_t rd_subscription;
  dds_entity_t rd_publication;

  /* DDS publisher, subscriber used for ROS 2 publishers and subscriptions */
  dds_entity_t dds_pub;
  dds_entity_t dds_sub;

  /* Participant reference count*/
  size_t node_count{0};
  std::mutex initialization_mutex;

  /* Shutdown flag */
  bool is_shutdown{false};

  /* suffix for GUIDs to construct unique client/service ids
     (protected by initialization_mutex) */
  uint32_t client_service_id;

  rmw_context_impl_s()
  : common(), domain_id(UINT32_MAX), ppant(0), client_service_id(0)
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
  init(rmw_init_options_t * options, size_t domain_id);

  // Destroys the participant, when node_count reaches 0.
  rmw_ret_t
  fini();

  ~rmw_context_impl_s()
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

struct user_callback_data_t
{
  std::mutex mutex;
  rmw_event_callback_t callback {nullptr};
  const void * user_data {nullptr};
  size_t unread_count {0};
  rmw_event_callback_t event_callback[DDS_STATUS_ID_MAX + 1] {nullptr};
  const void * event_data[DDS_STATUS_ID_MAX + 1] {nullptr};
  size_t event_unread_count[DDS_STATUS_ID_MAX + 1] {0};
};

struct CddsPublisher : CddsEntity
{
  dds_instance_handle_t pubiid;
  rmw_gid_t gid;
  struct ddsi_sertype * sertype;
  rosidl_message_type_support_t type_supports;
  dds_data_allocator_t data_allocator;
  uint32_t sample_size;
  bool is_loaning_available;
  user_callback_data_t user_callback_data;
};

struct CddsSubscription : CddsEntity
{
  rmw_gid_t gid;
  dds_entity_t rdcondh;
  rosidl_message_type_support_t type_supports;
  dds_data_allocator_t data_allocator;
  bool is_loaning_available;
  user_callback_data_t user_callback_data;
};

struct client_service_id_t
{
  // strangely, the writer_guid in an rmw_request_id_t is smaller than the identifier in
  // an rmw_gid_t
  uint8_t data[sizeof((reinterpret_cast<rmw_request_id_t *>(0))->writer_guid)]; // NOLINT
};

struct CddsCS
{
  std::unique_ptr<CddsPublisher> pub;
  std::unique_ptr<CddsSubscription> sub;
  client_service_id_t id;
};

struct CddsClient
{
  CddsCS client;

#if REPORT_BLOCKED_REQUESTS
  std::mutex lock;
  dds_time_t lastcheck;
  std::map<int64_t, dds_time_t> reqtime;
#endif
  user_callback_data_t user_callback_data;
};

struct CddsService
{
  CddsCS service;
  user_callback_data_t user_callback_data;
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

static void dds_listener_callback(dds_entity_t entity, void * arg)
{
  // Not currently used
  (void)entity;

  auto data = static_cast<user_callback_data_t *>(arg);

  std::lock_guard<std::mutex> guard(data->mutex);

  if (data->callback) {
    data->callback(data->user_data, 1);
  } else {
    data->unread_count++;
  }
}

#define MAKE_DDS_EVENT_CALLBACK_FN(event_type, EVENT_TYPE) \
  static void on_ ## event_type ## _fn( \
    dds_entity_t entity, \
    const dds_ ## event_type ## _status_t status, \
    void * arg) \
  { \
    (void)status; \
    (void)entity; \
    auto data = static_cast<user_callback_data_t *>(arg); \
    std::lock_guard<std::mutex> guard(data->mutex); \
    auto cb = data->event_callback[DDS_ ## EVENT_TYPE ## _STATUS_ID]; \
    if (cb) { \
      cb(data->event_data[DDS_ ## EVENT_TYPE ## _STATUS_ID], 1); \
    } else { \
      data->event_unread_count[DDS_ ## EVENT_TYPE ## _STATUS_ID]++; \
    } \
  }

// Define event callback functions
MAKE_DDS_EVENT_CALLBACK_FN(requested_deadline_missed, REQUESTED_DEADLINE_MISSED)
MAKE_DDS_EVENT_CALLBACK_FN(liveliness_lost, LIVELINESS_LOST)
MAKE_DDS_EVENT_CALLBACK_FN(offered_deadline_missed, OFFERED_DEADLINE_MISSED)
MAKE_DDS_EVENT_CALLBACK_FN(requested_incompatible_qos, REQUESTED_INCOMPATIBLE_QOS)
MAKE_DDS_EVENT_CALLBACK_FN(sample_lost, SAMPLE_LOST)
MAKE_DDS_EVENT_CALLBACK_FN(offered_incompatible_qos, OFFERED_INCOMPATIBLE_QOS)
MAKE_DDS_EVENT_CALLBACK_FN(liveliness_changed, LIVELINESS_CHANGED)

static void listener_set_event_callbacks(dds_listener_t * l, void * arg)
{
  dds_lset_requested_deadline_missed_arg(l, on_requested_deadline_missed_fn, arg, false);
  dds_lset_requested_incompatible_qos_arg(l, on_requested_incompatible_qos_fn, arg, false);
  dds_lset_sample_lost_arg(l, on_sample_lost_fn, arg, false);
  dds_lset_liveliness_lost_arg(l, on_liveliness_lost_fn, arg, false);
  dds_lset_offered_deadline_missed_arg(l, on_offered_deadline_missed_fn, arg, false);
  dds_lset_offered_incompatible_qos_arg(l, on_offered_incompatible_qos_fn, arg, false);
  dds_lset_liveliness_changed_arg(l, on_liveliness_changed_fn, arg, false);
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

extern "C" rmw_ret_t rmw_subscription_set_on_new_message_callback(
  rmw_subscription_t * rmw_subscription,
  rmw_event_callback_t callback,
  const void * user_data)
{
  auto sub = static_cast<CddsSubscription *>(rmw_subscription->data);

  user_callback_data_t * data = &(sub->user_callback_data);

  std::lock_guard<std::mutex> guard(data->mutex);

  // Set the user callback data
  data->callback = callback;
  data->user_data = user_data;

  if (callback && data->unread_count) {
    // Push events happened before having assigned a callback,
    // limiting them to the QoS depth.
    rmw_qos_profile_t sub_qos;

    if (!get_readwrite_qos(sub->enth, &sub_qos)) {
      return RMW_RET_ERROR;
    }

    size_t events = std::min(data->unread_count, sub_qos.depth);

    callback(user_data, events);
    data->unread_count = 0;
  }

  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_service_set_on_new_request_callback(
  rmw_service_t * rmw_service,
  rmw_event_callback_t callback,
  const void * user_data)
{
  auto srv = static_cast<CddsService *>(rmw_service->data);

  user_callback_data_t * data = &(srv->user_callback_data);

  std::lock_guard<std::mutex> guard(data->mutex);

  // Set the user callback data
  data->callback = callback;
  data->user_data = user_data;

  if (callback && data->unread_count) {
    // Push events happened before having assigned a callback
    callback(user_data, data->unread_count);
    data->unread_count = 0;
  }

  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_client_set_on_new_response_callback(
  rmw_client_t * rmw_client,
  rmw_event_callback_t callback,
  const void * user_data)
{
  auto cli = static_cast<CddsClient *>(rmw_client->data);

  user_callback_data_t * data = &(cli->user_callback_data);

  std::lock_guard<std::mutex> guard(data->mutex);

  // Set the user callback data
  data->callback = callback;
  data->user_data = user_data;

  if (callback && data->unread_count) {
    // Push events happened before having assigned a callback
    callback(user_data, data->unread_count);
    data->unread_count = 0;
  }

  return RMW_RET_OK;
}

template<typename T>
static void event_set_callback(
  T event,
  dds_status_id_t status_id,
  rmw_event_callback_t callback,
  const void * user_data)
{
  user_callback_data_t * data = &(event->user_callback_data);

  std::lock_guard<std::mutex> guard(data->mutex);

  // Set the user callback data
  data->event_callback[status_id] = callback;
  data->event_data[status_id] = user_data;

  if (callback && data->event_unread_count[status_id]) {
    // Push events happened before having assigned a callback
    callback(user_data, data->event_unread_count[status_id]);
    data->event_unread_count[status_id] = 0;
  }
}

extern "C" rmw_ret_t rmw_event_set_callback(
  rmw_event_t * rmw_event,
  rmw_event_callback_t callback,
  const void * user_data)
{
  switch (rmw_event->event_type) {
    case RMW_EVENT_LIVELINESS_CHANGED:
      {
        auto sub_event = static_cast<CddsSubscription *>(rmw_event->data);
        event_set_callback(
          sub_event, DDS_LIVELINESS_CHANGED_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_REQUESTED_DEADLINE_MISSED:
      {
        auto sub_event = static_cast<CddsSubscription *>(rmw_event->data);
        event_set_callback(
          sub_event, DDS_REQUESTED_DEADLINE_MISSED_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE:
      {
        auto sub_event = static_cast<CddsSubscription *>(rmw_event->data);
        event_set_callback(
          sub_event, DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_MESSAGE_LOST:
      {
        auto sub_event = static_cast<CddsSubscription *>(rmw_event->data);
        event_set_callback(
          sub_event, DDS_SAMPLE_LOST_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_LIVELINESS_LOST:
      {
        auto pub_event = static_cast<CddsPublisher *>(rmw_event->data);
        event_set_callback(
          pub_event, DDS_LIVELINESS_LOST_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_OFFERED_DEADLINE_MISSED:
      {
        auto pub_event = static_cast<CddsPublisher *>(rmw_event->data);
        event_set_callback(
          pub_event, DDS_OFFERED_DEADLINE_MISSED_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_OFFERED_QOS_INCOMPATIBLE:
      {
        auto pub_event = static_cast<CddsPublisher *>(rmw_event->data);
        event_set_callback(
          pub_event, DDS_OFFERED_INCOMPATIBLE_QOS_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_INVALID:
      {
        return RMW_RET_INVALID_ARGUMENT;
      }
  }
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
  if (NULL == src->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized src");
    return RMW_RET_INVALID_ARGUMENT;
  }
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

  rmw_init_options_t tmp = *src;
  tmp.enclave = rcutils_strdup(tmp.enclave, *allocator);
  if (NULL != src->enclave && NULL == tmp.enclave) {
    return RMW_RET_BAD_ALLOC;
  }
  tmp.security_options = rmw_get_zero_initialized_security_options();
  rmw_ret_t ret =
    rmw_security_options_copy(&src->security_options, allocator, &tmp.security_options);
  if (RMW_RET_OK != ret) {
    allocator->deallocate(tmp.enclave, allocator->state);
    return ret;
  }
  *dst = tmp;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init_options_fini(rmw_init_options_t * init_options)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
  if (NULL == init_options->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    init_options,
    init_options->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  rcutils_allocator_t * allocator = &init_options->allocator;
  RCUTILS_CHECK_ALLOCATOR(allocator, return RMW_RET_INVALID_ARGUMENT);

  allocator->deallocate(init_options->enclave, allocator->state);
  rmw_ret_t ret = rmw_security_options_fini(&init_options->security_options, allocator);
  *init_options = rmw_get_zero_initialized_init_options();
  return ret;
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

static std::map<std::string, std::vector<uint8_t>> parse_user_data(const dds_qos_t * qos)
{
  std::map<std::string, std::vector<uint8_t>> map;
  void * ud;
  size_t udsz;
  if (dds_qget_userdata(qos, &ud, &udsz)) {
    std::vector<uint8_t> udvec(static_cast<uint8_t *>(ud), static_cast<uint8_t *>(ud) + udsz);
    dds_free(ud);
    map = rmw::impl::cpp::parse_key_value(udvec);
  }
  return map;
}

static bool get_user_data_key(const dds_qos_t * qos, const std::string key, std::string & value)
{
  if (qos != nullptr) {
    auto map = parse_user_data(qos);
    auto name_found = map.find(key);
    if (name_found != map.end()) {
      value = std::string(name_found->second.begin(), name_found->second.end());
      return true;
    }
  }
  return false;
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
      std::string enclave;
      if (get_user_data_key(s->qos, "enclave", enclave)) {
        impl->common.graph_cache.add_participant(gid, enclave);
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
  std::lock_guard<std::mutex> lock(gcdds().domains_lock);
  /* return true: n_nodes incremented, localhost_only set correctly, domain exists
     "      false: n_nodes unchanged, domain left intact if it already existed */
  CddsDomain & dom = gcdds().domains[did];
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
      "<CycloneDDS><Domain><General><Interfaces><NetworkInterface address=\"127.0.0.1\"/>"
      "</Interfaces></General></Domain></CycloneDDS>,"
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
      gcdds().domains.erase(did);
      return false;
    }

    if ((dom.domain_handle = dds_create_domain(did, config.c_str())) < 0) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "rmw_create_node: failed to create domain, error %s", dds_strretcode(dom.domain_handle));
      gcdds().domains.erase(did);
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
    std::lock_guard<std::mutex> lock(gcdds().domains_lock);
    CddsDomain & dom = gcdds().domains[domain_id];
    assert(dom.refcount > 0);
    if (--dom.refcount == 0) {
      static_cast<void>(dds_delete(dom.domain_handle));
      gcdds().domains.erase(domain_id);
    }
  }
}

/* Attempt to set all the qos properties needed to enable DDS security */
static
rmw_ret_t configure_qos_for_security(
  dds_qos_t * qos,
  const rmw_security_options_t * security_options)
{
#if RMW_SUPPORT_SECURITY
  std::unordered_map<std::string, std::string> security_files;
  if (security_options->security_root_path == nullptr) {
    return RMW_RET_UNSUPPORTED;
  }

  if (!rmw_dds_common::get_security_files(
      "file:", security_options->security_root_path, security_files))
  {
    RCUTILS_LOG_INFO_NAMED(
      "rmw_cyclonedds_cpp", "could not find all security files");
    return RMW_RET_UNSUPPORTED;
  }

  dds_qset_prop(qos, "dds.sec.auth.identity_ca", security_files["IDENTITY_CA"].c_str());
  dds_qset_prop(qos, "dds.sec.auth.identity_certificate", security_files["CERTIFICATE"].c_str());
  dds_qset_prop(qos, "dds.sec.auth.private_key", security_files["PRIVATE_KEY"].c_str());
  dds_qset_prop(qos, "dds.sec.access.permissions_ca", security_files["PERMISSIONS_CA"].c_str());
  dds_qset_prop(qos, "dds.sec.access.governance", security_files["GOVERNANCE"].c_str());
  dds_qset_prop(qos, "dds.sec.access.permissions", security_files["PERMISSIONS"].c_str());

  dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
  dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
  dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");

  dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
  dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
  dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");

  dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
  dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
  dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");

  if (security_files.count("CRL") > 0) {
    dds_qset_prop(qos, "org.eclipse.cyclonedds.sec.auth.crl", security_files["CRL"].c_str());
  }

  return RMW_RET_OK;
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
rmw_context_impl_s::init(rmw_init_options_t * options, size_t domain_id)
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
  this->domain_id = static_cast<dds_domainid_t>(domain_id);

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
  get_entity_gid(this->ppant, this->ppant_gid);

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
    common.graph_guard_condition = nullptr;
  }
  if (common.pub) {
    destroy_publisher(common.pub);
    common.pub = nullptr;
  }
  if (common.sub) {
    destroy_subscription(common.sub);
    common.sub = nullptr;
  }
  if (ppant > 0 && dds_delete(ppant) < 0) {
    RCUTILS_SAFE_FWRITE_TO_STDERR(
      "Failed to destroy domain in destructor\n");
  }
  ppant = 0;

  check_destroy_domain(domain_id);
}

rmw_ret_t
rmw_context_impl_s::fini()
{
  std::lock_guard<std::mutex> guard(initialization_mutex);
  if (0u != --this->node_count) {
    // destruction shouldn't happen yet
    return RMW_RET_OK;
  }
  this->clean_up();
  return RMW_RET_OK;
}

template<typename entityT>
static void * init_and_alloc_sample(
  entityT & entity, const uint32_t sample_size, const bool alloc_on_heap = false)
{
  // initialise the data allocator
  if (alloc_on_heap) {
    RET_EXPECTED(
      dds_data_allocator_init_heap(&entity->data_allocator),
      DDS_RETCODE_OK,
      "Reader data allocator initialization failed for heap",
      return nullptr);
  } else {
    RET_EXPECTED(
      dds_data_allocator_init(entity->enth, &entity->data_allocator),
      DDS_RETCODE_OK,
      "Writer allocator initialisation failed",
      return nullptr);
  }
  // allocate memory for message + header
  // the header will be initialized and the chunk pointer will be returned
  auto chunk_ptr = dds_data_allocator_alloc(&entity->data_allocator, sample_size);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    chunk_ptr,
    "Failed to get loan",
    return nullptr);
  // Don't initialize the message memory, as this allocated memory will anyways be filled by the
  // user and initializing the memory here just creates undesired performance hit with the
  // zero-copy path
  return chunk_ptr;
}

template<typename entityT>
static rmw_ret_t fini_and_free_sample(entityT & entity, void * loaned_message)
{
  // fini the message
  rmw_cyclonedds_cpp::fini_message(&entity->type_supports, loaned_message);
  // free the message memory
  RET_EXPECTED(
    dds_data_allocator_free(
      &entity->data_allocator,
      loaned_message),
    DDS_RETCODE_OK,
    "Failed to free the loaned message",
    return RMW_RET_ERROR);
  // fini the allocator
  RET_EXPECTED(
    dds_data_allocator_fini(&entity->data_allocator),
    DDS_RETCODE_OK,
    "Failed to fini data allocator",
    return RMW_RET_ERROR);
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init(const rmw_init_options_t * options, rmw_context_t * context)
{
  rmw_ret_t ret;

  RCUTILS_CHECK_ARGUMENT_FOR_NULL(options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    options->implementation_identifier,
    "expected initialized init options",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    options,
    options->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    options->enclave,
    "expected non-null enclave",
    return RMW_RET_INVALID_ARGUMENT);
  if (NULL != context->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected a zero-initialized context");
    return RMW_RET_INVALID_ARGUMENT;
  }

  if (options->domain_id >= UINT32_MAX && options->domain_id != RMW_DEFAULT_DOMAIN_ID) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: domain id out of range");
    return RMW_RET_INVALID_ARGUMENT;
  }

  auto restore_context = rcpputils::make_scope_exit(
    [context]() {*context = rmw_get_zero_initialized_context();});

  context->instance_id = options->instance_id;
  context->implementation_identifier = eclipse_cyclonedds_identifier;
  // No custom handling of RMW_DEFAULT_DOMAIN_ID. Simply use a reasonable domain id.
  context->actual_domain_id =
    RMW_DEFAULT_DOMAIN_ID != options->domain_id ? options->domain_id : 0u;

  context->impl = new (std::nothrow) rmw_context_impl_t();
  if (nullptr == context->impl) {
    RMW_SET_ERROR_MSG("failed to allocate context impl");
    return RMW_RET_BAD_ALLOC;
  }
  auto cleanup_impl = rcpputils::make_scope_exit(
    [context]() {delete context->impl;});

  if ((ret = rmw_init_options_copy(options, &context->options)) != RMW_RET_OK) {
    return ret;
  }

  cleanup_impl.cancel();
  restore_context.cancel();
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_shutdown(rmw_context_t * context)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    context->impl,
    "expected initialized context",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  context->impl->is_shutdown = true;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_context_fini(rmw_context_t * context)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    context->impl,
    "expected initialized context",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (!context->impl->is_shutdown) {
    RMW_SET_ERROR_MSG("context has not been shutdown");
    return RMW_RET_INVALID_ARGUMENT;
  }
  rmw_ret_t ret = rmw_init_options_fini(&context->options);
  delete context->impl;
  *context = rmw_get_zero_initialized_context();
  return ret;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    NODES                                                          ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_node_t * rmw_create_node(
  rmw_context_t * context, const char * name, const char * namespace_)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(context, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return nullptr);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    context->impl,
    "expected initialized context",
    return nullptr);
  if (context->impl->is_shutdown) {
    RCUTILS_SET_ERROR_MSG("context has been shutdown");
    return nullptr;
  }

  int validation_result = RMW_NODE_NAME_VALID;
  rmw_ret_t ret = rmw_validate_node_name(name, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }
  if (RMW_NODE_NAME_VALID != validation_result) {
    const char * reason = rmw_node_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node name: %s", reason);
    return nullptr;
  }
  validation_result = RMW_NAMESPACE_VALID;
  ret = rmw_validate_namespace(namespace_, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }
  if (RMW_NAMESPACE_VALID != validation_result) {
    const char * reason = rmw_node_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid node namespace: %s", reason);
    return nullptr;
  }

  ret = context->impl->init(&context->options, context->actual_domain_id);
  if (RMW_RET_OK != ret) {
    return nullptr;
  }
  auto finalize_context = rcpputils::make_scope_exit(
    [context]() {context->impl->fini();});

  std::unique_ptr<CddsNode> node_impl(new (std::nothrow) CddsNode());
  RET_ALLOC_X(node_impl, return nullptr);

  rmw_node_t * node = rmw_node_allocate();
  RET_ALLOC_X(node, return nullptr);
  auto cleanup_node = rcpputils::make_scope_exit(
    [node]() {
      rmw_free(const_cast<char *>(node->name));
      rmw_free(const_cast<char *>(node->namespace_));
      rmw_node_free(node);
    });

  node->name = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(name) + 1));
  RET_ALLOC_X(node->name, return nullptr);
  memcpy(const_cast<char *>(node->name), name, strlen(name) + 1);

  node->namespace_ =
    static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(namespace_) + 1));
  RET_ALLOC_X(node->namespace_, return nullptr);
  memcpy(const_cast<char *>(node->namespace_), namespace_, strlen(namespace_) + 1);

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
      return nullptr;
    }
  }

  cleanup_node.cancel();
  node->implementation_identifier = eclipse_cyclonedds_identifier;
  node->data = node_impl.release();
  node->context = context;
  finalize_context.cancel();
  return node;
}

extern "C" rmw_ret_t rmw_destroy_node(rmw_node_t * node)
{
  rmw_ret_t result_ret = RMW_RET_OK;
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto node_impl = static_cast<CddsNode *>(node->data);

  {
    // Though graph_cache methods are thread safe, both cache update and publishing have to also
    // be atomic.
    // If not, the following race condition is possible:
    // node1-update-get-message / node2-update-get-message / node2-publish / node1-publish
    // In that case, the last message published is not accurate.
    auto common = &node->context->impl->common;
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo participant_msg =
      common->graph_cache.remove_node(common->gid, node->name, node->namespace_);
    result_ret = rmw_publish(
      common->pub, static_cast<void *>(&participant_msg), nullptr);
  }

  rmw_context_t * context = node->context;
  rmw_free(const_cast<char *>(node->name));
  rmw_free(const_cast<char *>(node->namespace_));
  rmw_node_free(const_cast<rmw_node_t *>(node));
  delete node_impl;
  context->impl->fini();
  return result_ret;
}

extern "C" const rmw_guard_condition_t * rmw_node_get_graph_guard_condition(const rmw_node_t * node)
{
  RET_NULL_X(node, return nullptr);
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
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_serialize(
  const void * ros_message,
  const rosidl_message_type_support_t * type_support,
  rmw_serialized_message_t * serialized_message)
{
  try {
    auto writer = rmw_cyclonedds_cpp::make_cdr_writer(
      rmw_cyclonedds_cpp::make_message_value_type(type_support));

    auto size = writer->get_serialized_size(ros_message);
    rmw_ret_t ret = rmw_serialized_message_resize(serialized_message, size);
    if (RMW_RET_OK != ret) {
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
  } catch (std::runtime_error & e) {
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

/* Publications need the sertype that DDSI uses for the topic when publishing a
   serialized message.  With the old ("arbitrary") interface of Cyclone, one doesn't know
   the sertype that is actually used because that may be the one that was provided in the
   call to dds_create_topic_arbitrary(), but it may also be one that was introduced by a
   preceding call to create the same topic.

   There is no way of discovering which case it is, and there is no way of getting access
   to the correct sertype.  The best one can do is to keep using one provided when
   creating the topic -- and fortunately using the wrong sertype has surprisingly few
   nasty side-effects, but it still wrong.

   Because the caller retains ownership, so this is easy, but it does require dropping the
   reference when cleaning up.

   The new ("generic") interface instead takes over the ownership of the reference iff it
   succeeds and it returns a non-counted reference to the sertype actually used.  The
   lifetime of the reference is at least as long as the lifetime of the DDS topic exists;
   and the topic's lifetime is at least that of the readers/writers using it.  This
   reference can therefore safely be used. */

static dds_entity_t create_topic(
  dds_entity_t pp, const char * name, struct ddsi_sertype * sertype,
  struct ddsi_sertype ** stact)
{
  dds_entity_t tp;
#if DDS_HAS_DDSI_SERTYPE
  tp = dds_create_topic_sertype(pp, name, &sertype, nullptr, nullptr, nullptr);
#else
  static_cast<void>(name);
  tp = dds_create_topic_generic(pp, &sertype, nullptr, nullptr, nullptr);
#endif
  if (tp < 0) {
    ddsi_sertype_unref(sertype);
  } else {
    if (stact) {
      *stact = sertype;
    }
  }
  return tp;
}

static dds_entity_t create_topic(dds_entity_t pp, const char * name, struct ddsi_sertype * sertype)
{
  dds_entity_t tp = create_topic(pp, name, sertype, nullptr);
  return tp;
}

void set_error_message_from_create_topic(dds_entity_t topic, const std::string & topic_name)
{
  assert(topic < 0);
  if (DDS_RETCODE_BAD_PARAMETER == topic) {
    const std::string error_msg = "failed to create topic [" + topic_name +
      "] because the function was given invalid parameters";
    RMW_SET_ERROR_MSG(error_msg.c_str());
  } else if (DDS_RETCODE_INCONSISTENT_POLICY == topic) {
    const std::string error_msg = "failed to create topic [" + topic_name +
      "] because it's already in use in this context with incompatible QoS settings";
    RMW_SET_ERROR_MSG(error_msg.c_str());
  } else if (DDS_RETCODE_PRECONDITION_NOT_MET == topic) {
    const std::string error_msg = "failed to create topic [" + topic_name +
      "] because it's already in use in this context with a different message type";
    RMW_SET_ERROR_MSG(error_msg.c_str());
  } else {
    const std::string error_msg = "failed to create topic [" + topic_name + "] for unknown reasons";
    RMW_SET_ERROR_MSG(error_msg.c_str());
  }
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
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher, "publisher handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    ros_message, "ros message handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  assert(pub);
  TRACEPOINT(rmw_publish, ros_message);
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
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher, "publisher handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    serialized_message, "serialized message handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  auto pub = static_cast<CddsPublisher *>(publisher->data);

  struct ddsi_serdata * d = serdata_rmw_from_serialized_message(
    pub->sertype, serialized_message->buffer, serialized_message->buffer_length);

#ifdef DDS_HAS_SHM
  // publishing a serialized message when SHM is available
  // (the type need not necessarily be fixed)
  if (dds_is_shared_memory_available(pub->enth)) {
    auto sample_ptr = init_and_alloc_sample(pub, serialized_message->buffer_length);
    RET_NULL_X(sample_ptr, return RMW_RET_ERROR);
    memcpy(sample_ptr, serialized_message->buffer, serialized_message->buffer_length);
    shm_set_data_state(sample_ptr, IOX_CHUNK_CONTAINS_SERIALIZED_DATA);
    d->iox_chunk = sample_ptr;
  }
#endif

  const bool ok = (dds_writecdr(pub->enth, d) >= 0);
  return ok ? RMW_RET_OK : RMW_RET_ERROR;
}

static rmw_ret_t publish_loaned_int(
  const rmw_publisher_t * publisher,
  void * ros_message)
{
#ifdef DDS_HAS_SHM
  RMW_CHECK_FOR_NULL_WITH_MSG(
    publisher, "publisher handle is null",
    return RMW_RET_INVALID_ARGUMENT);
  if (!publisher->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    ros_message, "ROS message handle is null",
    return RMW_RET_INVALID_ARGUMENT);

  auto cdds_publisher = static_cast<CddsPublisher *>(publisher->data);
  if (!cdds_publisher) {
    RMW_SET_ERROR_MSG("publisher data is null");
    return RMW_RET_ERROR;
  }

  // if the publisher allow loaning
  if (cdds_publisher->is_loaning_available) {
    auto d = new serdata_rmw(cdds_publisher->sertype, ddsi_serdata_kind::SDK_DATA);
    d->iox_chunk = ros_message;
    // since we write the loaned chunk here, set the data state to raw
    shm_set_data_state(d->iox_chunk, IOX_CHUNK_CONTAINS_RAW_DATA);
    if (dds_writecdr(cdds_publisher->enth, d) >= 0) {
      return RMW_RET_OK;
    } else {
      RMW_SET_ERROR_MSG("Failed to publish data");
      fini_and_free_sample(cdds_publisher, ros_message);
      ddsi_serdata_unref(d);
      return RMW_RET_ERROR;
    }
  } else {
    RMW_SET_ERROR_MSG("Publishing a loaned message of non fixed type is not allowed");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
#else
  static_cast<void>(publisher);
  static_cast<void>(ros_message);
  RMW_SET_ERROR_MSG("rmw_publish_loaned_message not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
#endif
}

extern "C" rmw_ret_t rmw_publish_loaned_message(
  const rmw_publisher_t * publisher,
  void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);
  return publish_loaned_int(publisher, ros_message);
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
    rcutils_error_string_t prev_error_string = rcutils_get_error_string();
    rcutils_reset_error();
    if ((ts =
      get_message_typesupport_handle(
        type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr)
    {
      return ts;
    } else {
      rcutils_error_string_t error_string = rcutils_get_error_string();
      rcutils_reset_error();
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "Type support not from this implementation. Got:\n"
        "    %s\n"
        "    %s\n"
        "while fetching it",
        prev_error_string.str, error_string.str);
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

static bool is_rmw_duration_unspecified(rmw_time_t duration)
{
  return rmw_time_equal(duration, RMW_DURATION_UNSPECIFIED);
}

static dds_duration_t rmw_duration_to_dds(rmw_time_t duration)
{
  if (rmw_time_equal(duration, RMW_DURATION_INFINITE)) {
    return DDS_INFINITY;
  } else {
    return rmw_time_total_nsec(duration);
  }
}

static rmw_time_t dds_duration_to_rmw(dds_duration_t duration)
{
  if (duration == DDS_INFINITY) {
    return RMW_DURATION_INFINITE;
  } else {
    return rmw_time_from_nsec(duration);
  }
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
    case RMW_QOS_POLICY_HISTORY_UNKNOWN:
      return nullptr;
    default:
      rmw_cyclonedds_cpp::unreachable();
  }
  switch (qos_policies->reliability) {
    case RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
      dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_INFINITY);
      break;
    case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
      dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
      break;
    case RMW_QOS_POLICY_RELIABILITY_UNKNOWN:
      return nullptr;
    default:
      rmw_cyclonedds_cpp::unreachable();
  }
  switch (qos_policies->durability) {
    case RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT:
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
    case RMW_QOS_POLICY_DURABILITY_UNKNOWN:
      return nullptr;
    default:
      rmw_cyclonedds_cpp::unreachable();
  }

  if (!is_rmw_duration_unspecified(qos_policies->lifespan)) {
    dds_qset_lifespan(qos, rmw_duration_to_dds(qos_policies->lifespan));
  }
  if (!is_rmw_duration_unspecified(qos_policies->deadline)) {
    dds_qset_deadline(qos, rmw_duration_to_dds(qos_policies->deadline));
  }

  if (is_rmw_duration_unspecified(qos_policies->liveliness_lease_duration)) {
    ldur = DDS_INFINITY;
  } else {
    ldur = rmw_duration_to_dds(qos_policies->liveliness_lease_duration);
  }
  switch (qos_policies->liveliness) {
    case RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_LIVELINESS_AUTOMATIC:
      dds_qset_liveliness(qos, DDS_LIVELINESS_AUTOMATIC, ldur);
      break;
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC:
      dds_qset_liveliness(qos, DDS_LIVELINESS_MANUAL_BY_TOPIC, ldur);
      break;
    case RMW_QOS_POLICY_LIVELINESS_UNKNOWN:
      return nullptr;
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
        // When using a policy of KEEP_ALL, the depth is meaningless.
        // CycloneDDS reports this as -1, but the rmw_qos_profile_t structure
        // expects an unsigned number.  Casting -1 to unsigned would yield
        // a value of 2^32 - 1, but unfortunately our XML-RPC connection
        // (used for the command-line tools) doesn't understand anything
        // larger than 2^31 - 1.  Just set the depth to 0 here instead.
        qos_policies->depth = 0;
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
    qos_policies->deadline = dds_duration_to_rmw(deadline);
  }

  {
    dds_duration_t lifespan;
    if (!dds_qget_lifespan(dds_qos, &lifespan)) {
      lifespan = DDS_INFINITY;
    }
    qos_policies->lifespan = dds_duration_to_rmw(lifespan);
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
    qos_policies->liveliness_lease_duration = dds_duration_to_rmw(lease_duration);
  }

  return true;
}

static bool is_type_self_contained(const rosidl_message_type_support_t * type_supports)
{
  auto ts = get_message_typesupport_handle(
    type_supports,
    rosidl_typesupport_introspection_cpp::typesupport_identifier);
  if (ts != nullptr) {   // CPP typesupport
    auto members = static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(
      ts->data);
    MessageTypeSupport_cpp mts(members);
    return mts.is_type_self_contained();
  } else {
    ts = get_message_typesupport_handle(
      type_supports,
      rosidl_typesupport_introspection_c__identifier);
    if (ts != nullptr) {  // C typesupport
      auto members = static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(
        ts->data);
      MessageTypeSupport_c mts(members);
      return mts.is_type_self_contained();
    } else {
      RMW_SET_ERROR_MSG("Non supported type-supported");
      return false;
    }
  }
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
  bool is_fixed_type = is_type_self_contained(type_support);
  uint32_t sample_size = static_cast<uint32_t>(rmw_cyclonedds_cpp::get_message_size(type_support));
  auto sertype = create_sertype(
    type_support->typesupport_identifier,
    create_message_type_support(type_support->data, type_support->typesupport_identifier), false,
    rmw_cyclonedds_cpp::make_message_value_type(type_supports), sample_size, is_fixed_type);
  struct ddsi_sertype * stact = nullptr;
  topic = create_topic(dds_ppant, fqtopic_name.c_str(), sertype, &stact);

  dds_listener_t * listener = dds_create_listener(&pub->user_callback_data);
  // Set the corresponding callbacks to listen for events
  listener_set_event_callbacks(listener, &pub->user_callback_data);

  if (topic < 0) {
    set_error_message_from_create_topic(topic, fqtopic_name);
    goto fail_topic;
  }
  if ((qos = create_readwrite_qos(qos_policies, false)) == nullptr) {
    goto fail_qos;
  }
  if ((pub->enth = dds_create_writer(dds_pub, topic, qos, listener)) < 0) {
    RMW_SET_ERROR_MSG("failed to create writer");
    goto fail_writer;
  }
  if (dds_get_instance_handle(pub->enth, &pub->pubiid) < 0) {
    RMW_SET_ERROR_MSG("failed to get instance handle for writer");
    goto fail_instance_handle;
  }
  get_entity_gid(pub->enth, pub->gid);
  pub->sertype = stact;
  dds_delete_listener(listener);
  pub->type_supports = *type_supports;
  pub->is_loaning_available = is_fixed_type && dds_is_loan_available(pub->enth);
  pub->sample_size = sample_size;
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
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_fini_publisher_allocation(rmw_publisher_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_fini_publisher_allocation: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

static rmw_publisher_t * create_publisher(
  dds_entity_t dds_ppant, dds_entity_t dds_pub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options
)
{
  CddsPublisher * pub;
  if ((pub =
    create_cdds_publisher(
      dds_ppant, dds_pub, type_supports, topic_name, qos_policies)) == nullptr)
  {
    return nullptr;
  }
  auto cleanup_cdds_publisher = rcpputils::make_scope_exit(
    [pub]() {
      if (dds_delete(pub->enth) < 0) {
        RCUTILS_LOG_ERROR_NAMED(
          "rmw_cyclonedds_cpp", "failed to delete writer during error handling");
      }
      delete pub;
    });

  rmw_publisher_t * rmw_publisher = rmw_publisher_allocate();
  RET_ALLOC_X(rmw_publisher, return nullptr);
  auto cleanup_rmw_publisher = rcpputils::make_scope_exit(
    [rmw_publisher]() {
      rmw_free(const_cast<char *>(rmw_publisher->topic_name));
      rmw_publisher_free(rmw_publisher);
    });
  rmw_publisher->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_publisher->data = pub;
  rmw_publisher->topic_name = reinterpret_cast<char *>(rmw_allocate(strlen(topic_name) + 1));
  RET_ALLOC_X(rmw_publisher->topic_name, return nullptr);
  memcpy(const_cast<char *>(rmw_publisher->topic_name), topic_name, strlen(topic_name) + 1);
  rmw_publisher->options = *publisher_options;
  rmw_publisher->can_loan_messages = pub->is_loaning_available;

  cleanup_rmw_publisher.cancel();
  cleanup_cdds_publisher.cancel();
  return rmw_publisher;
}

extern "C" rmw_publisher_t * rmw_create_publisher(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options
)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(type_supports, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, nullptr);
  if (0 == strlen(topic_name)) {
    RMW_SET_ERROR_MSG("topic_name argument is an empty string");
    return nullptr;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_policies, nullptr);
  if (!qos_policies->avoid_ros_namespace_conventions) {
    int validation_result = RMW_TOPIC_VALID;
    rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
    if (RMW_RET_OK != ret) {
      return nullptr;
    }
    if (RMW_TOPIC_VALID != validation_result) {
      const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid topic name: %s", reason);
      return nullptr;
    }
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher_options, nullptr);
  if (publisher_options->require_unique_network_flow_endpoints ==
    RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_STRICTLY_REQUIRED)
  {
    RMW_SET_ERROR_MSG(
      "Strict requirement on unique network flow endpoints for publishers not supported");
    return nullptr;
  }

  rmw_publisher_t * pub = create_publisher(
    node->context->impl->ppant, node->context->impl->dds_pub,
    type_supports, topic_name, qos_policies,
    publisher_options);
  if (pub == nullptr) {
    return nullptr;
  }
  auto cleanup_publisher = rcpputils::make_scope_exit(
    [pub]() {
      rmw_error_state_t error_state = *rmw_get_error_state();
      rmw_reset_error();
      if (RMW_RET_OK != destroy_publisher(pub)) {
        RMW_SAFE_FWRITE_TO_STDERR(rmw_get_error_string().str);
        RMW_SAFE_FWRITE_TO_STDERR(" during '" RCUTILS_STRINGIFY(__function__) "' cleanup\n");
        rmw_reset_error();
      }
      rmw_set_error_state(error_state.message, error_state.file, error_state.line_number);
    });

  // Update graph
  auto common = &node->context->impl->common;
  const auto cddspub = static_cast<const CddsPublisher *>(pub->data);
  {
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.associate_writer(cddspub->gid, common->gid, node->name, node->namespace_);
    if (RMW_RET_OK != rmw_publish(common->pub, static_cast<void *>(&msg), nullptr)) {
      static_cast<void>(common->graph_cache.dissociate_writer(
        cddspub->gid, common->gid, node->name, node->namespace_));
      return nullptr;
    }
  }

  cleanup_publisher.cancel();
  TRACEPOINT(rmw_publisher_init, static_cast<const void *>(pub), cddspub->gid.data);
  return pub;
}

extern "C" rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t * publisher, rmw_gid_t * gid)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);
  auto pub = static_cast<const CddsPublisher *>(publisher->data);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(gid1, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    gid1,
    gid1->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid2, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    gid2,
    gid2->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(result, RMW_RET_INVALID_ARGUMENT);
  /* alignment is potentially lost because of the translation to an array of bytes, so use
     memcmp instead of a simple integer comparison */
  *result = memcmp(gid1->data, gid2->data, sizeof(gid1->data)) == 0;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_publisher_count_matched_subscriptions(
  const rmw_publisher_t * publisher,
  size_t * subscription_count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription_count, RMW_RET_INVALID_ARGUMENT);

  auto pub = static_cast<CddsPublisher *>(publisher->data);
  dds_publication_matched_status_t status;
  if (dds_get_publication_matched_status(pub->enth, &status) < 0) {
    return RMW_RET_ERROR;
  }

  *subscription_count = status.current_count;
  return RMW_RET_OK;
}

rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t * publisher)
{
  RET_NULL(publisher);
  RET_WRONG_IMPLID(publisher);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (dds_assert_liveliness(pub->enth) < 0) {
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

rmw_ret_t rmw_publisher_wait_for_all_acked(
  const rmw_publisher_t * publisher,
  rmw_time_t wait_timeout)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (pub == nullptr) {
    RMW_SET_ERROR_MSG("The publisher is not a valid publisher.");
    return RMW_RET_INVALID_ARGUMENT;
  }

  dds_duration_t timeout = rmw_duration_to_dds(wait_timeout);
  switch (dds_wait_for_acks(pub->enth, timeout)) {
    case DDS_RETCODE_OK:
      return RMW_RET_OK;
    case DDS_RETCODE_BAD_PARAMETER:
      RMW_SET_ERROR_MSG("The publisher is not a valid publisher.");
      return RMW_RET_INVALID_ARGUMENT;
    case DDS_RETCODE_TIMEOUT:
      return RMW_RET_TIMEOUT;
    case DDS_RETCODE_UNSUPPORTED:
      return RMW_RET_UNSUPPORTED;
    default:
      return RMW_RET_ERROR;
  }
}

rmw_ret_t rmw_publisher_get_actual_qos(const rmw_publisher_t * publisher, rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (get_readwrite_qos(pub->enth, qos)) {
    return RMW_RET_OK;
  }
  return RMW_RET_ERROR;
}

static rmw_ret_t borrow_loaned_message_int(
  const rmw_publisher_t * publisher,
  const rosidl_message_type_support_t * type_support,
  void ** ros_message)
{
#ifdef DDS_HAS_SHM
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  if (!publisher->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(type_support, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(ros_message, RMW_RET_INVALID_ARGUMENT);
  if (*ros_message) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto cdds_publisher = static_cast<CddsPublisher *>(publisher->data);
  if (!cdds_publisher) {
    RMW_SET_ERROR_MSG("publisher data is null");
    return RMW_RET_ERROR;
  }

  // if the publisher can loan
  if (cdds_publisher->is_loaning_available) {
    auto sample_ptr = init_and_alloc_sample(cdds_publisher, cdds_publisher->sample_size);
    RET_NULL_X(sample_ptr, return RMW_RET_ERROR);
    *ros_message = sample_ptr;
    return RMW_RET_OK;
  } else {
    RMW_SET_ERROR_MSG("Borrowing loan for a non fixed type is not allowed");
    return RMW_RET_ERROR;
  }
#else
  (void) publisher;
  (void) type_support;
  (void) ros_message;
  RMW_SET_ERROR_MSG("rmw_borrow_loaned_message not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
#endif
}

extern "C" rmw_ret_t rmw_borrow_loaned_message(
  const rmw_publisher_t * publisher,
  const rosidl_message_type_support_t * type_support,
  void ** ros_message)
{
  return borrow_loaned_message_int(publisher, type_support, ros_message);
}

static rmw_ret_t return_loaned_message_from_publisher_int(
  const rmw_publisher_t * publisher,
  void * loaned_message)
{
#ifdef DDS_HAS_SHM
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  if (!publisher->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(loaned_message, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  auto cdds_publisher = static_cast<CddsPublisher *>(publisher->data);
  if (!cdds_publisher) {
    RMW_SET_ERROR_MSG("publisher data is null");
    return RMW_RET_ERROR;
  }

  // if the publisher can loan
  if (cdds_publisher->is_loaning_available) {
    return fini_and_free_sample(cdds_publisher, loaned_message);
  } else {
    RMW_SET_ERROR_MSG("returning loan for a non fixed type is not allowed");
    return RMW_RET_ERROR;
  }
#else
  (void) publisher;
  (void) loaned_message;
  RMW_SET_ERROR_MSG(
    "rmw_return_loaned_message_from_publisher not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
#endif
}

extern "C" rmw_ret_t rmw_return_loaned_message_from_publisher(
  const rmw_publisher_t * publisher,
  void * loaned_message)
{
  return return_loaned_message_from_publisher_int(publisher, loaned_message);
}

static rmw_ret_t destroy_publisher(rmw_publisher_t * publisher)
{
  rmw_ret_t ret = RMW_RET_OK;
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (pub != nullptr) {
    if (dds_delete(pub->enth) < 0) {
      RMW_SET_ERROR_MSG("failed to delete writer");
      ret = RMW_RET_ERROR;
    }
    delete pub;
  }
  rmw_free(const_cast<char *>(publisher->topic_name));
  rmw_publisher_free(publisher);
  return ret;
}

extern "C" rmw_ret_t rmw_destroy_publisher(rmw_node_t * node, rmw_publisher_t * publisher)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher,
    publisher->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  rmw_ret_t ret = RMW_RET_OK;
  rmw_error_state_t error_state;
  {
    auto common = &node->context->impl->common;
    const auto cddspub = static_cast<const CddsPublisher *>(publisher->data);
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.dissociate_writer(
      cddspub->gid, common->gid, node->name,
      node->namespace_);
    rmw_ret_t publish_ret =
      rmw_publish(common->pub, static_cast<void *>(&msg), nullptr);
    if (RMW_RET_OK != publish_ret) {
      error_state = *rmw_get_error_state();
      ret = publish_ret;
      rmw_reset_error();
    }
  }

  rmw_ret_t inner_ret = destroy_publisher(publisher);
  if (RMW_RET_OK != inner_ret) {
    if (RMW_RET_OK != ret) {
      RMW_SAFE_FWRITE_TO_STDERR(rmw_get_error_string().str);
      RMW_SAFE_FWRITE_TO_STDERR(" during '" RCUTILS_STRINGIFY(__function__) "'\n");
    } else {
      error_state = *rmw_get_error_state();
      ret = inner_ret;
    }
    rmw_reset_error();
  }

  if (RMW_RET_OK != ret) {
    rmw_set_error_state(error_state.message, error_state.file, error_state.line_number);
  }

  return ret;
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
  bool is_fixed_type = is_type_self_contained(type_support);
  uint32_t sample_size = static_cast<uint32_t>(rmw_cyclonedds_cpp::get_message_size(type_support));
  auto sertype = create_sertype(
    type_support->typesupport_identifier,
    create_message_type_support(type_support->data, type_support->typesupport_identifier), false,
    rmw_cyclonedds_cpp::make_message_value_type(type_supports), sample_size, is_fixed_type);
  topic = create_topic(dds_ppant, fqtopic_name.c_str(), sertype);

  dds_listener_t * listener = dds_create_listener(&sub->user_callback_data);
  // Set the callback to listen for new messages
  dds_lset_data_available_arg(listener, dds_listener_callback, &sub->user_callback_data, false);
  // Set the corresponding callbacks to listen for events
  listener_set_event_callbacks(listener, &sub->user_callback_data);

  if (topic < 0) {
    set_error_message_from_create_topic(topic, fqtopic_name);
    goto fail_topic;
  }
  if ((qos = create_readwrite_qos(qos_policies, ignore_local_publications)) == nullptr) {
    goto fail_qos;
  }
  if ((sub->enth = dds_create_reader(dds_sub, topic, qos, listener)) < 0) {
    RMW_SET_ERROR_MSG("failed to create reader");
    goto fail_reader;
  }
  get_entity_gid(sub->enth, sub->gid);
  if ((sub->rdcondh = dds_create_readcondition(sub->enth, DDS_ANY_STATE)) < 0) {
    RMW_SET_ERROR_MSG("failed to create readcondition");
    goto fail_readcond;
  }
  dds_delete_listener(listener);
  sub->type_supports = *type_support;
  sub->is_loaning_available = is_fixed_type && dds_is_loan_available(sub->enth);
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
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_fini_subscription_allocation(rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_fini_subscription_allocation: unimplemented");
  return RMW_RET_UNSUPPORTED;
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
    return nullptr;
  }
  auto cleanup_subscription = rcpputils::make_scope_exit(
    [sub]() {
      if (dds_delete(sub->rdcondh) < 0) {
        RMW_SAFE_FWRITE_TO_STDERR(
          "failed to delete readcondition during '"
          RCUTILS_STRINGIFY(__function__) "' cleanup\n");
      }
      if (dds_delete(sub->enth) < 0) {
        RMW_SAFE_FWRITE_TO_STDERR(
          "failed to delete reader during '"
          RCUTILS_STRINGIFY(__function__) "' cleanup\n");
      }
      delete sub;
    });
  rmw_subscription = rmw_subscription_allocate();
  RET_ALLOC_X(rmw_subscription, return nullptr);
  auto cleanup_rmw_subscription = rcpputils::make_scope_exit(
    [rmw_subscription]() {
      rmw_free(const_cast<char *>(rmw_subscription->topic_name));
      rmw_subscription_free(rmw_subscription);
    });
  rmw_subscription->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_subscription->data = sub;
  rmw_subscription->topic_name =
    static_cast<const char *>(rmw_allocate(strlen(topic_name) + 1));
  RET_ALLOC_X(rmw_subscription->topic_name, return nullptr);
  memcpy(const_cast<char *>(rmw_subscription->topic_name), topic_name, strlen(topic_name) + 1);
  rmw_subscription->options = *subscription_options;
  rmw_subscription->can_loan_messages = sub->is_loaning_available;
  rmw_subscription->is_cft_enabled = false;

  cleanup_subscription.cancel();
  cleanup_rmw_subscription.cancel();
  return rmw_subscription;
}

extern "C" rmw_subscription_t * rmw_create_subscription(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_subscription_options_t * subscription_options)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, nullptr);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(type_supports, nullptr);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, nullptr);
  if (0 == strlen(topic_name)) {
    RMW_SET_ERROR_MSG("topic_name argument is an empty string");
    return nullptr;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_policies, nullptr);
  if (!qos_policies->avoid_ros_namespace_conventions) {
    int validation_result = RMW_TOPIC_VALID;
    rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
    if (RMW_RET_OK != ret) {
      return nullptr;
    }
    if (RMW_TOPIC_VALID != validation_result) {
      const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("invalid topic_name argument: %s", reason);
      return nullptr;
    }
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription_options, nullptr);
  if (subscription_options->require_unique_network_flow_endpoints ==
    RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_STRICTLY_REQUIRED)
  {
    RMW_SET_ERROR_MSG(
      "Strict requirement on unique network flow endpoints for subscriptions not supported");
    return nullptr;
  }

  rmw_subscription_t * sub = create_subscription(
    node->context->impl->ppant, node->context->impl->dds_sub,
    type_supports, topic_name, qos_policies,
    subscription_options);
  if (sub == nullptr) {
    return nullptr;
  }
  auto cleanup_subscription = rcpputils::make_scope_exit(
    [sub]() {
      rmw_error_state_t error_state = *rmw_get_error_state();
      rmw_reset_error();
      if (RMW_RET_OK != destroy_subscription(sub)) {
        RMW_SAFE_FWRITE_TO_STDERR(rmw_get_error_string().str);
        RMW_SAFE_FWRITE_TO_STDERR(" during '" RCUTILS_STRINGIFY(__function__) "' cleanup\n");
        rmw_reset_error();
      }
      rmw_set_error_state(error_state.message, error_state.file, error_state.line_number);
    });

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
    return nullptr;
  }

  cleanup_subscription.cancel();
  TRACEPOINT(rmw_subscription_init, static_cast<const void *>(sub), cddssub->gid.data);
  return sub;
}

extern "C" rmw_ret_t rmw_subscription_count_matched_publishers(
  const rmw_subscription_t * subscription, size_t * publisher_count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription,
    subscription->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher_count, RMW_RET_INVALID_ARGUMENT);

  auto sub = static_cast<CddsSubscription *>(subscription->data);
  dds_subscription_matched_status_t status;
  if (dds_get_subscription_matched_status(sub->enth, &status) < 0) {
    return RMW_RET_ERROR;
  }

  *publisher_count = status.current_count;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_subscription_get_actual_qos(
  const rmw_subscription_t * subscription,
  rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription,
    subscription->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  auto sub = static_cast<CddsSubscription *>(subscription->data);
  if (get_readwrite_qos(sub->enth, qos)) {
    return RMW_RET_OK;
  }
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_subscription_set_content_filter(
  rmw_subscription_t * subscription,
  const rmw_subscription_content_filter_options_t * options)
{
  static_cast<void>(subscription);
  static_cast<void>(options);

  RMW_SET_ERROR_MSG("rmw_subscription_set_content_filter: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_subscription_get_content_filter(
  const rmw_subscription_t * subscription,
  rcutils_allocator_t * allocator,
  rmw_subscription_content_filter_options_t * options)
{
  static_cast<void>(subscription);
  static_cast<void>(allocator);
  static_cast<void>(options);

  RMW_SET_ERROR_MSG("rmw_subscription_get_content_filter: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

static rmw_ret_t destroy_subscription(rmw_subscription_t * subscription)
{
  rmw_ret_t ret = RMW_RET_OK;
  auto sub = static_cast<CddsSubscription *>(subscription->data);
  clean_waitset_caches();
  if (dds_delete(sub->rdcondh) < 0) {
    RMW_SET_ERROR_MSG("failed to delete readcondition");
    ret = RMW_RET_ERROR;
  }
  if (dds_delete(sub->enth) < 0) {
    if (RMW_RET_OK == ret) {
      RMW_SET_ERROR_MSG("failed to delete reader");
      ret = RMW_RET_ERROR;
    } else {
      RMW_SAFE_FWRITE_TO_STDERR("failed to delete reader\n");
    }
  }
  delete sub;
  rmw_free(const_cast<char *>(subscription->topic_name));
  rmw_subscription_free(subscription);
  return ret;
}

extern "C" rmw_ret_t rmw_destroy_subscription(rmw_node_t * node, rmw_subscription_t * subscription)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription,
    subscription->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  rmw_ret_t ret = RMW_RET_OK;
  rmw_error_state_t error_state;
  rmw_error_string_t error_string;
  {
    auto common = &node->context->impl->common;
    const auto cddssub = static_cast<const CddsSubscription *>(subscription->data);
    std::lock_guard<std::mutex> guard(common->node_update_mutex);
    rmw_dds_common::msg::ParticipantEntitiesInfo msg =
      common->graph_cache.dissociate_reader(
      cddssub->gid, common->gid, node->name,
      node->namespace_);
    ret = rmw_publish(common->pub, static_cast<void *>(&msg), nullptr);
    if (RMW_RET_OK != ret) {
      error_state = *rmw_get_error_state();
      error_string = rmw_get_error_string();
      rmw_reset_error();
    }
  }

  rmw_ret_t local_ret = destroy_subscription(subscription);
  if (RMW_RET_OK != local_ret) {
    if (RMW_RET_OK != ret) {
      RMW_SAFE_FWRITE_TO_STDERR(error_string.str);
      RMW_SAFE_FWRITE_TO_STDERR(" during '" RCUTILS_STRINGIFY(__function__) "'\n");
    }
    ret = local_ret;
  } else if (RMW_RET_OK != ret) {
    rmw_set_error_state(error_state.message, error_state.file, error_state.line_number);
  }

  return ret;
}

static void message_info_from_sample_info(
  const dds_sample_info_t & info, rmw_message_info_t * message_info)
{
  message_info->publisher_gid.implementation_identifier = eclipse_cyclonedds_identifier;
  memset(message_info->publisher_gid.data, 0, sizeof(message_info->publisher_gid.data));
  assert(sizeof(info.publication_handle) <= sizeof(message_info->publisher_gid.data));
  memcpy(
    message_info->publisher_gid.data, &info.publication_handle,
    sizeof(info.publication_handle));
  message_info->source_timestamp = info.source_timestamp;
  // TODO(iluetkeb) add received timestamp, when implemented by Cyclone
  message_info->received_timestamp = 0;
  message_info->publication_sequence_number = RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED;
  message_info->reception_sequence_number = RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED;
}

static rmw_ret_t rmw_take_int(
  const rmw_subscription_t * subscription, void * ros_message,
  bool * taken, rmw_message_info_t * message_info)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(
    taken, RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_ARGUMENT_FOR_NULL(
    ros_message, RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_ARGUMENT_FOR_NULL(
    subscription, RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle,
    subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  CddsSubscription * sub = static_cast<CddsSubscription *>(subscription->data);
  RET_NULL(sub);
  dds_sample_info_t info;
  while (dds_take(sub->enth, &ros_message, &info, 1, 1) == 1) {
    if (info.valid_data) {
      *taken = true;
      if (message_info) {
        message_info_from_sample_info(info, message_info);
      }
#if REPORT_LATE_MESSAGES > 0
      dds_time_t tnow = dds_time();
      dds_time_t dt = tnow - info.source_timestamp;
      if (dt >= DDS_MSECS(REPORT_LATE_MESSAGES)) {
        fprintf(stderr, "** sample in history for %.fms\n", static_cast<double>(dt) / 1e6);
      }
#endif
      goto take_done;
    }
  }
  *taken = false;
take_done:
  TRACEPOINT(
    rmw_take,
    static_cast<const void *>(subscription),
    static_cast<const void *>(ros_message),
    (message_info ? message_info->source_timestamp : 0LL),
    *taken);
  return RMW_RET_OK;
}

static rmw_ret_t rmw_take_seq(
  const rmw_subscription_t * subscription,
  size_t count,
  rmw_message_sequence_t * message_sequence,
  rmw_message_info_sequence_t * message_info_sequence,
  size_t * taken)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(
    taken, RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_ARGUMENT_FOR_NULL(
    message_sequence, RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_ARGUMENT_FOR_NULL(
    message_info_sequence, RMW_RET_INVALID_ARGUMENT);

  RMW_CHECK_ARGUMENT_FOR_NULL(
    subscription, RMW_RET_INVALID_ARGUMENT);
  RET_WRONG_IMPLID(subscription);

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle,
    subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  if (0u == count) {
    RMW_SET_ERROR_MSG("count cannot be 0");
    return RMW_RET_INVALID_ARGUMENT;
  }

  if (count > message_sequence->capacity) {
    RMW_SET_ERROR_MSG("Insuffient capacity in message_sequence");
    return RMW_RET_INVALID_ARGUMENT;
  }

  if (count > message_info_sequence->capacity) {
    RMW_SET_ERROR_MSG("Insuffient capacity in message_info_sequence");
    return RMW_RET_INVALID_ARGUMENT;
  }

  if (count > (std::numeric_limits<uint32_t>::max)()) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
      "Cannot take %zu samples at once, limit is %d",
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
        message_info_from_sample_info(info, message_info);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(
    subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(
    serialized_message, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(
    taken, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle,
    subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION)
  CddsSubscription * sub = static_cast<CddsSubscription *>(subscription->data);
  RET_NULL(sub);
  dds_sample_info_t info;
  struct ddsi_serdata * d;
  while (dds_takecdr(sub->enth, &d, 1, &info, DDS_ANY_STATE) == 1) {
    if (info.valid_data) {
      if (message_info) {
        message_info_from_sample_info(info, message_info);
      }

      // taking a serialized msg from shared memory
#ifdef DDS_HAS_SHM
      if (d->iox_chunk != nullptr) {
        auto iox_header = iceoryx_header_from_chunk(d->iox_chunk);
        if (iox_header->shm_data_state == IOX_CHUNK_CONTAINS_SERIALIZED_DATA) {
          const size_t size = iox_header->data_size;
          if (rmw_serialized_message_resize(serialized_message, size) != RMW_RET_OK) {
            ddsi_serdata_unref(d);
            *taken = false;
            return RMW_RET_ERROR;
          }
          ddsi_serdata_to_ser(d, 0, size, serialized_message->buffer);
          serialized_message->buffer_length = size;
          ddsi_serdata_unref(d);
          *taken = true;
          return RMW_RET_OK;
        } else if (iox_header->shm_data_state == IOX_CHUNK_CONTAINS_RAW_DATA) {
          if (rmw_serialize(d->iox_chunk, &sub->type_supports, serialized_message) != RMW_RET_OK) {
            RMW_SET_ERROR_MSG("Failed to serialize sample from loaned memory");
            ddsi_serdata_unref(d);
            *taken = false;
            return RMW_RET_ERROR;
          }
          ddsi_serdata_unref(d);
          *taken = true;
          return RMW_RET_OK;
        } else {
          RMW_SET_ERROR_MSG("The recieved sample over SHM is not initialized");
          ddsi_serdata_unref(d);
          return RMW_RET_ERROR;
        }
        // release the chunk
        free_iox_chunk(static_cast<iox_sub_t *>(d->iox_subscriber), &d->iox_chunk);
      } else  // NOLINT
#endif
      {
        size_t size = ddsi_serdata_size(d);
        if (rmw_serialized_message_resize(serialized_message, size) != RMW_RET_OK) {
          ddsi_serdata_unref(d);
          *taken = false;
          return RMW_RET_ERROR;
        }
        ddsi_serdata_to_ser(d, 0, size, serialized_message->buffer);
        serialized_message->buffer_length = size;
        ddsi_serdata_unref(d);
        *taken = true;
        return RMW_RET_OK;
      }
    }
    ddsi_serdata_unref(d);
  }
  *taken = false;
  return RMW_RET_OK;
}

static rmw_ret_t rmw_take_loan_int(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_message_info_t * message_info)
{
#ifdef DDS_HAS_SHM
  RMW_CHECK_ARGUMENT_FOR_NULL(
    subscription, RMW_RET_INVALID_ARGUMENT);
  if (!subscription->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(
    loaned_message, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(
    taken, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle,
    subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto cdds_subscription = static_cast<CddsSubscription *>(subscription->data);
  if (!cdds_subscription) {
    RMW_SET_ERROR_MSG("Subscription data is null");
    return RMW_RET_ERROR;
  }

  dds_sample_info_t info;
  struct ddsi_serdata * d;
  while (dds_takecdr(cdds_subscription->enth, &d, 1, &info, DDS_ANY_STATE) == 1) {
    if (info.valid_data) {
      if (message_info) {
        message_info_from_sample_info(info, message_info);
      }
      if (d->iox_chunk != nullptr) {
        // the iox chunk has data, based on the kind of the data return the data accordingly to
        // the user
        auto iox_header = iceoryx_header_from_chunk(d->iox_chunk);
        // if the iox chunk has the data in serialized form
        if (iox_header->shm_data_state == IOX_CHUNK_CONTAINS_SERIALIZED_DATA) {
          rmw_serialized_message_t ser_msg;
          ser_msg.buffer_length = iox_header->data_size;
          ser_msg.buffer = static_cast<uint8_t *>(d->iox_chunk);
          if (rmw_deserialize(&ser_msg, &cdds_subscription->type_supports, *loaned_message) !=
            RMW_RET_OK)
          {
            RMW_SET_ERROR_MSG("Failed to deserialize sample from shared memory buffer");
            ddsi_serdata_unref(d);
            *taken = false;
            return RMW_RET_ERROR;
          }
        } else if (iox_header->shm_data_state == IOX_CHUNK_CONTAINS_RAW_DATA) {
          *loaned_message = d->iox_chunk;
        } else {
          RMW_SET_ERROR_MSG("Received iox chunk is uninitialized");
          ddsi_serdata_unref(d);
          *taken = false;
          return RMW_RET_ERROR;
        }
        *taken = true;
        // doesn't allocate, but initialise the allocator to free the chunk later when the loan
        // is returned
        dds_data_allocator_init(
          cdds_subscription->enth, &cdds_subscription->data_allocator);
        // set the loaned chunk to null, so that the  loaned chunk is not release in
        // rmw_serdata_free(), but will be released when
        // `rmw_return_loaned_message_from_subscription()` is called
        d->iox_chunk = nullptr;
        ddsi_serdata_unref(d);
        return RMW_RET_OK;
      } else if (d->type->iox_size > 0U) {
        auto sample_ptr = init_and_alloc_sample(cdds_subscription, d->type->iox_size, true);
        RET_NULL_X(sample_ptr, return RMW_RET_ERROR);
        ddsi_serdata_to_sample(d, sample_ptr, nullptr, nullptr);
        *loaned_message = sample_ptr;
        ddsi_serdata_unref(d);
        *taken = true;
        return RMW_RET_OK;
      } else {
        RMW_SET_ERROR_MSG("Data nor loan is available to take");
        ddsi_serdata_unref(d);
        *taken = false;
        return RMW_RET_ERROR;
      }
    }
    ddsi_serdata_unref(d);
  }
  *taken = false;
  return RMW_RET_OK;
#else
  static_cast<void>(subscription);
  static_cast<void>(loaned_message);
  static_cast<void>(taken);
  static_cast<void>(message_info);
  RMW_SET_ERROR_MSG("rmw_take_loaned_message not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
#endif
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
  RMW_CHECK_ARGUMENT_FOR_NULL(message_info, RMW_RET_INVALID_ARGUMENT);
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

  RMW_CHECK_ARGUMENT_FOR_NULL(
    message_info, RMW_RET_INVALID_ARGUMENT);

  return rmw_take_ser_int(subscription, serialized_message, taken, message_info);
}

extern "C" rmw_ret_t rmw_take_loaned_message(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  return rmw_take_loan_int(subscription, loaned_message, taken, nullptr);
}

extern "C" rmw_ret_t rmw_take_loaned_message_with_info(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(allocation);
  RMW_CHECK_ARGUMENT_FOR_NULL(
    message_info, RMW_RET_INVALID_ARGUMENT);
  return rmw_take_loan_int(subscription, loaned_message, taken, message_info);
}

static rmw_ret_t return_loaned_message_from_subscription_int(
  const rmw_subscription_t * subscription,
  void * loaned_message)
{
#ifdef DDS_HAS_SHM
  RMW_CHECK_ARGUMENT_FOR_NULL(
    subscription, RMW_RET_INVALID_ARGUMENT);
  if (!subscription->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(
    loaned_message, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle,
    subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto cdds_subscription = static_cast<CddsSubscription *>(subscription->data);
  if (!cdds_subscription) {
    RMW_SET_ERROR_MSG("Subscription data is null");
    return RMW_RET_ERROR;
  }

  // if the subscription allow loaning
  if (cdds_subscription->is_loaning_available) {
    return fini_and_free_sample(cdds_subscription, loaned_message);
  } else {
    RMW_SET_ERROR_MSG("returning loan for a non fixed type is not allowed");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
#else
  (void) subscription;
  (void) loaned_message;
  RMW_SET_ERROR_MSG(
    "rmw_return_loaned_message_from_subscription not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
#endif
}

extern "C" rmw_ret_t rmw_return_loaned_message_from_subscription(
  const rmw_subscription_t * subscription,
  void * loaned_message)
{
  return return_loaned_message_from_subscription_int(subscription, loaned_message);
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
  {RMW_EVENT_MESSAGE_LOST, DDS_SAMPLE_LOST_STATUS},
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
  RET_NULL(publisher);
  RET_WRONG_IMPLID(publisher);
  return init_rmw_event(
    rmw_event,
    publisher->implementation_identifier,
    publisher->data,
    event_type);
}

extern "C" rmw_ret_t rmw_subscription_event_init(
  rmw_event_t * rmw_event, const rmw_subscription_t * subscription, rmw_event_type_t event_type)
{
  RET_NULL(subscription);
  RET_WRONG_IMPLID(subscription);
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
  RET_NULL(event_handle);
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

    case RMW_EVENT_MESSAGE_LOST: {
        auto ei = static_cast<rmw_message_lost_status_t *>(event_info);
        auto sub = static_cast<CddsSubscription *>(event_handle->data);
        dds_sample_lost_status_t st;
        if (dds_get_sample_lost_status(sub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        }
        ei->total_count = static_cast<size_t>(st.total_count);
        ei->total_count_change = static_cast<size_t>(st.total_count_change);
        *taken = true;
        return RMW_RET_OK;
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
  RET_NULL(guard_condition_handle);
  RET_WRONG_IMPLID(guard_condition_handle);
  auto * gcond_impl = static_cast<CddsGuardCondition *>(guard_condition_handle->data);
  dds_set_guardcondition(gcond_impl->gcondh, true);
  return RMW_RET_OK;
}

extern "C" rmw_wait_set_t * rmw_create_wait_set(rmw_context_t * context, size_t max_conditions)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(context, nullptr);
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
    std::lock_guard<std::mutex> lock(gcdds().lock);
    // Lazily create dummy guard condition
    if (gcdds().waitsets.size() == 0) {
      if ((gcdds().gc_for_empty_waitset = dds_create_guardcondition(DDS_CYCLONEDDS_HANDLE)) < 0) {
        RMW_SET_ERROR_MSG("failed to create guardcondition for handling empty waitsets");
        goto fail_create_dummy;
      }
    }
    // Attach never-triggered guard condition.  As it will never be triggered, it will never be
    // included in the result of dds_waitset_wait
    if (dds_waitset_attach(ws->waitseth, gcdds().gc_for_empty_waitset, INTPTR_MAX) < 0) {
      RMW_SET_ERROR_MSG("failed to attach dummy guard condition for blocking on empty waitset");
      goto fail_attach_dummy;
    }
    gcdds().waitsets.insert(ws);
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
  RET_NULL(wait_set);
  RET_WRONG_IMPLID(wait_set);
  auto result = RMW_RET_OK;
  auto ws = static_cast<CddsWaitset *>(wait_set->data);
  RET_NULL(ws);
  dds_delete(ws->waitseth);
  {
    std::lock_guard<std::mutex> lock(gcdds().lock);
    gcdds().waitsets.erase(ws);
    if (gcdds().waitsets.size() == 0) {
      dds_delete(gcdds().gc_for_empty_waitset);
      gcdds().gc_for_empty_waitset = 0;
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
  std::lock_guard<std::mutex> lock(gcdds().lock);
  for (auto && ws : gcdds().waitsets) {
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
  RET_NULL_X(wait_set, return RMW_RET_INVALID_ARGUMENT);
  RET_WRONG_IMPLID(wait_set);
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
    (dds_time_t) rmw_time_total_nsec(*wait_timeout);
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

using get_matched_endpoints_fn_t = dds_return_t (*)(
  dds_entity_t h,
  dds_instance_handle_t * xs, size_t nxs);
using BuiltinTopicEndpoint = std::unique_ptr<dds_builtintopic_endpoint_t,
    std::function<void (dds_builtintopic_endpoint_t *)>>;

static rmw_ret_t get_matched_endpoints(
  dds_entity_t h, get_matched_endpoints_fn_t fn, std::vector<dds_instance_handle_t> & res)
{
  dds_return_t ret;
  if ((ret = fn(h, res.data(), res.size())) < 0) {
    return RMW_RET_ERROR;
  }
  while (static_cast<size_t>(ret) >= res.size()) {
    // 128 is a completely arbitrary margin to reduce the risk of having to retry
    // when matches are create/deleted in parallel
    res.resize(static_cast<size_t>(ret) + 128);
    if ((ret = fn(h, res.data(), res.size())) < 0) {
      return RMW_RET_ERROR;
    }
  }
  res.resize(static_cast<size_t>(ret));
  return RMW_RET_OK;
}

static void free_builtintopic_endpoint(dds_builtintopic_endpoint_t * e)
{
  dds_delete_qos(e->qos);
  dds_free(e->topic_name);
  dds_free(e->type_name);
  dds_free(e);
}

static BuiltinTopicEndpoint get_matched_subscription_data(
  dds_entity_t writer, dds_instance_handle_t readerih)
{
  BuiltinTopicEndpoint ep(dds_get_matched_subscription_data(writer, readerih),
    free_builtintopic_endpoint);
  return ep;
}

static BuiltinTopicEndpoint get_matched_publication_data(
  dds_entity_t reader, dds_instance_handle_t writerih)
{
  BuiltinTopicEndpoint ep(dds_get_matched_publication_data(reader, writerih),
    free_builtintopic_endpoint);
  return ep;
}

static const std::string csid_to_string(const client_service_id_t & id)
{
  std::ostringstream os;
  os << std::hex;
  os << std::setw(2) << static_cast<int>(id.data[0]);
  for (size_t i = 1; i < sizeof(id.data); i++) {
    os << "." << static_cast<int>(id.data[i]);
  }
  return os.str();
}

static rmw_ret_t rmw_take_response_request(
  CddsCS * cs, rmw_service_info_t * request_header,
  void * ros_data, bool * taken, dds_time_t * source_timestamp,
  dds_instance_handle_t srcfilter)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(taken, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_data, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(request_header, RMW_RET_INVALID_ARGUMENT);
  cdds_request_wrapper_t wrap;
  dds_sample_info_t info;
  wrap.data = ros_data;
  void * wrap_ptr = static_cast<void *>(&wrap);
  while (dds_take(cs->sub->enth, &wrap_ptr, &info, 1, 1) == 1) {
    if (info.valid_data) {
      static_assert(
        sizeof(request_header->request_id.writer_guid) ==
        sizeof(wrap.header.guid) + sizeof(info.publication_handle),
        "request header size assumptions not met");
      memcpy(
        static_cast<void *>(request_header->request_id.writer_guid),
        static_cast<const void *>(&wrap.header.guid), sizeof(wrap.header.guid));
      memcpy(
        static_cast<void *>(request_header->request_id.writer_guid + sizeof(wrap.header.guid)),
        static_cast<const void *>(&info.publication_handle), sizeof(info.publication_handle));
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
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client,
    client->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    service,
    service->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto info = static_cast<CddsService *>(service->data);
  return rmw_take_response_request(
    &info->service, request_header, ros_request, taken, nullptr,
    false);
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

enum class client_present_t
{
  FAILURE,  // an error occurred when checking
  MAYBE,    // reader not matched, writer still present
  YES,      // reader matched
  GONE      // neither reader nor writer
};

static bool check_client_service_endpoint(
  const dds_builtintopic_endpoint_t * ep,
  const std::string key, const std::string needle)
{
  if (ep != nullptr) {
    std::string clientid;
    get_user_data_key(ep->qos, key, clientid);
    return clientid == needle;
  }
  return false;
}

static client_present_t check_for_response_reader(
  const CddsCS & service,
  const dds_instance_handle_t reqwrih)
{
  auto reqwr = get_matched_publication_data(service.sub->enth, reqwrih);
  std::string clientid;
  if (reqwr == nullptr) {
    return client_present_t::GONE;
  } else if (!get_user_data_key(reqwr->qos, "clientid", clientid)) {
    // backwards-compatibility: a client without a client id, assume all is well
    return client_present_t::YES;
  } else {
    // look for this client's reader: if we have matched it, all is well;
    // if not, continue waiting
    std::vector<dds_instance_handle_t> rds;
    if (get_matched_endpoints(service.pub->enth, dds_get_matched_subscriptions, rds) < 0) {
      RMW_SET_ERROR_MSG("rmw_send_response: failed to get reader/writer matches");
      return client_present_t::FAILURE;
    }
    // if we have matched this client's reader, all is well
    for (const auto & rdih : rds) {
      auto rd = get_matched_subscription_data(service.pub->enth, rdih);
      if (check_client_service_endpoint(rd.get(), "clientid", clientid)) {
        return client_present_t::YES;
      }
    }
    return client_present_t::MAYBE;
  }
}

extern "C" rmw_ret_t rmw_send_response(
  const rmw_service_t * service,
  rmw_request_id_t * request_header, void * ros_response)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    service,
    service->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(request_header, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_response, RMW_RET_INVALID_ARGUMENT);
  CddsService * info = static_cast<CddsService *>(service->data);
  cdds_request_header_t header;
  dds_instance_handle_t reqwrih;
  static_assert(
    sizeof(request_header->writer_guid) == sizeof(header.guid) + sizeof(reqwrih),
    "request header size assumptions not met");
  memcpy(
    static_cast<void *>(&header.guid), static_cast<const void *>(request_header->writer_guid),
    sizeof(header.guid));
  memcpy(
    static_cast<void *>(&reqwrih),
    static_cast<const void *>(request_header->writer_guid + sizeof(header.guid)), sizeof(reqwrih));
  header.seq = request_header->sequence_number;
  // Block until the response reader has been matched by the response writer (this is a
  // workaround: rmw_service_server_is_available should keep returning false until this
  // is a given).
  // TODO(eboasson): rmw_service_server_is_available should block the request instead (#191)
  client_present_t st;
  std::chrono::system_clock::time_point tnow = std::chrono::system_clock::now();
  std::chrono::system_clock::time_point tend = tnow + 100ms;
  while ((st =
    check_for_response_reader(
      info->service,
      reqwrih)) == client_present_t::MAYBE && tnow < tend)
  {
    dds_sleepfor(DDS_MSECS(10));
    tnow = std::chrono::system_clock::now();
  }
  switch (st) {
    case client_present_t::FAILURE:
      break;
    case client_present_t::MAYBE:
      return RMW_RET_TIMEOUT;
    case client_present_t::YES:
      return rmw_send_response_request(&info->service, header, ros_response);
    case client_present_t::GONE:
      return RMW_RET_OK;
  }
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_send_request(
  const rmw_client_t * client, const void * ros_request,
  int64_t * sequence_id)
{
  static std::atomic_uint next_request_id;
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client,
    client->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(sequence_id, RMW_RET_INVALID_ARGUMENT);

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
    rcutils_error_string_t prev_error_string = rcutils_get_error_string();
    rcutils_reset_error();
    if ((ts =
      get_service_typesupport_handle(
        type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier)) != nullptr)
    {
      return ts;
    } else {
      rcutils_error_string_t error_string = rcutils_get_error_string();
      rcutils_reset_error();
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
        "Service type support not from this implementation. Got:\n"
        "    %s\n"
        "    %s\n"
        "while fetching it",
        prev_error_string.str, error_string.str);
      return nullptr;
    }
  }
}

static void get_unique_csid(const rmw_node_t * node, client_service_id_t & id)
{
  auto impl = node->context->impl;
  static_assert(
    sizeof(dds_guid_t) <= sizeof(id.data),
    "client/service id assumed it can hold a DDSI GUID");
  static_assert(
    sizeof(dds_guid_t) <= sizeof((reinterpret_cast<rmw_gid_t *>(0))->data),
    "client/service id assumes rmw_gid_t can hold a DDSI GUID");
  uint32_t x;

  {
    std::lock_guard<std::mutex> guard(impl->initialization_mutex);
    x = ++impl->client_service_id;
  }

  // construct id by taking the entity prefix (which is just the first 12
  // bytes of the GID, which itself is just the GUID padded with 0's; then
  // overwriting the entity id with the big-endian counter value
  memcpy(id.data, impl->ppant_gid.data, 12);
  for (size_t i = 0, s = 24; i < 4; i++, s -= 8) {
    id.data[12 + i] = static_cast<uint8_t>(x >> s);
  }
}

static rmw_ret_t rmw_init_cs(
  CddsCS * cs, user_callback_data_t * cb_data,
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name, const rmw_qos_profile_t * qos_policies,
  bool is_service)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(type_supports, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(service_name, RMW_RET_INVALID_ARGUMENT);
  if (0 == strlen(service_name)) {
    RMW_SET_ERROR_MSG("service_name argument is an empty string");
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_policies, RMW_RET_INVALID_ARGUMENT);
  if (!qos_policies->avoid_ros_namespace_conventions) {
    int validation_result = RMW_TOPIC_VALID;
    rmw_ret_t ret = rmw_validate_full_topic_name(service_name, &validation_result, nullptr);
    if (RMW_RET_OK != ret) {
      return ret;
    }
    if (RMW_TOPIC_VALID != validation_result) {
      const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
      RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("service_name argument is invalid: %s", reason);
      return RMW_RET_INVALID_ARGUMENT;
    }
  }

  const rosidl_service_type_support_t * type_support = get_service_typesupport(type_supports);
  RET_NULL(type_support);

  auto pub = std::make_unique<CddsPublisher>();
  auto sub = std::make_unique<CddsSubscription>();
  std::string subtopic_name, pubtopic_name;
  void * pub_type_support, * sub_type_support;

  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> pub_msg_ts, sub_msg_ts;

  dds_listener_t * listener = dds_create_listener(cb_data);
  dds_lset_data_available_arg(listener, dds_listener_callback, cb_data, false);

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
  struct sertype_rmw * pub_st, * sub_st;

  pub_st = create_sertype(
    type_support->typesupport_identifier, pub_type_support, true,
    std::move(pub_msg_ts));
  struct ddsi_sertype * pub_stact;
  pubtopic = create_topic(node->context->impl->ppant, pubtopic_name.c_str(), pub_st, &pub_stact);
  if (pubtopic < 0) {
    set_error_message_from_create_topic(pubtopic, pubtopic_name);
    goto fail_pubtopic;
  }

  sub_st = create_sertype(
    type_support->typesupport_identifier, sub_type_support, true,
    std::move(sub_msg_ts));
  subtopic = create_topic(node->context->impl->ppant, subtopic_name.c_str(), sub_st);
  if (subtopic < 0) {
    set_error_message_from_create_topic(subtopic, subtopic_name);
    goto fail_subtopic;
  }
  // before proceeding to outright ignore given QoS policies, sanity check them
  dds_qos_t * qos;
  if ((qos = create_readwrite_qos(qos_policies, false)) == nullptr) {
    goto fail_qos;
  }

  // store a unique identifier for this client/service in the user
  // data of the reader and writer so that we can always determine
  // which pairs belong together
  get_unique_csid(node, cs->id);
  {
    std::string user_data = std::string(is_service ? "serviceid=" : "clientid=") + csid_to_string(
      cs->id) + std::string(";");
    dds_qset_userdata(qos, user_data.c_str(), user_data.size());
  }
  if ((pub->enth = dds_create_writer(node->context->impl->dds_pub, pubtopic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create writer");
    goto fail_writer;
  }
  get_entity_gid(pub->enth, pub->gid);
  pub->sertype = pub_stact;
  if ((sub->enth = dds_create_reader(node->context->impl->dds_sub, subtopic, qos, listener)) < 0) {
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
  dds_delete_listener(listener);
  dds_delete_qos(qos);
  dds_delete(subtopic);
  dds_delete(pubtopic);

  cs->pub = std::move(pub);
  cs->sub = std::move(sub);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client,
    client->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
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
  delete info;
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
      &info->client, &info->user_callback_data,
      node, type_supports, service_name, qos_policies, false) != RMW_RET_OK)
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
  delete info;
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_client(rmw_node_t * node, rmw_client_t * client)
{
  return destroy_client(node, client);
}

static rmw_ret_t destroy_service(const rmw_node_t * node, rmw_service_t * service)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    service,
    service->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
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
  delete info;
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
      &info->service, &info->user_callback_data,
      node, type_supports, service_name, qos_policies, true) != RMW_RET_OK)
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
  delete info;
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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_names)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_namespaces)) {
    return RMW_RET_INVALID_ARGUMENT;
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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_names)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_namespaces)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(enclaves)) {
    return RMW_RET_INVALID_ARGUMENT;
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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_names_and_types_check_zero(tptyp)) {
    return RMW_RET_INVALID_ARGUMENT;
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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_names_and_types_check_zero(sntyp)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

  auto common_context = &node->context->impl->common;
  return common_context->graph_cache.get_names_and_types(
    _demangle_service_from_topic,
    _demangle_service_type_only,
    allocator,
    sntyp);
}

static rmw_ret_t get_topic_name(dds_entity_t endpoint_handle, std::string & name)
{
  std::vector<char> tmp(128);
  dds_return_t rc = dds_get_name(dds_get_topic(endpoint_handle), tmp.data(), tmp.size());
  if (rc > 0 && static_cast<size_t>(rc) >= tmp.size()) {
    // topic name is too long for the buffer, but now we know how long it is
    tmp.resize(static_cast<size_t>(rc) + 1);
    rc = dds_get_name(dds_get_topic(endpoint_handle), tmp.data(), tmp.size());
  }
  if (rc < 0) {
    return RMW_RET_ERROR;
  } else if (static_cast<size_t>(rc) >= tmp.size()) {
    // topic names can't change, so the topic must have been deleted and the
    // handle reused for something with a longer name (which is exceedingly
    // unlikely), and so it really is an error
    return RMW_RET_ERROR;
  }

  name = std::string(tmp.begin(), tmp.begin() + rc);
  return RMW_RET_OK;
}

static rmw_ret_t check_for_service_reader_writer(const CddsCS & client, bool * is_available)
{
  std::vector<dds_instance_handle_t> rds, wrs;
  assert(is_available != nullptr && !*is_available);
  if (get_matched_endpoints(client.pub->enth, dds_get_matched_subscriptions, rds) < 0 ||
    get_matched_endpoints(client.sub->enth, dds_get_matched_publications, wrs) < 0)
  {
    RMW_SET_ERROR_MSG("rmw_service_server_is_available: failed to get reader/writer matches");
    return RMW_RET_ERROR;
  }
  // first extract all service ids from matched readers
  std::set<std::string> needles;
  for (const auto & rdih : rds) {
    auto rd = get_matched_subscription_data(client.pub->enth, rdih);
    std::string serviceid;
    if (rd && get_user_data_key(rd->qos, "serviceid", serviceid)) {
      needles.insert(serviceid);
    }
  }
  if (needles.empty()) {
    // if no services advertising a serviceid have been matched, but there
    // are matched request readers and response writers, then we fall back
    // to the old method of simply requiring the existence of matches.
    *is_available = !rds.empty() && !wrs.empty();
  } else {
    // scan the writers to see if there is at least one response writer
    // matching a discovered request reader
    for (const auto & wrih : wrs) {
      auto wr = get_matched_publication_data(client.sub->enth, wrih);
      std::string serviceid;
      if (wr &&
        get_user_data_key(
          wr->qos, "serviceid",
          serviceid) && needles.find(serviceid) != needles.end())
      {
        *is_available = true;
        break;
      }
    }
  }
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_service_server_is_available(
  const rmw_node_t * node,
  const rmw_client_t * client,
  bool * is_available)
{
  RET_NULL(node);
  RET_WRONG_IMPLID(node);
  RET_NULL(client);
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
    return ret;
  }
  return check_for_service_reader_writer(info->client, is_available);
}

extern "C" rmw_ret_t rmw_count_publishers(
  const rmw_node_t * node, const char * topic_name,
  size_t * count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
  int validation_result = RMW_TOPIC_VALID;
  rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return ret;
  }
  if (RMW_TOPIC_VALID != validation_result) {
    const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("topic_name argument is invalid: %s", reason);
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(count, RMW_RET_INVALID_ARGUMENT);

  auto common_context = &node->context->impl->common;
  const std::string mangled_topic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
  return common_context->graph_cache.get_writer_count(mangled_topic_name, count);
}

extern "C" rmw_ret_t rmw_count_subscribers(
  const rmw_node_t * node, const char * topic_name,
  size_t * count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
  int validation_result = RMW_TOPIC_VALID;
  rmw_ret_t ret = rmw_validate_full_topic_name(topic_name, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return ret;
  }
  if (RMW_TOPIC_VALID != validation_result) {
    const char * reason = rmw_full_topic_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("topic_name argument is invalid: %s", reason);
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(count, RMW_RET_INVALID_ARGUMENT);

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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  int validation_result = RMW_NODE_NAME_VALID;
  rmw_ret_t ret = rmw_validate_node_name(node_name, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return ret;
  }
  if (RMW_NODE_NAME_VALID != validation_result) {
    const char * reason = rmw_node_name_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("node_name argument is invalid: %s", reason);
    return RMW_RET_INVALID_ARGUMENT;
  }
  validation_result = RMW_NAMESPACE_VALID;
  ret = rmw_validate_namespace(node_namespace, &validation_result, nullptr);
  if (RMW_RET_OK != ret) {
    return ret;
  }
  if (RMW_NAMESPACE_VALID != validation_result) {
    const char * reason = rmw_namespace_validation_result_string(validation_result);
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("node_namespace argument is invalid: %s", reason);
    return RMW_RET_INVALID_ARGUMENT;
  }
  ret = rmw_names_and_types_check_zero(topic_names_and_types);
  if (RMW_RET_OK != ret) {
    return ret;
  }

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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_topic_endpoint_info_array_check_zero(publishers_info)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node,
    node->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(topic_name, RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_topic_endpoint_info_array_check_zero(subscriptions_info)) {
    return RMW_RET_INVALID_ARGUMENT;
  }

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

extern "C" rmw_ret_t rmw_qos_profile_check_compatible(
  const rmw_qos_profile_t publisher_profile,
  const rmw_qos_profile_t subscription_profile,
  rmw_qos_compatibility_type_t * compatibility,
  char * reason,
  size_t reason_size)
{
  return rmw_dds_common::qos_profile_check_compatible(
    publisher_profile, subscription_profile, compatibility, reason, reason_size);
}

extern "C" rmw_ret_t rmw_client_request_publisher_get_actual_qos(
  const rmw_client_t * client,
  rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  auto cli = static_cast<CddsClient *>(client->data);

  if (get_readwrite_qos(cli->client.pub->enth, qos)) {
    return RMW_RET_OK;
  }

  RMW_SET_ERROR_MSG("failed to get client's request publisher QoS");
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_client_response_subscription_get_actual_qos(
  const rmw_client_t * client,
  rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  auto cli = static_cast<CddsClient *>(client->data);

  if (get_readwrite_qos(cli->client.sub->enth, qos)) {
    return RMW_RET_OK;
  }

  RMW_SET_ERROR_MSG("failed to get client's response subscription QoS");
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_service_response_publisher_get_actual_qos(
  const rmw_service_t * service,
  rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  auto srv = static_cast<CddsService *>(service->data);

  if (get_readwrite_qos(srv->service.pub->enth, qos)) {
    return RMW_RET_OK;
  }

  RMW_SET_ERROR_MSG("failed to get service's response publisher QoS");
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_service_request_subscription_get_actual_qos(
  const rmw_service_t * service,
  rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);

  auto srv = static_cast<CddsService *>(service->data);

  if (get_readwrite_qos(srv->service.sub->enth, qos)) {
    return RMW_RET_OK;
  }

  RMW_SET_ERROR_MSG("failed to get service's request subscription QoS");
  return RMW_RET_ERROR;
}

extern "C" bool rmw_feature_supported(rmw_feature_t feature)
{
  (void)feature;
  return false;
}
