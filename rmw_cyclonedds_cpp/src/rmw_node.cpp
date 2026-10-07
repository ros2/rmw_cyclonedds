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
#include <cstdlib>
#include <cstring>
#ifdef __linux__
#include <fstream>
#endif
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <thread>
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
#include <system_error>
#include <exception>

#include "rcutils/allocator.h"
#include "rcutils/env.h"
#include "rcutils/filesystem.h"
#include "rcutils/format_string.h"
#include "rcutils/logging_macros.h"
#include "rcutils/process.h"
#include "rcutils/strdup.h"

#include "rmw/allocators.h"
#include "rmw/convert_rcutils_ret_to_rmw_ret.h"
#include "rmw/discovery_options.h"
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
#include "BufferEndpointDiscovery.hpp"

#include "rmw_version_test.hpp"

#include "rmw/get_topic_endpoint_info.h"
#include "rmw/get_service_endpoint_info.h"
#include "rmw/incompatible_qos_events_statuses.h"
#include "rmw/topic_endpoint_info_array.h"
#include "rmw/service_endpoint_info_array.h"

#include "rmw_dds_common/context.hpp"
#include "rmw_dds_common/graph_cache.hpp"
#include "rmw_dds_common/msg/participant_entities_info.hpp"
#include "rmw_dds_common/qos.hpp"

#include "rmw_security_common/security.hpp"

#include "rosidl_buffer/buffer_impl_base.hpp"
#include "rosidl_runtime_c/type_hash.h"

#include "rosidl_typesupport_cpp/message_type_support.hpp"

#include "tracetools/tracetools.h"

#include "namespace_prefix.hpp"

#include "dds/dds.h"
#include "cdds_version.hpp"
#if CDDS_VERSION > CDDS_VERSION_0_10
#include "dds/ddsc/dds_psmx.h"
#else
#include "dds/ddsc/dds_data_allocator.h"
#include "dds/ddsc/dds_loan_api.h"
#endif
#include "serdata.hpp"
#include "demangle.hpp"

#include "dyntype.hpp"
#include "type_name.hpp"

using namespace std::literals::chrono_literals;

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

#define RET_ERR(msg) RET_ERR_X(msg, return RMW_RET_ERROR)
#define RET_NULL(var) RET_NULL_X(var, return RMW_RET_ERROR)
#define RET_ALLOC(var) RET_ALLOC_X(var, return RMW_RET_ERROR)

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
  const rmw_node_t * node, dds_entity_t dds_ppant, dds_entity_t dds_pub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options
);
static rmw_ret_t destroy_publisher(rmw_publisher_t * publisher);

static rmw_subscription_t * create_subscription(
  const rmw_node_t * node, dds_entity_t dds_ppant, dds_entity_t dds_pub,
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

  rmw_discovery_options_t discovery_options;
  uint32_t refcount;

  /* handle of the domain entity */
  dds_entity_t domain_handle;

  /* Default constructor so operator[] can be safely be used to look one up */
  CddsDomain()
  : refcount(0), domain_handle(0)
  {
    discovery_options = rmw_get_zero_initialized_discovery_options();
  }

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
#if CDDS_VERSION == CDDS_VERSION_0_10
  dds_data_allocator_t data_allocator;
#endif
  uint32_t sample_size;
  bool is_loaning_available;
  user_callback_data_t user_callback_data;
  // Base (fully-qualified) topic name, kept around so a
  // publication_matched callback -- fired long after
  // create_cdds_publisher() returns -- can derive the private per-peer
  // topic name the matching subscription side computes independently.
  std::string base_topic_name;
  rmw_cyclonedds_cpp::BufferEndpointDiscovery buffer_discovery;
  // Set once at creation, so rmw_publish() -- the universal hot path,
  // called for every publisher whether buffer-backed or not -- can skip
  // buffer_discovery.mutex entirely for the common case instead of taking
  // it just to find private_writers_or_readers empty. A publisher with this
  // false never has the buffer-discovery on_publication_matched (below,
  // create_cdds_publisher's `if (has_buffer_fields) dds_lset_publication_
  // matched_arg(...)`) registered as a listener at all, so its map is
  // provably always empty; this flag lets rmw_publish() know that without
  // reading the map.
  //
  // on_publication_matched (buffer-discovery, gated on has_buffer_fields)
  // and on_publication_matched_fn (generic, registered unconditionally by
  // listener_set_event_callbacks()) are different functions, but that does
  // NOT mean they cannot conflict: dds_lset_publication_matched_arg()
  // replaces the listener's registration BY EVENT TYPE, not by function
  // identity, so create_cdds_publisher()'s later, has_buffer_fields-gated
  // call to it silently overwrites listener_set_event_callbacks()'s
  // earlier one on the same listener, disabling the standard
  // RMW_EVENT_PUBLICATION_MATCHED callback and unread-count bookkeeping
  // for every buffer-backed publisher. Fixed at on_publication_matched()'s
  // own definition: it calls on_publication_matched_fn() explicitly, so
  // both behaviors run off the one slot instead of one silently losing
  // the other.
  bool has_buffer_fields {false};
  // on_publication_matched() needs a rmw_node_t* to call
  // build_endpoint_info_from_match(), which reaches the node's own
  // rmw_dds_common::GraphCache -- nothing else on CddsPublisher carries
  // that back-reference. Set once, in create_cdds_publisher(), before
  // dds_create_writer() -- same ordering rule as base_topic_name above,
  // since the matched callback can fire the instant the writer exists.
  const rmw_node_t * node {nullptr};
  // Built once here, at creation time, rather than rebuilt via
  // make_message_value_type() on EVERY publish call for a buffer-backed
  // publisher with an active peer -- a recursive introspection type-tree
  // walk on the hot path has_buffer_fields exists specifically to keep
  // cheap.
  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> buffer_message_value_type;
  // consume_buffer_descriptor() correlates a shared-topic message with its
  // private-topic descriptor by exact dds_time_t equality. dds_time()'s
  // finite resolution means two rapid rmw_publish() calls on the SAME
  // publisher can legitimately return the identical value -- a collision
  // lets a later message's descriptor be installed into an earlier
  // message's buffer field. See rmw_publish()'s own comment on how this
  // enforces strict per-publisher monotonicity to close it.
  std::atomic<dds_time_t> last_buffer_tstamp {0};
};

struct CddsSubscription : CddsEntity
{
  rmw_gid_t gid;
  dds_entity_t rdcondh;
  rosidl_message_type_support_t type_supports;
  // The reader's own sertype, ref-counted by on_subscription_matched() to
  // create a second topic of the same message type for a private, per-peer
  // reader.
  struct ddsi_sertype * sertype;
#if CDDS_VERSION == CDDS_VERSION_0_10
  dds_data_allocator_t data_allocator;
#endif
  bool is_loaning_available;
  user_callback_data_t user_callback_data;
  // Mirrors CddsPublisher's own fields above.
  std::string base_topic_name;
  rmw_cyclonedds_cpp::BufferEndpointDiscovery buffer_discovery;
  // Mirrors CddsPublisher::has_buffer_fields -- rmw_take()'s consumer of
  // the private readers is the hot path here, called for every
  // subscription whether buffer-backed or not. A subscription with this
  // false never has the buffer-discovery on_subscription_matched
  // registered as a listener at all (see create_cdds_subscription), so
  // its map is provably always empty; this flag lets rmw_take() know that
  // without touching buffer_discovery at all. See
  // CddsPublisher::has_buffer_fields's own comment for the real conflict
  // with listener_set_event_callbacks()'s registration and the fix at
  // on_subscription_matched()'s own definition.
  bool has_buffer_fields {false};
  // Mirrors CddsPublisher::node -- on_subscription_matched() needs it
  // for build_endpoint_info_from_match(), same reason as the publisher side.
  const rmw_node_t * node {nullptr};
  // Mirrors CddsPublisher::buffer_message_value_type -- built once
  // here, at creation time, so rmw_take()'s install_first_buffer_impl()
  // walk on every received sample never rebuilds the introspection type
  // tree via make_message_value_type() on the hot path.
  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> buffer_message_value_type;
};

struct client_service_id_t
{
  uint8_t data[RMW_GID_STORAGE_SIZE];
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
MAKE_DDS_EVENT_CALLBACK_FN(inconsistent_topic, INCONSISTENT_TOPIC)
MAKE_DDS_EVENT_CALLBACK_FN(subscription_matched, SUBSCRIPTION_MATCHED)
MAKE_DDS_EVENT_CALLBACK_FN(publication_matched, PUBLICATION_MATCHED)

static void listener_set_event_callbacks(dds_listener_t * l, void * arg)
{
  dds_lset_requested_deadline_missed_arg(l, on_requested_deadline_missed_fn, arg, false);
  dds_lset_requested_incompatible_qos_arg(l, on_requested_incompatible_qos_fn, arg, false);
  dds_lset_sample_lost_arg(l, on_sample_lost_fn, arg, false);
  dds_lset_liveliness_lost_arg(l, on_liveliness_lost_fn, arg, false);
  dds_lset_offered_deadline_missed_arg(l, on_offered_deadline_missed_fn, arg, false);
  dds_lset_offered_incompatible_qos_arg(l, on_offered_incompatible_qos_fn, arg, false);
  dds_lset_liveliness_changed_arg(l, on_liveliness_changed_fn, arg, false);
  dds_lset_inconsistent_topic_arg(l, on_inconsistent_topic_fn, arg, false);
  dds_lset_subscription_matched_arg(l, on_subscription_matched_fn, arg, false);
  dds_lset_publication_matched_arg(l, on_publication_matched_fn, arg, false);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(rmw_subscription, RMW_RET_INVALID_ARGUMENT);
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

    // For KEEP_ALL history, depth is reported as 0 (since CycloneDDS internally
    // uses -1 for unlimited depth, which gets mapped to 0 in dds_qos_to_rmw_qos).
    // In that case, pass through the full unread_count instead of clipping to 0.
    size_t events = (sub_qos.depth > 0) ?
      std::min(data->unread_count, sub_qos.depth) :
      data->unread_count;

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
  RMW_CHECK_ARGUMENT_FOR_NULL(rmw_service, RMW_RET_INVALID_ARGUMENT);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(rmw_client, RMW_RET_INVALID_ARGUMENT);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(rmw_event, RMW_RET_INVALID_ARGUMENT);
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

    case RMW_EVENT_SUBSCRIPTION_MATCHED:
      {
        auto sub_event = static_cast<CddsSubscription *>(rmw_event->data);
        event_set_callback(
          sub_event, DDS_SUBSCRIPTION_MATCHED_STATUS_ID,
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

    case RMW_EVENT_PUBLISHER_INCOMPATIBLE_TYPE:
      {
        auto pub_event = static_cast<CddsPublisher *>(rmw_event->data);
        event_set_callback(
          pub_event, DDS_INCONSISTENT_TOPIC_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_SUBSCRIPTION_INCOMPATIBLE_TYPE:
      {
        auto sub_event = static_cast<CddsSubscription *>(rmw_event->data);
        event_set_callback(
          sub_event, DDS_INCONSISTENT_TOPIC_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_PUBLICATION_MATCHED:
      {
        auto pub_event = static_cast<CddsPublisher *>(rmw_event->data);
        event_set_callback(
          pub_event, DDS_PUBLICATION_MATCHED_STATUS_ID,
          callback, user_data);
        break;
      }

    case RMW_EVENT_INVALID:
    case RMW_EVENT_TYPE_MAX:
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
  if (nullptr != init_options->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }
  init_options->instance_id = 0;
  init_options->implementation_identifier = eclipse_cyclonedds_identifier;
  init_options->allocator = allocator;
  init_options->impl = nullptr;
  init_options->discovery_options = rmw_get_zero_initialized_discovery_options(),
  init_options->domain_id = RMW_DEFAULT_DOMAIN_ID;
  init_options->enclave = nullptr;
  init_options->security_options = rmw_get_zero_initialized_security_options();
  return rmw_discovery_options_init(&(init_options->discovery_options), 0, &allocator);
}

extern "C" rmw_ret_t rmw_init_options_copy(const rmw_init_options_t * src, rmw_init_options_t * dst)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(src, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(dst, RMW_RET_INVALID_ARGUMENT);
  if (nullptr == src->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized src");
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    init options copy, src->implementation_identifier,
    eclipse_cyclonedds_identifier, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  if (nullptr != dst->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected zero-initialized dst");
    return RMW_RET_INVALID_ARGUMENT;
  }
  const rcutils_allocator_t * allocator = &src->allocator;

  rmw_init_options_t tmp = *src;
  rmw_ret_t ret;
  if (src->enclave != nullptr) {
    ret = rmw_enclave_options_copy(src->enclave, allocator, &tmp.enclave);
    if (RMW_RET_OK != ret) {
      return ret;
    }
  }
  tmp.security_options = rmw_get_zero_initialized_security_options();
  ret =
    rmw_security_options_copy(&src->security_options, allocator, &tmp.security_options);
  if (RMW_RET_OK != ret) {
    rmw_enclave_options_fini(tmp.enclave, allocator);
    // Error already set
    return ret;
  }
  *dst = tmp;
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init_options_fini(rmw_init_options_t * init_options)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(init_options, RMW_RET_INVALID_ARGUMENT);
  if (nullptr == init_options->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected initialized init_options");
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    init options, init_options->implementation_identifier,
    eclipse_cyclonedds_identifier, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  rcutils_allocator_t * allocator = &init_options->allocator;
  RCUTILS_CHECK_ALLOCATOR(allocator, return RMW_RET_INVALID_ARGUMENT);

  rmw_ret_t ret;
  if (init_options->enclave != nullptr) {
    ret = rmw_enclave_options_fini(init_options->enclave, allocator);
    if (ret != RMW_RET_OK) {
      return ret;
    }
  }
  ret = rmw_security_options_fini(&init_options->security_options, allocator);
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

// Builds a rmw_topic_endpoint_info_t for a peer found via a
// publication_matched/subscription_matched callback -- needed by
// create_descriptor_with_endpoint()/from_descriptor_with_endpoint() calls,
// neither of which this RMW has ever had a call site for before. There is
// no existing "look up one entity's node identity by
// GUID" entry point on rmw_dds_common::GraphCache; the public API this
// RMW already calls elsewhere (rmw_get_publishers_info_by_topic()) is
// per-topic instead, so this reuses exactly that: fetch every writer/
// reader known for the peer's own topic, then pick out the one entry
// whose endpoint_gid matches. `handle_builtintopic_endpoint()` above feeds
// the graph cache from the identical `dds_builtintopic_endpoint_t::key`
// via `convert_guid_to_gid()`, which is what makes the two sides
// comparable.
//
// `peer_is_reader` names what the PEER is, not the caller -- a publisher's
// on_publication_matched() has a peer that is a subscription (reader), so
// it passes true; a subscriber's on_subscription_matched() has a peer
// that is a publication (writer), so it passes false.
static rmw_ret_t build_endpoint_info_from_match(
  const rmw_node_t * node, const dds_builtintopic_endpoint_t * ep, bool peer_is_reader,
  rmw_topic_endpoint_info_t * out)
{
  // `node` is null for the internal ros_discovery_info publisher
  // (create_publisher(nullptr, ...) in rmw_context_impl_s::init()). That
  // publisher's type (ParticipantEntitiesInfo) is never buffer-backed, so
  // has_buffer_fields() is false and on_publication_matched() is never
  // registered as its listener -- this callback cannot currently fire for
  // it. Guarding anyway: that non-firing is an invariant of a different
  // function, not something this one can see or enforce, and a future
  // change to either one would otherwise turn an unreachable null into a
  // crash with no local signal of why.
  if (node == nullptr) {
    RMW_SET_ERROR_MSG(
      "build_endpoint_info_from_match: node is null (called for an "
      "internal, non-buffer-backed publisher?)");
    return RMW_RET_ERROR;
  }
  auto common_context = &node->context->impl->common;
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rmw_topic_endpoint_info_array_t array = rmw_get_zero_initialized_topic_endpoint_info_array();
  const std::string topic_name = ep->topic_name;
  const rmw_ret_t lookup_ret = peer_is_reader ?
    common_context->graph_cache.get_readers_info_by_topic(
    topic_name, _demangle_if_ros_type, &allocator, &array) :
    common_context->graph_cache.get_writers_info_by_topic(
    topic_name, _demangle_if_ros_type, &allocator, &array);
  if (RMW_RET_OK != lookup_ret) {
    return lookup_ret;
  }

  rmw_gid_t peer_gid;
  convert_guid_to_gid(ep->key, peer_gid);
  const rmw_topic_endpoint_info_t * match = nullptr;
  for (size_t i = 0; i < array.size; i++) {
    if (0 == memcmp(array.info_array[i].endpoint_gid, peer_gid.data, RMW_GID_STORAGE_SIZE)) {
      match = &array.info_array[i];
      break;
    }
  }
  if (match == nullptr) {
    rmw_topic_endpoint_info_array_fini(&array, &allocator);
    RMW_SET_ERROR_MSG(
      "build_endpoint_info_from_match: matched peer's own GID not found in the graph "
      "cache's per-topic listing -- discovery has not caught up yet");
    return RMW_RET_ERROR;
  }

  rmw_ret_t ret;
  if (RMW_RET_OK != (ret = rmw_topic_endpoint_info_set_node_name(
      out, match->node_name, &allocator)) ||
    RMW_RET_OK != (ret = rmw_topic_endpoint_info_set_node_namespace(
      out, match->node_namespace, &allocator)) ||
    RMW_RET_OK != (ret = rmw_topic_endpoint_info_set_topic_type(
      out, match->topic_type, &allocator)) ||
    RMW_RET_OK != (ret = rmw_topic_endpoint_info_set_topic_type_hash(
      out, &match->topic_type_hash)) ||
    RMW_RET_OK != (ret = rmw_topic_endpoint_info_set_endpoint_type(
      out, peer_is_reader ? RMW_ENDPOINT_SUBSCRIPTION : RMW_ENDPOINT_PUBLISHER)) ||
    RMW_RET_OK != (ret = rmw_topic_endpoint_info_set_gid(
      out, peer_gid.data, RMW_GID_STORAGE_SIZE)) ||
    RMW_RET_OK != (ret = rmw_topic_endpoint_info_set_qos_profile(out, &match->qos_profile)))
  {
    // `out` is NOT finalized here. Every current caller
    // (on_publication_matched/on_subscription_matched's synchronous path
    // and both background retry loops) unconditionally finalizes
    // `out`/`retry_info` on ANY non-OK return, regardless of which failure
    // branch produced it. Finalizing here too would double-free whatever
    // the earlier successful set_* calls allocated. `out` is left
    // populated (mid-chain state) for the caller to finalize instead, same
    // as every other failure path in this function already leaves it.
    rmw_topic_endpoint_info_array_fini(&array, &allocator);
    return ret;
  }

  rmw_ret_t fini_ret = rmw_topic_endpoint_info_array_fini(&array, &allocator);
  if (RMW_RET_OK != fini_ret) {
    return fini_ret;
  }
  return RMW_RET_OK;
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
  void * raw = nullptr;
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
  void * raw = nullptr;
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

      rosidl_type_hash_t type_hash = rosidl_get_zero_initialized_type_hash();
      rosidl_type_hash_t ser_type_hash = rosidl_get_zero_initialized_type_hash();
      rosidl_type_hash_t * ser_type_hash_ptr = nullptr;
      void * userdata;
      size_t userdata_size;
      if (dds_qget_userdata(s->qos, &userdata, &userdata_size)) {
        RCPPUTILS_SCOPE_EXIT(dds_free(userdata));
        if (RMW_RET_OK != rmw_dds_common::parse_type_hash_from_user_data(
            reinterpret_cast<const uint8_t *>(userdata), userdata_size, type_hash))
        {
          RCUTILS_LOG_DEBUG_NAMED(
            "rmw_cyclonedds_cpp",
            "Failed to parse type hash for topic '%s' with type '%s' from USER_DATA '%*s'.",
            s->topic_name, s->type_name,
            static_cast<int>(userdata_size), reinterpret_cast<char *>(userdata));
          type_hash = rosidl_get_zero_initialized_type_hash();
          // We've handled the error, so clear it out.
          rmw_reset_error();
        }
        if (RMW_RET_OK == rmw_dds_common::parse_sertype_hash_from_user_data(
            reinterpret_cast<const uint8_t *>(userdata), userdata_size, ser_type_hash))
        {
          ser_type_hash_ptr = &ser_type_hash;
        }
      }

      impl->common.graph_cache.add_entity(
        gid,
        std::string(s->topic_name),
        std::string(s->type_name),
        type_hash,
        ppgid,
        qos_profile,
        is_reader,
        ser_type_hash_ptr);
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

static bool check_create_domain(dds_domainid_t did, rmw_discovery_options_t * discovery_options)
{
  std::lock_guard<std::mutex> lock(gcdds().domains_lock);
  /* return true: n_nodes incremented, discovery params set correctly, domain exists
     "      false: n_nodes unchanged, domain left intact if it already existed */
  CddsDomain & dom = gcdds().domains[did];
  if (dom.refcount != 0) {
    /* Discovery parameters must match */
    bool options_equal = false;
    const auto rc =
      rmw_discovery_options_equal(discovery_options, &dom.discovery_options, &options_equal);
    if (RMW_RET_OK != rc) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "check_create_domain: unable to check if discovery options are equal: %i",
        rc);
      return false;
    }
    if (options_equal) {
      dom.refcount++;
      return true;
    } else {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "check_create_domain: attempt at creating nodes in the same domain with different "
        "discovery parameters");
      return false;
    }
  } else {
    dom.refcount = 1;
    dom.discovery_options = *discovery_options;

    bool add_localhost_as_static_peer = false;
    bool add_static_peers = false;
    bool disable_multicast = false;

    switch (discovery_options->automatic_discovery_range) {
      case RMW_AUTOMATIC_DISCOVERY_RANGE_NOT_SET:
        RMW_SET_ERROR_MSG("automatic discovery range must be set");
        return false;
        break;
      case RMW_AUTOMATIC_DISCOVERY_RANGE_SUBNET:
        add_localhost_as_static_peer = false;
        add_static_peers = true;
        disable_multicast = false;
        break;
      case RMW_AUTOMATIC_DISCOVERY_RANGE_SYSTEM_DEFAULT:
        /* Avoid changing DDS discovery options*/
        add_localhost_as_static_peer = false;
        add_static_peers = false;
        disable_multicast = false;
        if (discovery_options->static_peers_count > 0) {
          RCUTILS_LOG_WARN_NAMED(
            "rmw_cyclonedds_cpp",
            "check_create_domain: %lu static peers were specified, but discovery is "
            "set to use the RMW implementation default, so these static peers will be ignored.",
            discovery_options->static_peers_count);
        }
        break;
      case RMW_AUTOMATIC_DISCOVERY_RANGE_LOCALHOST:
        /* Automatic discovery on localhost only */
        add_localhost_as_static_peer = true;
        add_static_peers = true;
        disable_multicast = true;
        break;
      case RMW_AUTOMATIC_DISCOVERY_RANGE_OFF:
        /* Automatic discovery off: disable multicast entirely. */
        add_localhost_as_static_peer = false;
        add_static_peers = false;
        disable_multicast = true;
        if (discovery_options->static_peers_count > 0) {
          RCUTILS_LOG_WARN_NAMED(
            "rmw_cyclonedds_cpp",
            "check_create_domain: %lu static peers were specified, but discovery is "
            "turned off, so these static peers will be ignored.",
            discovery_options->static_peers_count);
        }
        break;
    }

    std::string config;
    if (
      add_localhost_as_static_peer ||
      add_static_peers ||
      disable_multicast)
    {
      config = "<CycloneDDS><Domain>";

      if (disable_multicast) {
        config += "<General><AllowMulticast>false</AllowMulticast></General>";
      }

      const bool discovery_off =
        disable_multicast && !add_localhost_as_static_peer && !add_static_peers;
      if (discovery_off) {
        /* This means we have an OFF range, so we should use the domain tag to
          block all attemtps at automatic discovery. Another participant would
          need to use this exact same domain tag, down to the PID, to discover
          the endpoints of this node.

          Setting ParticipantIndex to none eliminates the 119 limit on the number
          of participants on a machine.
          */
        config += "<Discovery><ParticipantIndex>none</ParticipantIndex>";
        config += "<Tag>ros_discovery_off_" + std::to_string(rcutils_get_pid()) + "</Tag>";
      } else {
        config += "<Discovery><ParticipantIndex>auto</ParticipantIndex>";
        // This controls the number of participants that can be discovered on a single host,
        // which is roughly equivalent to the number of ROS 2 processes.
        // If it's too small then we won't connect to all participants.
        // If it's too large then we will send a lot of announcement traffic.
        // The default number here is picked arbitrarily.
        config += "<MaxAutoParticipantIndex>32</MaxAutoParticipantIndex>";
      }

      if (  // NOLINT
        (add_static_peers && discovery_options->static_peers_count > 0) ||
        add_localhost_as_static_peer)
      {
        config += "<Peers>";

        if (add_localhost_as_static_peer) {
          config += "<Peer address=\"localhost\"/>";
        }

        for (size_t ii = 0; ii < discovery_options->static_peers_count; ++ii) {
          config += "<Peer address=\"";
          config += discovery_options->static_peers[ii].peer_address;
          config += "\"/>";
        }
        config += "</Peers>";
      }
      /* NOTE: Empty configuration fragments are ignored, so it is safe to
        unconditionally append a comma. */
      config += "</Discovery></Domain></CycloneDDS>,";
    }

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

    RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "Config XML is %s", config.c_str());

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
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  rcutils_string_map_t security_files = rcutils_get_zero_initialized_string_map();
  rcutils_ret_t ret = rcutils_string_map_init(&security_files, 0, allocator);

  if (ret != RMW_RET_OK) {
    RMW_SET_ERROR_MSG("Failed to initialize string map for security");
    return RMW_RET_ERROR;
  }

  auto scope_exit_ws = rcpputils::make_scope_exit(
    [&security_files]()
    {
      rcutils_ret_t ret = rcutils_string_map_fini(&security_files);
      if (ret != RMW_RET_OK) {
        RMW_SET_ERROR_MSG("Failed to fini string map for security");
      }
    });

  if (security_options->security_root_path == nullptr) {
    return RMW_RET_UNSUPPORTED;
  }

  if (get_security_files(
      "file:", security_options->security_root_path, &security_files) != RMW_RET_OK)
  {
    RCUTILS_LOG_INFO_NAMED(
      "rmw_cyclonedds_cpp", "could not find all security files");
    return RMW_RET_UNSUPPORTED;
  }

  dds_qset_prop(
    qos, "dds.sec.auth.identity_ca",
    std::string(rcutils_string_map_get(&security_files, "IDENTITY_CA")).c_str());
  dds_qset_prop(
    qos, "dds.sec.auth.identity_certificate",
    std::string(rcutils_string_map_get(&security_files, "CERTIFICATE")).c_str());
  dds_qset_prop(
    qos, "dds.sec.auth.private_key",
    std::string(rcutils_string_map_get(&security_files, "PRIVATE_KEY")).c_str());
  dds_qset_prop(
    qos, "dds.sec.access.permissions_ca",
    std::string(rcutils_string_map_get(&security_files, "PERMISSIONS_CA")).c_str());
  dds_qset_prop(
    qos, "dds.sec.access.governance",
    std::string(rcutils_string_map_get(&security_files, "GOVERNANCE")).c_str());
  dds_qset_prop(
    qos, "dds.sec.access.permissions",
    std::string(rcutils_string_map_get(&security_files, "PERMISSIONS")).c_str());

  dds_qset_prop(qos, "dds.sec.auth.library.path", "dds_security_auth");
  dds_qset_prop(qos, "dds.sec.auth.library.init", "init_authentication");
  dds_qset_prop(qos, "dds.sec.auth.library.finalize", "finalize_authentication");

  dds_qset_prop(qos, "dds.sec.crypto.library.path", "dds_security_crypto");
  dds_qset_prop(qos, "dds.sec.crypto.library.init", "init_crypto");
  dds_qset_prop(qos, "dds.sec.crypto.library.finalize", "finalize_crypto");

  dds_qset_prop(qos, "dds.sec.access.library.path", "dds_security_ac");
  dds_qset_prop(qos, "dds.sec.access.library.init", "init_access_control");
  dds_qset_prop(qos, "dds.sec.access.library.finalize", "finalize_access_control");

  if (rcutils_string_map_key_exists(&security_files, "CRL")) {
    dds_qset_prop(
      qos, "org.eclipse.cyclonedds.sec.auth.crl",
      std::string(rcutils_string_map_get(&security_files, "CRL")).c_str());
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

#ifdef __linux__
  {
    // rmem_max is a system-wide setting, so warn only once per process
    static std::once_flag rmem_max_warn_once;
    std::call_once(
      rmem_max_warn_once, []() {
        std::ifstream rmem_max_file("/proc/sys/net/core/rmem_max");
        if (rmem_max_file.is_open()) {
          size_t rmem_max = 0;
          rmem_max_file >> rmem_max;
          if (rmem_max < 8388608) {
            RCUTILS_LOG_WARN_NAMED(
              "rmw_cyclonedds_cpp",
              "system rmem_max (%zu) is lower than the recommended minimum of 8388608. "
              "Increase it: sudo sysctl -w net.core.rmem_max=8388608",
              rmem_max);
          }
        }
      });
  }
#endif

  /* Take domains_lock and hold it until after the participant creation succeeded or
    failed: otherwise there is a race with rmw_destroy_node deleting the last participant
    and tearing down the domain for versions of Cyclone that implement the original
    version of dds_create_domain that doesn't return a handle.  */
  this->domain_id = static_cast<dds_domainid_t>(domain_id);

  if (!check_create_domain(this->domain_id, &options->discovery_options)) {
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
    nullptr, this->ppant, this->dds_pub,
    rosidl_typesupport_cpp::get_message_type_support_handle<ParticipantEntitiesInfo>(),
    "ros_discovery_info",
    &pubsub_qos,
    &publisher_options);
  if (this->common.pub == nullptr) {
    this->clean_up();
    return RMW_RET_ERROR;
  }
  this->common.publish_callback = [](const rmw_publisher_t * pub, const void * msg) {
      return rmw_publish(
      pub,
      msg,
      nullptr);
    };

  rmw_subscription_options_t subscription_options = rmw_get_default_subscription_options();
  subscription_options.ignore_local_publications = true;
  // FIXME: keyed topics => KEEP_LAST and depth 1.
  pubsub_qos.history = RMW_QOS_POLICY_HISTORY_KEEP_ALL;
  this->common.sub = create_subscription(
    nullptr, this->ppant, this->dds_sub,
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
  if (common.publish_callback) {
    common.publish_callback = nullptr;
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
#if CDDS_VERSION == CDDS_VERSION_0_10
  if (alloc_on_heap) {
    if (dds_data_allocator_init_heap(&entity->data_allocator) != DDS_RETCODE_OK) {
      RMW_SET_ERROR_MSG("Reader data allocator initialization failed for heap");
      return nullptr;
    }
  } else {
    if (dds_data_allocator_init(entity->enth, &entity->data_allocator) != DDS_RETCODE_OK) {
      RMW_SET_ERROR_MSG("Writer allocator initialisation failed");
      return nullptr;
    }
  }
  // allocate memory for message + header
  // the header will be initialized and the chunk pointer will be returned
  auto chunk_ptr = dds_data_allocator_alloc(&entity->data_allocator, sample_size);
#else
  static_cast<void>(alloc_on_heap);
  void * chunk_ptr;
  if (dds_request_loan_of_size(entity->enth, sample_size, &chunk_ptr) != DDS_RETCODE_OK) {
    chunk_ptr = nullptr;
  }
#endif
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
  // handling C++ typesupport
  const rosidl_message_type_support_t * ts = get_message_typesupport_handle(
    &entity->type_supports, rosidl_typesupport_introspection_cpp::typesupport_identifier);
  if (ts != nullptr) {
    auto members =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(ts->data);
    members->fini_function(loaned_message);
  } else {
    // handle C Typesupport
    const rosidl_message_type_support_t * ts_c = get_message_typesupport_handle(
      &entity->type_supports, rosidl_typesupport_introspection_c__identifier);
    if (ts_c != nullptr) {
      auto members =
        static_cast<const rosidl_typesupport_introspection_c__MessageMembers *>(ts_c->data);
      members->fini_function(loaned_message);
    } else {
      throw std::runtime_error("fini_message, unsupported typesupport");
    }
  }

#if CDDS_VERSION == CDDS_VERSION_0_10
  // free the message memory
  if (dds_data_allocator_free(&entity->data_allocator, loaned_message) != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("Failed to free the loaned message");
    return RMW_RET_ERROR;
  }
  // fini the allocator
  if (dds_data_allocator_fini(&entity->data_allocator) != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("Failed to fini data allocator");
    return RMW_RET_ERROR;
  }
#else
  if (dds_return_loan(entity->enth, &loaned_message, 1) != DDS_RETCODE_OK) {
    RMW_SET_ERROR_MSG("Failed to free the loaned message");
    return RMW_RET_ERROR;
  }
#endif
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
    options, options->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    options->enclave,
    "expected non-null enclave",
    return RMW_RET_INVALID_ARGUMENT);
  if (nullptr != context->implementation_identifier) {
    RMW_SET_ERROR_MSG("expected a zero-initialized context");
    return RMW_RET_INVALID_ARGUMENT;
  }

  if (options->domain_id >= UINT32_MAX && options->domain_id != RMW_DEFAULT_DOMAIN_ID) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_init: domain id out of range");
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
    context, context->implementation_identifier, eclipse_cyclonedds_identifier,
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
    context, context->implementation_identifier, eclipse_cyclonedds_identifier,
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
    context, context->implementation_identifier, eclipse_cyclonedds_identifier,
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
    const char * reason = rmw_namespace_validation_result_string(validation_result);
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

  auto common = &context->impl->common;
  rmw_ret_t rmw_ret = common->add_node_graph(
    name, namespace_);
  if (RMW_RET_OK != rmw_ret) {
    return nullptr;
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto node_impl = static_cast<CddsNode *>(node->data);

  auto common = &node->context->impl->common;
  result_ret = common->remove_node_graph(
    node->name, node->namespace_);

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
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return nullptr);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL_X(node_impl, return nullptr);
  return node->context->impl->common.graph_guard_condition;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    (DE)SERIALIZATION                                              ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

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
    auto message_value_type = rmw_cyclonedds_cpp::make_message_value_type(type_support);
    auto writer = rmw_cyclonedds_cpp::make_cdr_writer(
      message_value_type.get(),
      rmw_cyclonedds_cpp::SampleOrRequest::Sample);
    auto size = writer->get_serialized_size(ros_message, rmw_cyclonedds_cpp::SampleOrKey::Sample);
    rmw_ret_t ret = rmw_serialized_message_resize(serialized_message, size);
    if (RMW_RET_OK != ret) {
      rmw_reset_error();
      RMW_SET_ERROR_MSG("rmw_serialize: failed to allocate space for message");
      return ret;
    }
    writer->serialize(
      serialized_message->buffer, ros_message,
      rmw_cyclonedds_cpp::SampleOrKey::Sample);
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
  try {
    auto message_value_type = rmw_cyclonedds_cpp::make_message_value_type(type_support);
    auto reader = rmw_cyclonedds_cpp::make_cdr_reader(
      message_value_type.get(),
      rmw_cyclonedds_cpp::SampleOrRequest::Sample);
    reader->deserialize(
      ros_message, serialized_message->buffer, serialized_message->buffer_length,
      rmw_cyclonedds_cpp::SampleOrKey::Sample);
    return RMW_RET_OK;
  } catch (std::exception & e) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING("rmw_serialize: %s", e.what());
  }
  return RMW_RET_ERROR;
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
  tp = dds_create_topic_sertype(pp, name, &sertype, nullptr, nullptr, nullptr);
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
  dds_time_t tstamp = dds_time();
  // The timestamp-collision fix below must NOT be gated only on
  // `pub->has_buffer_fields` -- that is true for any publisher whose
  // MESSAGE TYPE merely contains a buffer-backed field, regardless of
  // whether any peer has actually negotiated a non-CPU backend, and gating
  // on it alone would make EVERY publish on a buffer-capable topic get its
  // source_timestamp silently replaced by a synthetic counter and written
  // to the ordinary SHARED topic -- visible to every subscriber of that
  // topic, including ones with no buffer backend and no interest in the
  // zero-copy path -- even when zero-copy is never used with any peer.
  // Gated instead on the same condition the private-write block below
  // already uses (a non-empty peer map), checked under the same
  // shared_lock reused for that block, so the correlation mechanism only
  // touches source_timestamp when there is actually something to
  // correlate.
  std::shared_lock<std::shared_mutex> buffer_lock;
  bool has_negotiated_peer = false;
  if (pub->has_buffer_fields) {
    buffer_lock = std::shared_lock<std::shared_mutex>(pub->buffer_discovery.mutex);
    has_negotiated_peer = !pub->buffer_discovery.private_writers_or_readers.empty();
  }
  // consume_buffer_descriptor() correlates a message with its
  // private-topic descriptor by exact dds_time_t equality (both writes
  // below share `tstamp`). dds_time()'s finite resolution means two rapid
  // rmw_publish() calls on the SAME publisher can legitimately return the
  // identical value; if the first one's private write is skipped
  // (documented CPU-fallback) while the second's succeeds under the same
  // timestamp, the exact-match lookup can install the second message's
  // descriptor into the first's buffer field.
  //
  // The skew from true wall-clock time introduced by the counter below is
  // NOT bounded to "at most one nanosecond per colliding call, never
  // accumulating drift" in general -- that would be false if dds_time()
  // ever REGRESSES below `prev` (an NTP step backward, clock resync, VM
  // migration): every subsequent publish would take the `prev+1` branch
  // forever, since real time must first count back up to the artificially
  // -advanced counter, corrupting source_timestamp (seen by every
  // subscriber, including DEADLINE/LIFESPAN/TIME_BASED_FILTER QoS
  // consumers, not just the descriptor-matching one this exists for) for
  // the rest of the publisher's lifetime. A same-nanosecond collision and
  // a real clock regression are told apart by MAGNITUDE: a collision means
  // `tstamp <= prev` by at most a handful of nanoseconds (both readings
  // happen microseconds apart on the same call path); a real regression is
  // orders of magnitude larger. Past this bound, trust the fresh reading
  // over the counter instead of perpetuating the divergence -- accepting
  // the same, bounded collision risk this mechanism exists to close, for
  // the one call right after a genuine clock regression, rather than an
  // unbounded one for every call after it.
  if (has_negotiated_peer) {
    static constexpr dds_time_t kMaxCollisionCatchupNs = 1'000'000LL;  // 1ms
    dds_time_t prev = pub->last_buffer_tstamp.load(std::memory_order_relaxed);
    dds_time_t candidate;
    do {
      if (tstamp > prev) {
        candidate = tstamp;
      } else if (prev - tstamp > kMaxCollisionCatchupNs) {
        // A real clock regression, not a same-instant collision -- resync
        // to the fresh reading rather than keep advancing the counter.
        candidate = tstamp;
      } else {
        candidate = prev + 1;
      }
    } while (!pub->last_buffer_tstamp.compare_exchange_weak(
        prev, candidate, std::memory_order_relaxed));
    tstamp = candidate;
  }
  TRACETOOLS_TRACEPOINT(rmw_publish, (const void *)publisher, ros_message, tstamp);
  if (dds_write_ts(pub->enth, ros_message, tstamp) < 0) {
    RMW_SET_ERROR_MSG("failed to publish data");
    return RMW_RET_ERROR;
  }

  // Writes a descriptor object -- not the original message -- through
  // each matched peer's private writer. This is the actual consumer of
  // the discovery mechanism and endpoint-info helpers above; before this,
  // on_publication_matched() created private writers that nothing ever
  // wrote to.
  //
  // has_buffer_fields is checked BEFORE taking the mutex, not just before
  // reading the map: this is the universal publish hot path, called for
  // every publisher whether buffer-backed or not, and a publisher with this
  // false can never have on_publication_matched registered as a listener at
  // all (see create_cdds_publisher) -- its map is provably always empty, so
  // there is nothing here for the lock to protect for it.
  // A failed private write deliberately does NOT return RMW_RET_ERROR
  // from this function: rclcpp/application callers treat a non-OK
  // rmw_publish() return as "nothing was sent" and may resend -- but the
  // shared-topic write above this comment already succeeded, so a resend
  // after this error would be a DUPLICATE delivery to every shared-topic
  // subscriber, not a retry of a failed send. That would invert the
  // existing rmw_publish() contract rather than extend it, so a failed
  // private write is logged only.
  if (pub->has_buffer_fields) {
    // A plain mutex held across this whole loop would serialize
    // rmw_publish against on_publication_matched's own use of the same
    // mutex (its cheap early duplicate-check gate) for as long as the
    // backend calls below take; it must also tolerate
    // destroy_publisher()/destroy_subscription(), which DO clear() this
    // map. A shared_lock resolves both: compatible with the match
    // callbacks' own read-only checks (also shared_lock -- see those call
    // sites), but mutually exclusive with the callbacks' emplace() and
    // destroy's clear() (both unique_lock) -- see
    // BufferEndpointDiscovery.hpp's own comment on the mutex.
    //
    // Reuses buffer_lock (acquired above, before the timestamp-
    // monotonicity block) instead of taking its own separate shared_lock
    // here: both blocks read the identical map under the identical mutex,
    // and has_negotiated_peer above is exactly this emptiness check,
    // computed once under one lock rather than twice under two.
    //
    // consume_buffer_descriptor()'s matching shared_lock carries the same
    // symmetric writer-starvation risk -- std::shared_mutex has no
    // default writer-priority guarantee on Linux, and this lock is held
    // across the identical backend-call loop below.
    if (has_negotiated_peer) {
      // create_cdds_publisher() builds the introspection type tree once
      // and caches it on pub->buffer_message_value_type, rather than
      // rebuilding it via make_message_value_type() on EVERY publish
      // call.
      //
      // find_buffer_impl() returns only the FIRST buffer-backed field
      // found (deliberate scope boundary -- see its own comment). Every
      // peer below shares this one impl pointer.
      const void * impl = rmw_cyclonedds_cpp::find_buffer_impl(
        pub->buffer_message_value_type.get(), ros_message);
      // `impl` is ONE concrete BufferImplBase<uint8_t> subtype --
      // whatever the application actually constructed the message's
      // buffer field with -- but each
      // peer below may have negotiated a DIFFERENT backend TYPE NAME
      // (common_backend_types is intersected per-peer in
      // on_publication_matched, from RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS'
      // possibly-multi-name list). Passing impl to a peer whose backend
      // does not match impl's own real type let a backend's
      // create_descriptor_with_endpoint() static_cast impl to the WRONG
      // concrete class with no runtime check -- exactly what the new test
      // backend's own doc comment discloses as unsafe outside this test
      // suite's own controlled traffic.
      // This cast assumes the element type is uint8_t, and
      // rosidl::BufferImplBase<T> is templated on arbitrary T, so a buffer
      // field instantiated with a different element type would
      // reinterpret the wrong template instantiation (UB) with no runtime
      // tag to catch it. This is not reachable through any message
      // rosidl's own generator can currently produce, not merely
      // undemonstrated in this project's own message set.
      // rosidl_generator_cpp's idl__struct.hpp.em (resource/
      // idl__struct.hpp.em, the template that maps a .msg field's
      // declared type to a C++ member type) comments its own rule
      // explicitly: "unbounded uint8 sequences map to rosidl::Buffer" --
      // singular, uint8 only. No other primitive sequence type is ever
      // generated as a rosidl::Buffer<T> field by current tooling, so the
      // T this cast assumes is the ONLY T the generator that produces
      // these fields is capable of emitting. Genuinely scope-limited, the
      // same way the single-buffer-field limitation above is: closing it
      // for real would need a generator change upstream, at which point
      // this cast would need a matching runtime element-size or type-id
      // check before dispatching.
      const rosidl::BufferImplBase<uint8_t> * typed_impl =
        static_cast<const rosidl::BufferImplBase<uint8_t> *>(impl);
      for (auto & entry : pub->buffer_discovery.private_writers_or_readers) {
        if (impl == nullptr || !entry.second.backend) {
          continue;
        }
        // get_backend_type() is NOT assumed non-throwing on either
        // interface -- neither BufferBackend::get_backend_type() nor
        // BufferImplBase<T>::get_backend_type() is declared noexcept.
        // Guarded the same way every other virtual call into
        // plugin/producer-supplied code on this extern "C" hot path
        // already is: an exception is treated like a type mismatch (skip
        // this peer, fall back to the shared write already delivered
        // above).
        bool backend_type_matches;
        try {
          backend_type_matches =
            typed_impl->get_backend_type() == entry.second.backend->get_backend_type();
        } catch (const std::exception & e) {
          RCUTILS_LOG_ERROR_NAMED(
            "rmw_cyclonedds_cpp", "rmw_publish: get_backend_type threw: %s", e.what());
          continue;
        }
        if (!backend_type_matches) {
          continue;
        }
        // A backend legitimately returning nullptr here ("the peer does not
        // support this backend", its own doc comment) is not an error -- it
        // is the documented signal to fall back to the ordinary CPU path,
        // which the shared writer above already delivered to this peer.
        //
        // A per-peer mutex on entry.second alone would NOT actually
        // serialize concurrent callers of this backend -- pluginlib caches
        // backend instances by class name, so two different peers
        // negotiating the same backend TYPE share the identical
        // rosidl::BufferBackend object underneath two different
        // PrivateEndpoint entries, each with its own mutex. The lock that
        // actually matters is keyed by backend TYPE, not by peer -- see
        // backend_instance_mutex()'s own comment in BufferEndpointDiscovery.
        // create_descriptor_with_endpoint() is vendor/plugin code
        // (rosidl::BufferBackend, loaded via pluginlib) with no noexcept
        // guarantee, called here from rmw_publish -- an extern "C" API
        // boundary, and a hot one. An exception is treated exactly like
        // the documented nullptr return: fall back to the ordinary CPU
        // path for this peer.
        std::shared_ptr<void> descriptor;
        try {
          std::lock_guard<std::mutex> backend_lock(
            rmw_cyclonedds_cpp::backend_instance_mutex(entry.second.backend->get_backend_type()));
          descriptor = entry.second.backend->create_descriptor_with_endpoint(
            impl, entry.second.endpoint_info);
        } catch (const std::exception & e) {
          RCUTILS_LOG_ERROR_NAMED(
            "rmw_cyclonedds_cpp", "rmw_publish: create_descriptor_with_endpoint threw: %s",
            e.what());
          continue;
        }
        if (!descriptor) {
          continue;
        }
        if (dds_write_ts(entry.second.entity, descriptor.get(), tstamp) < 0) {
          RCUTILS_LOG_ERROR_NAMED(
            "rmw_cyclonedds_cpp", "rmw_publish: failed to write descriptor to private writer");
        }
      }
    }
  }
  return RMW_RET_OK;
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
  const dds_time_t tstamp = dds_time();
  TRACETOOLS_TRACEPOINT(rmw_publish, (const void *)publisher, serialized_message, tstamp);

  struct ddsi_serdata * d = serdata_rmw_from_serialized_message(
    pub->sertype, SDK_DATA, serialized_message->buffer, serialized_message->buffer_length);
  d->timestamp.v = tstamp;
  d->statusinfo = 0;

#if CDDS_VERSION == CDDS_VERSION_0_10 && defined DDS_HAS_SHM
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

  const bool ok = (dds_forwardcdr(pub->enth, d) >= 0);
  return ok ? RMW_RET_OK : RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_publish_loaned_message(
  const rmw_publisher_t * publisher,
  void * ros_message,
  rmw_publisher_allocation_t * allocation)
{
#if CDDS_VERSION > CDDS_VERSION_0_10
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  if (!publisher->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  return rmw_publish(publisher, ros_message, allocation);
#elif defined DDS_HAS_SHM
  static_cast<void>(allocation);
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
    const dds_time_t tstamp = dds_time();
    d->timestamp.v = tstamp;
    d->statusinfo = 0;
    TRACETOOLS_TRACEPOINT(rmw_publish, (const void *)publisher, ros_message, tstamp);
    if (dds_forwardcdr(cdds_publisher->enth, d) >= 0) {
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
  static_cast<void>(allocation);
  RMW_SET_ERROR_MSG("rmw_publish_loaned_message not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
#endif
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
  const rosidl_type_hash_t & type_hash,
  bool ignore_local_publications,
  const std::string & extra_user_data,
  bool has_buffer_fields = false)
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
  }
  switch (qos_policies->reliability) {
    case RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_RELIABILITY_BEST_AVAILABLE:
    case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
      dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_INFINITY);
      break;
    case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
      dds_qset_reliability(qos, DDS_RELIABILITY_BEST_EFFORT, 0);
      break;
    case RMW_QOS_POLICY_RELIABILITY_UNKNOWN:
      return nullptr;
  }
  switch (qos_policies->durability) {
    case RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT:
    case RMW_QOS_POLICY_DURABILITY_BEST_AVAILABLE:
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
    case RMW_QOS_POLICY_LIVELINESS_BEST_AVAILABLE:
      dds_qset_liveliness(qos, DDS_LIVELINESS_AUTOMATIC, ldur);
      break;
    case RMW_QOS_POLICY_LIVELINESS_UNKNOWN:
      return nullptr;
  }
  if (ignore_local_publications) {
    dds_qset_ignorelocal(qos, DDS_IGNORELOCAL_PARTICIPANT);
  }

  std::string typehash_str;
  if (RMW_RET_OK != rmw_dds_common::encode_type_hash_for_user_data_qos(type_hash, typehash_str)) {
    RCUTILS_LOG_WARN_NAMED(
      "rmw_cyclonedds_cpp",
      "Failed to encode type hash for topic, will not distribute it in USER_DATA.");
    typehash_str.clear();
    // We've handled the error, so clear it out.
    rmw_reset_error();
  }
  std::string user_data = extra_user_data + typehash_str;
  // Advertise buffer-backend capability, appended as its own
  // key=value entry in the SAME format rmw_dds_common::
  // encode_type_hash_for_user_data_qos() already uses (confirmed from its
  // real upstream source: "typehash=" + value + ";") and that
  // parse_user_data()/rmw::impl::cpp::parse_key_value() already parses as a
  // map, not a single opaque blob. Appending a new ";"-terminated key is
  // exactly what this format is for -- it cannot disturb the existing
  // typehash entry's own decode, which is keyed, not positional. "cpu" is
  // always included: every buffer-backed field always has a CPU fallback
  // (the safe CPU-fallback path already exists), so it is always true.
  //
  // Advertising alone gives every buffer-backed endpoint the SAME
  // "bufbackends=cpu" -- there is no way yet for one specific endpoint to
  // declare it wants or offers a real non-CPU backend, which is exactly the
  // signal G2/G3's discovery callbacks need to decide whether to create a
  // private topic. RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS is that signal: a
  // comma-separated list of ADDITIONAL backend names (beyond "cpu") this
  // process's own buffer-backed endpoints advertise, read once per endpoint
  // creation. Unset (the default): behavior is unchanged from G1 alone,
  // "bufbackends=cpu" only -- this is the measured regression floor
  // measured, and stays true since nothing sets this variable in that path.
  if (has_buffer_fields) {
    std::string backends = "cpu";
    if (const char * extra = std::getenv("RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS")) {
      // Only advertise a name this process can actually resolve via
      // find_backend_by_type(). Without
      // this, two peers' mutually-ADVERTISED intersection could still
      // contain a name only one side's registry can actually load a
      // plugin for -- an env-var typo or an uninstalled plugin silently
      // reproducing the exact sertype-mismatch class this PR fixes, from
      // a different cause than the ordering bug it already fixes.
      std::string extra_str(extra);
      size_t start = 0;
      while (start < extra_str.size()) {
        size_t comma = extra_str.find(',', start);
        std::string name = extra_str.substr(
          start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!name.empty() && name != "cpu") {
          if (rmw_cyclonedds_cpp::backend_locally_available(name)) {
            backends += "," + name;
          } else {
            RCUTILS_LOG_WARN_NAMED(
              "rmw_cyclonedds_cpp",
              "RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS names '%s', but no locally-loadable "
              "backend plugin reports that type -- not advertising it", name.c_str());
          }
        }
        if (comma == std::string::npos) {
          break;
        }
        start = comma + 1;
      }
    }
    user_data += "bufbackends=" + backends + ";";
  }
  dds_qset_userdata(qos, user_data.data(), user_data.size());

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
    case DDS_INVALID_QOS_POLICY_ID:
      return RMW_QOS_POLICY_INVALID;
    case DDS_USERDATA_QOS_POLICY_ID:
    case DDS_PRESENTATION_QOS_POLICY_ID:
    case DDS_LATENCYBUDGET_QOS_POLICY_ID:
    case DDS_OWNERSHIP_QOS_POLICY_ID:
    case DDS_OWNERSHIPSTRENGTH_QOS_POLICY_ID:
    case DDS_TIMEBASEDFILTER_QOS_POLICY_ID:
    case DDS_PARTITION_QOS_POLICY_ID:
    case DDS_DESTINATIONORDER_QOS_POLICY_ID:
    case DDS_RESOURCELIMITS_QOS_POLICY_ID:
    case DDS_ENTITYFACTORY_QOS_POLICY_ID:
    case DDS_WRITERDATALIFECYCLE_QOS_POLICY_ID:
    case DDS_READERDATALIFECYCLE_QOS_POLICY_ID:
    case DDS_TOPICDATA_QOS_POLICY_ID:
    case DDS_GROUPDATA_QOS_POLICY_ID:
    case DDS_TRANSPORTPRIORITY_QOS_POLICY_ID:
    case DDS_DURABILITYSERVICE_QOS_POLICY_ID:
    case DDS_PROPERTY_QOS_POLICY_ID:
    case DDS_TYPE_CONSISTENCY_ENFORCEMENT_QOS_POLICY_ID:
    case DDS_DATA_REPRESENTATION_QOS_POLICY_ID:
      return RMW_QOS_POLICY_INVALID;
  }
  return RMW_QOS_POLICY_INVALID;
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
    }
    qos_policies->liveliness_lease_duration = dds_duration_to_rmw(lease_duration);
  }

  return true;
}

// on_publication_matched and on_subscription_matched must NOT each
// independently pick a backend by trying the PEER's advertised list
// against their OWN local registry -- a registry can hold a plugin for a
// backend this side never chose to
// ADVERTISE (RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS is per-process, and an
// installed plugin is not the same fact as an advertised one). The two
// sides also read DIFFERENT peer lists (the publisher reads the matched
// subscriber's advertisement, the subscriber reads the matched publisher's)
// in different orders, so "first common name in the peer's list order" is
// not symmetric -- both sides could independently and silently converge on
// DIFFERENT backends for the one private topic they must agree on.
//
// Instead, this computes the intersection of what BOTH sides actually
// advertised (own_non_cpu_backend_types(handle) reads the SAME QoS
// user_data key this side itself set, so it's exactly what this endpoint
// offered), sort it into one canonical order, and pick from that. The
// intersection of two sets is commutative -- {peer} ∩ {own} is IDENTICAL
// regardless of which side computes it or which order either side listed
// its own names in -- so a fixed sort of that fixed set gives both sides
// the same answer.
static std::vector<std::string> own_non_cpu_backend_types(dds_entity_t handle)
{
  dds_qos_t * qos = dds_create_qos();
  std::vector<std::string> result;
  if (dds_get_qos(handle, qos) >= 0) {
    result = rmw_cyclonedds_cpp::all_non_cpu_backend_types(qos);
  }
  dds_delete_qos(qos);
  return result;
}

// The tail of on_publication_matched from the duplicate re-check through
// the emplace, factored out so both the
// synchronous fast path (graph cache already had the peer) and the
// deferred retry thread below (graph cache caught up later) can call the
// same code once `endpoint_info` is actually built. Takes `endpoint_info`
// by value and is responsible for finalizing it on every return path.
static void finish_publication_match(
  CddsPublisher * pub, dds_entity_t writer, const rmw_cyclonedds_cpp::PeerGuid & peer_guid,
  const std::string & private_name, const std::vector<std::string> & common_backend_types,
  rmw_topic_endpoint_info_t endpoint_info)
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  {
    std::shared_lock<std::shared_mutex> lock(pub->buffer_discovery.mutex);
    if (pub->buffer_discovery.private_writers_or_readers.count(peer_guid) > 0) {
      rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
      return;
    }
  }

  // Resolve a live backend instance for the peer's advertised type.
  // find_backend_by_type() is what makes this a name lookup instead of a
  // hardcoded plugin.
  //
  // find_backend_by_type() -> BufferBackendRegistry::create_backend_instance()
  // is a pluginlib::ClassLoader instantiate call on the registry this
  // CddsPublisher shares across every peer match -- and this callback fires
  // concurrently for the same publisher (the race the emplace() below
  // already guards against). Locking only around this one call, rather than
  // the whole function, keeps the mutex off the slow path (topic/writer
  // creation) while still serializing the one call that is not itself
  // documented as safe for concurrent use.
  // A per-CddsPublisher registry would make it possible for "advertised"
  // (create_readwrite_qos's env-var read) and "locally resolvable" (this
  // lookup) to diverge silently. Both are instead backed by the SAME
  // process-wide registry (rmw_cyclonedds_cpp::process_backend_registry())
  // -- create_readwrite_qos only ever advertises a name
  // backend_locally_available() already confirmed, so every name in
  // common_backend_types is guaranteed resolvable here.
  std::shared_ptr<rosidl::BufferBackend> backend;
  std::string backend_type;
  // This whole call is guarded by try/catch rather than treated as dead
  // code on the theory that create_backend_instance()
  // (find_backend_by_type()'s only pluginlib call) already catches
  // internally. True for that call alone -- find_backend_by_type()
  // (backend_utils.hpp) also calls the resolved backend's own
  // get_backend_type() afterward, outside that try/catch, and that
  // virtual call is unguarded. This runs on the synchronous CycloneDDS
  // listener-callback path AND from the detached async-retry thread -- an
  // uncaught exception is UB at the C-callback ABI boundary in the first
  // case and calls std::terminate() in the second. The try/catch is
  // per-candidate, inside the loop below, rather than wrapped around the
  // WHOLE for-loop: a single try/catch around the loop would break this
  // function's own multi-candidate fallback design
  // (all_non_cpu_backend_types' own doc comment: "try each in turn ...
  // rather than giving up after the first one") -- a throw on a non-last
  // candidate would abandon every remaining candidate, where a plain null
  // return continues to the next one. Per-candidate placement means a
  // throwing candidate is treated exactly like one that returns null.
  {
    std::lock_guard<std::mutex> lock(rmw_cyclonedds_cpp::process_backend_registry_mutex());
    for (const auto & candidate : common_backend_types) {
      try {
        backend = rosidl_buffer_backend_registry::find_backend_by_type(
          rmw_cyclonedds_cpp::process_backend_registry(), candidate);
      } catch (const std::exception & e) {
        RCUTILS_LOG_ERROR_NAMED(
          "rmw_cyclonedds_cpp",
          "on_publication_matched: find_backend_by_type threw for candidate '%s': %s, "
          "trying next candidate", candidate.c_str(), e.what());
        backend.reset();
        continue;
      }
      if (backend) {
        backend_type = candidate;
        break;
      }
    }
  }
  if (!backend) {
    std::string tried;
    for (const auto & candidate : common_backend_types) {
      tried += (tried.empty() ? "" : ",") + candidate;
    }
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_publication_matched: no buffer backend plugin registered for any of the common "
      "set '%s', skipping private topic '%s'", tried.c_str(), private_name.c_str());
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }

  // The private topic's sertype comes from the resolved backend's OWN
  // descriptor type support -- an ordinary, unmodified ROS message type
  // (confirmed from rosidl::BufferBackend's own doc comment). CycloneDDS's
  // generic serializer for it is exactly the one every other ordinary
  // message type already gets; the descriptor path needs no change to the
  // wire-level writer at all, only a private topic whose sertype names
  // the backend's own type. on_subscription_matched above mirrors this
  // same sertype-resolution logic.
  //
  // get_descriptor_type_support() is an unguarded virtual call into
  // third-party pluginlib-loaded code, at the same
  // synchronous-listener-callback / detached-retry-thread boundaries the
  // sibling create_descriptor_with_endpoint() call (rmw_publish) is
  // already guarded for. Treated the same way as a null return below: log
  // and skip the private topic for this peer.
  //
  // The try/catch alone only guards the ABI boundary, not concurrent
  // access to the shared instance -- pluginlib's createSharedInstance()
  // caches backend instances by class name (backend_instance_mutex()'s
  // own doc comment), so two peers resolving the same backend type share
  // one rosidl::BufferBackend object. Every other call into it
  // (create_descriptor_with_endpoint, create_empty_descriptor,
  // from_descriptor_with_endpoint) holds backend_instance_mutex() for this
  // reason, and so does this call.
  const rosidl_message_type_support_t * descriptor_type_supports;
  try {
    std::lock_guard<std::mutex> backend_lock(
      rmw_cyclonedds_cpp::backend_instance_mutex(backend->get_backend_type()));
    descriptor_type_supports = backend->get_descriptor_type_support();
  } catch (const std::exception & e) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_publication_matched: backend '%s' get_descriptor_type_support threw: %s",
      backend_type.c_str(), e.what());
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }
  const rosidl_message_type_support_t * resolved = get_typesupport(descriptor_type_supports);
  if (resolved == nullptr) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
      "on_publication_matched: backend '%s' descriptor type support not from this RMW: %s",
      backend_type.c_str(), rmw_get_error_string().str);
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "on_publication_matched: %s", rmw_get_error_string().str);
    rmw_reset_error();
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }
  auto message_value_type = rmw_cyclonedds_cpp::make_message_value_type(descriptor_type_supports);
  const std::string type_name = get_message_type_name(resolved);
  auto * private_sertype = create_sertype(type_name, false, std::move(message_value_type));
  create_msg_dds_dynamic_type(
    resolved->typesupport_identifier, resolved->data, dds_get_participant(writer),
    private_sertype);
  dds_entity_t private_topic =
    create_topic(dds_get_participant(writer), private_name.c_str(), private_sertype);
  if (private_topic < 0) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_publication_matched: failed to create private topic '%s'", private_name.c_str());
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }

  dds_qos_t * private_writer_qos = rmw_cyclonedds_cpp::make_private_endpoint_qos();
  dds_entity_t private_writer =
    dds_create_writer(dds_get_parent(writer), private_topic, private_writer_qos, nullptr);
  dds_delete_qos(private_writer_qos);
  dds_delete(private_topic);
  if (private_writer < 0) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_publication_matched: failed to create private writer for '%s'", private_name.c_str());
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }

  std::unique_lock<std::shared_mutex> lock(pub->buffer_discovery.mutex);
  // PrivateEndpoint carries a std::mutex (reader_cache_mutex, see its own
  // comment), which is neither copyable nor movable -- constructing a
  // PrivateEndpoint temporary and handing it to emplace() does not
  // compile, since the map would have to move it into place.
  // piecewise_construct builds it in place instead.
  if (!pub->buffer_discovery.private_writers_or_readers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peer_guid),
      std::forward_as_tuple(private_writer, backend, endpoint_info)).second)
  {
    // Lost a race against another invocation of this same callback.
    dds_delete(private_writer);
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }
  RCUTILS_LOG_INFO_NAMED(
    "rmw_cyclonedds_cpp",
    "on_publication_matched: created private buffer-backend topic '%s' (backend '%s')",
    private_name.c_str(), backend_type.c_str());
}

// build_endpoint_info_from_match()'s "peer's own GID not found in the
// graph cache's per-topic listing" failure is not a rare corner case --
// rmw_dds_common's GraphCache is populated by its OWN discovery topic,
// which propagates asynchronously and commonly lags the raw DDS
// built-in-topic match this callback fires on, especially at process
// startup.
//
// Retrying a few times with sleep_for() right inside
// on_publication_matched/on_subscription_matched would be bounded (80ms
// max), but still blocks CycloneDDS's shared listener-callback thread,
// which delays delivery of every OTHER pending listener event on that
// participant, not just this one peer's discovery. Both callers instead
// try once synchronously (the common case: the graph cache already has
// the peer) and, on failure, defer the retry loop to a detached
// background thread instead of sleeping here -- see their own comments
// for how that thread's lifetime is kept safe.
//
// Fires whenever `writer` matches or unmatches a subscription. On a
// match whose advertised user_data names a non-CPU buffer backend,
// create a private topic + writer for that one peer, named identically
// to what on_subscription_matched computes on the other side, so the
// two converge on the same DDS topic without
// ever exchanging the name out of band. Guarded so a redundant re-fire
// (DDS fires this on unmatch too, and can re-fire on a transient network
// blip) never creates a second private writer for the same peer.
// Forward-declared so on_publication_matched can hand a late-discovered
// (retried) ep to the same processing path a synchronously found one
// uses, without duplicating that path's body.
static void process_matched_publication(
  CddsPublisher * pub, dds_entity_t writer, dds_instance_handle_t peer_handle,
  dds_builtintopic_endpoint_t * ep);

static void on_publication_matched(
  dds_entity_t writer, const dds_publication_matched_status_t status, void * arg)
{
  auto * pub = static_cast<CddsPublisher *>(arg);
  // create_cdds_publisher() registers this callback via
  // dds_lset_publication_matched_arg() AFTER listener_set_event_callbacks()
  // already registered on_publication_matched_fn on the SAME listener for
  // the SAME event -- CycloneDDS's dds_listener_t holds exactly one
  // function pointer per event type, so a plain second registration would
  // silently replace that one instead of composing with it, disabling the
  // standard RMW_EVENT_PUBLICATION_MATCHED callback and unread-count
  // bookkeeping (data->event_callback/event_unread_count) for every
  // has_buffer_fields publisher. The two functions are different code, but
  // dds_lset_publication_matched_arg() replaces by EVENT TYPE, not by
  // function identity, so a second call on the same listener always
  // overwrites the first regardless of which function either call named.
  // Fixed by calling the generic handler explicitly, first and
  // unconditionally -- before any of this function's own early returns --
  // so both behaviors run off the one registered slot instead of one
  // silently losing the other.
  on_publication_matched_fn(writer, status, &pub->user_callback_data);
  const dds_instance_handle_t peer_handle = status.last_subscription_handle;
  dds_builtintopic_endpoint_t * ep = dds_get_matched_subscription_data(writer, peer_handle);
  if (ep != nullptr) {
    process_matched_publication(pub, writer, peer_handle, ep);
    return;
  }
  // A null ep here is ambiguous -- it can mean the peer is already gone
  // (an unmatch raced ahead of this callback), but it can also mean the
  // graph cache simply has not caught up yet with a peer discovered
  // moments ago, which a single synchronous call can lose the race
  // against. Retried on a detached thread, reusing the same
  // begin_async_retry()/end_async_retry()-style lifecycle the retry
  // below uses for a different failure -- this callback itself never
  // blocks, since blocking it here would delay delivery of every OTHER
  // pending listener event on this participant, not just this one
  // peer's (the exact hazard the retry below is already written to
  // avoid).
  if (!rmw_cyclonedds_cpp::begin_null_ep_retry(pub->buffer_discovery)) {
    // Capped out, or shutting down -- treat this null ep as an unmatch,
    // same as if the retry below had found nothing.
    return;
  }
  try {
    std::thread(
      [pub, writer, peer_handle]()
      {
        // Bounded and short: this thread exists only to re-ask a
        // question a plain unmatch answers identically to a late
        // discovery (null), so it is deliberately cheaper than the
        // retry below, which only runs after a non-null ep's endpoint
        // info fails to build. Spawning one of these per ordinary
        // unmatch under topic churn is a real cost, bounded by
        // begin_null_ep_retry()'s own cap rather than avoided by
        // guessing which case a null ep is.
        constexpr int kNullEpRetries = 3;
        constexpr auto kNullEpRetryDelay = std::chrono::milliseconds(2);
        dds_builtintopic_endpoint_t * retry_ep = nullptr;
        for (int attempt = 0; attempt < kNullEpRetries && retry_ep == nullptr; attempt++) {
          std::this_thread::sleep_for(kNullEpRetryDelay);
          retry_ep = dds_get_matched_subscription_data(writer, peer_handle);
        }
        if (retry_ep != nullptr) {
          process_matched_publication(pub, writer, peer_handle, retry_ep);
        }
        // else: still gone -- an unmatch raced ahead of this callback,
        // matching this function's own top-of-function case for the
        // same condition.
        rmw_cyclonedds_cpp::end_null_ep_retry(pub->buffer_discovery);
      }).detach();
  } catch (const std::exception & e) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_publication_matched: failed to start null-ep retry thread: %s, giving up on "
      "this match", e.what());
    rmw_cyclonedds_cpp::end_null_ep_retry(pub->buffer_discovery);
  }
}

static void process_matched_publication(
  CddsPublisher * pub, dds_entity_t writer, dds_instance_handle_t peer_handle,
  dds_builtintopic_endpoint_t * ep)
{
  if (!rmw_cyclonedds_cpp::advertises_non_cpu_backend(ep->qos)) {
    dds_builtintopic_free_endpoint(ep);
    return;
  }

  rmw_cyclonedds_cpp::PeerGuid peer_guid;
  memcpy(peer_guid.data(), ep->key.v, peer_guid.size());

  // This check runs here, as the cheapest possible gate, before the
  // deferred-to-a-background-thread retry logic and the
  // descriptor-resolution work below -- not after them. A redundant
  // re-fire for a peer already privately matched is the normal case this
  // callback's own comment documents, not a rare one, so it must not pay
  // that fuller cost before being discarded.
  {
    std::shared_lock<std::shared_mutex> lock(pub->buffer_discovery.mutex);
    if (pub->buffer_discovery.private_writers_or_readers.count(peer_guid) > 0) {
      dds_builtintopic_free_endpoint(ep);
      return;
    }
  }

  const std::string private_name =
    rmw_cyclonedds_cpp::private_topic_name(pub->base_topic_name, ep->key);
  // A peer may advertise several alternative backends (e.g. "cuda,shm"),
  // and this process may only have a plugin for a LATER one in that
  // list -- trying
  // only the first and giving up abandons a private topic that a shared
  // backend could actually have supported.
  const std::vector<std::string> peer_backend_types =
    rmw_cyclonedds_cpp::all_non_cpu_backend_types(ep->qos);
  // Intersected with what THIS publisher itself advertised (not with the
  // registry's installed plugins), then sorted into one canonical order --
  // see own_non_cpu_backend_types()'s own comment on why this, and not the
  // peer's list order, is what both sides must key off of.
  std::vector<std::string> common_backend_types =
    rosidl_buffer_backend_registry::get_common_backends(
    peer_backend_types, own_non_cpu_backend_types(writer));
  std::sort(common_backend_types.begin(), common_backend_types.end());
  // A peer advertising a backend this side has none in common with (e.g.
  // peer only offers "cuda", this side only has "shm") is a NORMAL case,
  // not an error -- but building endpoint info
  // allocates a full rmw_topic_endpoint_info_t before the (only) place
  // that used to check for this, several lines further down. Checked here
  // instead, before any of that cost is paid.
  if (common_backend_types.empty()) {
    RCUTILS_LOG_INFO_NAMED(
      "rmw_cyclonedds_cpp",
      "on_publication_matched: no backend in common with peer for '%s', staying on the "
      "shared topic", private_name.c_str());
    dds_builtintopic_free_endpoint(ep);
    return;
  }

  // build_endpoint_info_from_match() needs this SAME ep -- read
  // before freeing it, rather than fetching a second copy from DDS below,
  // which would race against an intervening unmatch. Tried once,
  // synchronously: the common case is the graph cache already has the
  // peer, and this keeps that case exactly as fast as the unmatched path.
  rmw_topic_endpoint_info_t endpoint_info = rmw_get_zero_initialized_topic_endpoint_info();
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  const rmw_ret_t info_ret =
    build_endpoint_info_from_match(pub->node, ep, /*peer_is_reader=*/ true, &endpoint_info);
  dds_builtintopic_free_endpoint(ep);

  if (RMW_RET_OK == info_ret) {
    // Symmetric fix to on_subscription_matched's own -- see
    // its comment for the full reasoning and the live trace that
    // confirmed the reentrant-entity-creation mechanism. Calling
    // finish_publication_match() synchronously here has the identical
    // hazard for a same-process, same-participant match: its own nested
    // dds_create_writer() (for the private topic) can be invoked
    // reentrant on a thread still inside this process's own
    // dds_create_writer()/dds_create_reader() call. Deferred the same way.
    if (!rmw_cyclonedds_cpp::begin_async_retry(pub->buffer_discovery)) {
      rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
      return;
    }
    try {
      std::thread(
        [pub, writer, peer_guid, private_name, common_backend_types, endpoint_info]()
        {
          finish_publication_match(
            pub, writer, peer_guid, private_name, common_backend_types, endpoint_info);
          rmw_cyclonedds_cpp::end_async_retry(pub->buffer_discovery);
        }).detach();
    } catch (const std::exception & e) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "on_publication_matched: failed to start deferred-match thread for '%s': %s, "
        "giving up on this match", private_name.c_str(), e.what());
      rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
      rmw_cyclonedds_cpp::end_async_retry(pub->buffer_discovery);
    }
    return;
  }

  // The graph cache had not caught up yet on the first attempt.
  // Retrying with sleep_for() right here would block CycloneDDS's shared
  // listener-callback thread and delay every OTHER pending listener event
  // on this participant, not just this one peer's discovery. Deferred to
  // a detached background thread instead: `writer`/`peer_handle` are
  // stable value types (a DDS
  // handle and an instance handle, not pointers into `ep`, which is
  // already freed above), so the thread re-fetches its OWN fresh `ep` each
  // attempt rather than reaching past this function's freeing of the
  // original one. `pub` is captured as a raw pointer with no other
  // lifetime guard -- begin_async_retry()/end_async_retry() bracket this
  // thread's lifetime so destroy_publisher() can wait for it (via
  // wait_for_async_retries()) before freeing `pub` or deleting `writer`.
  rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
  rmw_reset_error();
  // begin_async_retry() refuses (returns false) once destroy_publisher()
  // has called begin_shutdown() -- see BufferEndpointDiscovery.hpp. Not
  // spawning at all is correct here: the publisher is on its way out, so
  // a delayed private topic for it would never be used anyway.
  if (!rmw_cyclonedds_cpp::begin_async_retry(pub->buffer_discovery)) {
    return;
  }
  // std::thread's constructor can throw std::system_error (e.g. the OS
  // refuses to start a new thread
  // under load or during a mass teardown). Uncaught, that exception would
  // propagate out of this CycloneDDS C listener callback (UB at that ABI
  // boundary) AND leave pending_async_retries incremented forever with no
  // matching end_async_retry() -- every future destroy_publisher() on this
  // publisher would then hang in wait_for_async_retries(). Caught here:
  // undo the increment and give up on this one retry, exactly like the
  // "peer unmatched" and "still failing after kMaxAttempts" cases below
  // already do.
  try {
    std::thread(
      [pub, writer, peer_handle, peer_guid, private_name, common_backend_types]()
      {
        constexpr int kMaxAttempts = 4;
        constexpr auto kRetryDelay = std::chrono::milliseconds(20);
        rcutils_allocator_t retry_allocator = rcutils_get_default_allocator();
        rmw_topic_endpoint_info_t retry_info = rmw_get_zero_initialized_topic_endpoint_info();
        rmw_ret_t ret = RMW_RET_ERROR;
        for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
          std::this_thread::sleep_for(kRetryDelay);
          dds_builtintopic_endpoint_t * retry_ep =
          dds_get_matched_subscription_data(writer, peer_handle);
          if (retry_ep == nullptr) {
            // The peer unmatched while this thread was sleeping -- give up
            // quietly, matching on_publication_matched's own top-of-function
            // check for the same condition.
            ret = RMW_RET_ERROR;
            break;
          }
          ret = build_endpoint_info_from_match(
            pub->node, retry_ep, /*peer_is_reader=*/ true, &retry_info);
          dds_builtintopic_free_endpoint(retry_ep);
          if (RMW_RET_OK == ret) {
            break;
          }
          rmw_topic_endpoint_info_fini(&retry_info, &retry_allocator);
          retry_info = rmw_get_zero_initialized_topic_endpoint_info();
          rmw_reset_error();
        }
        if (RMW_RET_OK == ret) {
          finish_publication_match(
            pub, writer, peer_guid, private_name, common_backend_types, retry_info);
        } else {
          RCUTILS_LOG_ERROR_NAMED(
            "rmw_cyclonedds_cpp",
            "on_publication_matched: failed to build endpoint info for '%s' after "
            "backgrounded retries", private_name.c_str());
          rmw_topic_endpoint_info_fini(&retry_info, &retry_allocator);
        }
        rmw_cyclonedds_cpp::end_async_retry(pub->buffer_discovery);
      }).detach();
  } catch (const std::exception & e) {
    // Catching std::exception, not just std::system_error, matters here:
    // constructing std::thread also decay-copies the lambda's captures
    // (including common_backend_types, a std::vector<std::string>) on
    // THIS thread before the new one starts -- if that copy throws
    // std::bad_alloc or any other std::exception that is not a
    // system_error, catching only system_error would leave it uncaught,
    // propagating out of this CycloneDDS listener callback (ABI-boundary
    // UB) and leaking pending_async_retries forever.
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_publication_matched: failed to start background retry thread for '%s': %s, "
      "giving up on this match", private_name.c_str(), e.what());
    rmw_cyclonedds_cpp::end_async_retry(pub->buffer_discovery);
  }
}

static CddsPublisher * create_cdds_publisher(
  const rmw_node_t * node, dds_entity_t dds_ppant, dds_entity_t dds_pub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name,
  const rmw_qos_profile_t * qos_policies)
{
  if (!topic_name || topic_name[0] == '\0') {
    RMW_SET_ERROR_MSG("topic_name is null or empty string");
    return nullptr;
  }

  RET_NULL_X(qos_policies, return nullptr);
  const rosidl_message_type_support_t * type_support = get_typesupport(type_supports);
  RET_NULL_X(type_support, return nullptr);
  const std::string type_name = get_message_type_name(type_support);
  if (type_name.empty()) {
    return nullptr;
  }
  CddsPublisher * pub = new CddsPublisher();
  dds_entity_t topic;
  dds_qos_t * qos;

  std::string fqtopic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", qos_policies);
  auto message_type_support = rmw_cyclonedds_cpp::make_message_value_type(type_supports);
  const bool is_self_contained = message_type_support->is_self_contained();
  const size_t sample_size = message_type_support->sizeof_type();
  // Computed before the std::move below, which invalidates
  // message_type_support.
  const bool has_buffer_fields =
    rmw_cyclonedds_cpp::has_buffer_backed_fields(message_type_support.get());

  auto sertype = create_sertype(
    type_name,
    false,
    std::move(message_type_support));
  create_msg_dds_dynamic_type(
    type_support->typesupport_identifier, type_support->data, dds_ppant,
    sertype);
  struct ddsi_sertype * stact = nullptr;
  topic = create_topic(dds_ppant, fqtopic_name.c_str(), sertype, &stact);
  // Set before dds_create_writer below so on_publication_matched --
  // which can fire as soon as the writer exists -- never reads these
  // unset. pub->pubiid/gid, in contrast, genuinely cannot be known until
  // after dds_create_writer returns, which is why they stay assigned below.
  pub->base_topic_name = fqtopic_name;
  pub->node = node;
  pub->has_buffer_fields = has_buffer_fields;
  pub->sertype = stact;

  dds_listener_t * listener = dds_create_listener(&pub->user_callback_data);
  // Set the corresponding callbacks to listen for events
  listener_set_event_callbacks(listener, &pub->user_callback_data);
  if (has_buffer_fields) {
    dds_lset_publication_matched_arg(listener, on_publication_matched, pub, false);
  }

  if (topic < 0) {
    set_error_message_from_create_topic(topic, fqtopic_name);
    goto fail_topic;
  }
  qos = create_readwrite_qos(
    qos_policies, *type_support->get_type_hash_func(type_support), false, "",
    has_buffer_fields);
  if (qos == nullptr) {
    goto fail_qos;
  }
  pub->enth = dds_create_writer(dds_pub, topic, qos, listener);
  if (pub->enth < 0) {
    RMW_SET_ERROR_MSG("failed to create writer");
    goto fail_writer;
  }
  if (dds_get_instance_handle(pub->enth, &pub->pubiid) < 0) {
    RMW_SET_ERROR_MSG("failed to get instance handle for writer");
    goto fail_instance_handle;
  }

  get_entity_gid(pub->enth, pub->gid);
  dds_delete_listener(listener);
  pub->type_supports = *type_supports;
  // This rebuilds the identical introspection tree
  // make_message_value_type() already built above (moved into
  // create_sertype()) -- a real, avoidable second walk. Not fixed here:
  // create_sertype() takes ownership of its tree via
  // std::unique_ptr<StructValueType>, so the SAME instance cannot also be
  // owned by pub->buffer_message_value_type -- closing this for real would
  // mean changing create_sertype()'s (and sertype_rmw's own) ownership
  // model to std::shared_ptr across all of its call sites in this file, a
  // materially bigger change than this feature's own scope. Paid once per
  // publisher/subscription creation, never on the per-message publish/take
  // hot path the caching elsewhere in this file is about.
  if (has_buffer_fields) {
    pub->buffer_message_value_type =
      rmw_cyclonedds_cpp::make_message_value_type(&pub->type_supports);
  }
#if CDDS_VERSION == CDDS_VERSION_0_10
  pub->is_loaning_available = is_self_contained && dds_is_loan_available(pub->enth);
#else
  // Some form of loaning is always possible, but the exact behaviour depends on type and
  // whether or not Iceoryx can be used.  I'm not sure to what the expectations are
  // exactly, this should keep it essentially unchanged from the behaviour in Humble and
  // Iron.
  pub->is_loaning_available = is_self_contained && dds_is_shared_memory_available(pub->enth);
#endif
  pub->sample_size = sample_size;
  dds_delete_qos(qos);
  dds_delete(topic);

  return pub;

fail_instance_handle:
  // dds_create_writer() above already made pub->enth and its
  // on_publication_matched listener live, so a concurrent match in this
  // window can spawn a detached async-retry thread capturing raw
  // `pub`/`pub->enth` before this label ever runs -- the identical
  // use-after-free class create_publisher()'s own scope_exit closes for
  // its own failure path, needed again here because this is a different,
  // deeper failure path inside create_cdds_publisher() itself.
  rmw_cyclonedds_cpp::begin_shutdown(pub->buffer_discovery);
  rmw_cyclonedds_cpp::wait_for_async_retries(pub->buffer_discovery);
  if (dds_delete(pub->enth) < 0) {
    RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to destroy writer during error handling");
  }
  // A SYNCHRONOUS match completed before this failure path ran (the
  // window the comment above already names) can have inserted a private
  // writer into this map. The shutdown/wait/delete sequence above stops
  // and drains ASYNC retries but does not touch the map itself, so it
  // must be drained separately here -- leaking that writer and its
  // endpoint_info the same way an unmatched destroy_publisher() would
  // otherwise.
  rmw_cyclonedds_cpp::drain_private_endpoints(pub->buffer_discovery);
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
  const rmw_node_t * node, dds_entity_t dds_ppant, dds_entity_t dds_pub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_publisher_options_t * publisher_options
)
{
  CddsPublisher * pub;
  if ((pub =
    create_cdds_publisher(
      node, dds_ppant, dds_pub, type_supports, topic_name, qos_policies)) == nullptr)
  {
    return nullptr;
  }
  auto cleanup_cdds_publisher = rcpputils::make_scope_exit(
    [pub]() {
      // on_publication_matched can fire (and spawn a detached async-retry
      // thread capturing `pub`/`pub->enth`) the instant
      // create_cdds_publisher() returns, before this scope_exit ever
      // runs -- the same race destroy_publisher()'s
      // begin_shutdown()/wait_for_async_retries()/dds_delete() ordering
      // exists to close. This construction-failure path must use that
      // same pair, or `pub` is deleted into the identical
      // use-after-free window.
      rmw_cyclonedds_cpp::begin_shutdown(pub->buffer_discovery);
      rmw_cyclonedds_cpp::wait_for_async_retries(pub->buffer_discovery);
      if (dds_delete(pub->enth) < 0) {
        RCUTILS_LOG_ERROR_NAMED(
          "rmw_cyclonedds_cpp", "failed to delete writer during error handling");
      }
      // Mirrors fail_instance_handle's own drain inside
      // create_cdds_publisher -- a synchronous match completed after
      // create_cdds_publisher() returned but before this scope_exit runs
      // can have inserted a private writer into this map, and the
      // sequence above does not drain it on its own.
      rmw_cyclonedds_cpp::drain_private_endpoints(pub->buffer_discovery);
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
  // Adapt any 'best available' QoS options
  rmw_qos_profile_t adapted_qos_policies = *qos_policies;
  rmw_ret_t ret = rmw_dds_common::qos_profile_get_best_available_for_topic_publisher(
    node, topic_name, &adapted_qos_policies, rmw_get_subscriptions_info_by_topic);
  if (RMW_RET_OK != ret) {
    return nullptr;
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
    node, node->context->impl->ppant, node->context->impl->dds_pub,
    type_supports, topic_name, &adapted_qos_policies,
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
  if (RMW_RET_OK != common->add_publisher_graph(
      cddspub->gid,
      node->name, node->namespace_))
  {
    return nullptr;
  }

  cleanup_publisher.cancel();
  TRACETOOLS_TRACEPOINT(rmw_publisher_init, static_cast<const void *>(pub), cddspub->gid.data);
  return pub;
}

extern "C" rmw_ret_t rmw_get_gid_for_publisher(const rmw_publisher_t * publisher, rmw_gid_t * gid)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);
  auto pub = static_cast<const CddsPublisher *>(publisher->data);
  gid->implementation_identifier = eclipse_cyclonedds_identifier;
  memset(gid->data, 0, sizeof(gid->data));
  static_assert(
    sizeof(pub->pubiid) <= sizeof(gid->data),
    "publisher id is larger than max rmw gid size");
  memcpy(gid->data, &pub->pubiid, sizeof(pub->pubiid));
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_get_gid_for_client(const rmw_client_t * client, rmw_gid_t * gid)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client, client->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid, RMW_RET_INVALID_ARGUMENT);

  const CddsClient * cli = static_cast<const CddsClient *>(client->data);
  gid->implementation_identifier = eclipse_cyclonedds_identifier;
  memset(gid->data, 0, sizeof(gid->data));
  static_assert(
    sizeof(cli->client.id.data) <= sizeof(gid->data),
    "client id is larger than max rmw gid size");
  memcpy(gid->data, cli->client.id.data, sizeof(cli->client.id.data));
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_compare_gids_equal(
  const rmw_gid_t * gid1, const rmw_gid_t * gid2,
  bool * result)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(gid1, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    gid1, gid1->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(gid2, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    gid2, gid2->implementation_identifier, eclipse_cyclonedds_identifier,
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
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription_count, RMW_RET_INVALID_ARGUMENT);

  auto pub = static_cast<CddsPublisher *>(publisher->data);
  const dds_return_t count = dds_get_matched_subscriptions(pub->enth, nullptr, 0);
  if (count < 0) {
    return RMW_RET_ERROR;
  }

  *subscription_count = static_cast<size_t>(count);
  return RMW_RET_OK;
}

rmw_ret_t rmw_publisher_assert_liveliness(const rmw_publisher_t * publisher)
{
  RET_NULL(publisher);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
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
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
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
  }
  return RMW_RET_ERROR;
}

rmw_ret_t rmw_publisher_get_actual_qos(const rmw_publisher_t * publisher, rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(qos, RMW_RET_INVALID_ARGUMENT);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (get_readwrite_qos(pub->enth, qos)) {
    return RMW_RET_OK;
  }
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_borrow_loaned_message(
  const rmw_publisher_t * publisher,
  const rosidl_message_type_support_t * type_support,
  void ** ros_message)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  if (!publisher->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(type_support, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
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
#if CDDS_VERSION > CDDS_VERSION_0_10 || defined DDS_HAS_SHM
    auto sample_ptr = init_and_alloc_sample(cdds_publisher, cdds_publisher->sample_size);
    RET_NULL_X(sample_ptr, return RMW_RET_ERROR);
    *ros_message = sample_ptr;
    return RMW_RET_OK;
#else
    RMW_SET_ERROR_MSG("rmw_borrow_loaned_message not implemented for rmw_cyclonedds_cpp");
    return RMW_RET_UNSUPPORTED;
#endif
  } else {
    RMW_SET_ERROR_MSG("Borrowing loan for a non fixed type is not allowed");
    return RMW_RET_ERROR;
  }
}

extern "C" rmw_ret_t rmw_return_loaned_message_from_publisher(
  const rmw_publisher_t * publisher,
  void * loaned_message)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(publisher, RMW_RET_INVALID_ARGUMENT);
  if (!publisher->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(loaned_message, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  auto cdds_publisher = static_cast<CddsPublisher *>(publisher->data);
  if (!cdds_publisher) {
    RMW_SET_ERROR_MSG("publisher data is null");
    return RMW_RET_ERROR;
  }

  // if the publisher can loan
  if (cdds_publisher->is_loaning_available) {
#if CDDS_VERSION > CDDS_VERSION_0_10 || defined DDS_HAS_SHM
    return fini_and_free_sample(cdds_publisher, loaned_message);
#else
    RMW_SET_ERROR_MSG(
      "rmw_return_loaned_message_from_publisher not implemented for rmw_cyclonedds_cpp");
    return RMW_RET_UNSUPPORTED;
#endif
  } else {
    RMW_SET_ERROR_MSG("returning loan for a non fixed type is not allowed");
    return RMW_RET_ERROR;
  }
}

static rmw_ret_t destroy_publisher(rmw_publisher_t * publisher)
{
  rmw_ret_t ret = RMW_RET_OK;
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (pub != nullptr) {
    // dds_delete(pub->enth) THEN wait_for_async_retries() would still
    // leave a race open. dds_delete's own
    // "blocks until any invocation already executing has returned"
    // guarantee only covers the SYNCHRONOUS listener callback -- it says
    // nothing about a std::thread that a PRIOR match event's callback
    // already detached and returned from. That thread, still sleeping
    // inside its retry loop, holds `writer` (== pub->enth) and calls
    // dds_get_matched_subscription_data(writer, ...)/dds_get_parent(writer)
    // regardless of whether dds_delete(pub->enth) has already run.
    //
    // Fixed by reordering: begin_shutdown() (stops any NEW retry thread
    // from spawning, including one a currently-executing synchronous
    // callback might be about to start) THEN wait_for_async_retries()
    // (blocks until every retry thread already spawned -- from this match
    // or an earlier one -- has fully finished using `writer`) THEN
    // dds_delete(pub->enth). By the time the entity is actually deleted,
    // no thread, spawned or synchronous, can still be touching it: the
    // flag stops future ones, the wait drains existing ones, and
    // dds_delete's own blocking-until-callback-returns guarantee covers
    // whatever synchronous invocation might be running at this exact
    // instant (it will see the flag and refuse to spawn before returning).
    rmw_cyclonedds_cpp::begin_shutdown(pub->buffer_discovery);
    rmw_cyclonedds_cpp::wait_for_async_retries(pub->buffer_discovery);
    if (dds_delete(pub->enth) < 0) {
      RMW_SET_ERROR_MSG("failed to delete writer");
      ret = RMW_RET_ERROR;
    }
    // Private per-peer writers created by on_publication_matched()
    // are parented under the same publisher as pub->enth (dds_get_parent(
    // writer) in that callback), so deleting pub->enth alone leaves them
    // as orphan entities that outlive this publisher.
    //
    // rmw_publish() takes a shared_lock to read this map -- draining it
    // (a mutation) must take the exclusive lock, or a concurrent publish's
    // dereference of a
    // PrivateEndpoint* into it races this drain's dds_delete()/fini().
    // drain_private_endpoints() takes that lock itself.
    rmw_cyclonedds_cpp::drain_private_endpoints(pub->buffer_discovery);
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  rmw_ret_t ret = RMW_RET_OK;
  rmw_error_state_t error_state;
  auto common = &node->context->impl->common;
  const auto cddspub = static_cast<const CddsPublisher *>(publisher->data);
  rmw_ret_t publish_ret = common->remove_publisher_graph(
    cddspub->gid,
    node->name, node->namespace_);
  if (RMW_RET_OK != publish_ret) {
    error_state = *rmw_get_error_state();
    ret = publish_ret;
    rmw_reset_error();
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

// Mirrors finish_publication_match's own factoring-out, for the same
// reason -- shared by on_subscription_matched's synchronous fast path and
// its deferred retry thread below.
static void finish_subscription_match(
  CddsSubscription * sub, dds_entity_t reader, const rmw_cyclonedds_cpp::PeerGuid & peer_guid,
  const std::string & private_name, const std::vector<std::string> & common_backend_types,
  rmw_topic_endpoint_info_t endpoint_info)
{
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  {
    std::shared_lock<std::shared_mutex> lock(sub->buffer_discovery.mutex);
    if (sub->buffer_discovery.private_writers_or_readers.count(peer_guid) > 0) {
      rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
      return;
    }
  }

  // Same process-wide registry as on_publication_matched -- see its own
  // comment on why this replaced a per-entity one.
  std::shared_ptr<rosidl::BufferBackend> backend;
  std::string backend_type;
  // See on_publication_matched's own comment: removed as dead code, then
  // restored, then moved inside the loop -- a whole-loop try/catch
  // abandoned every remaining candidate when a non-last one threw,
  // instead of trying the next one the way a plain null return would.
  {
    std::lock_guard<std::mutex> lock(rmw_cyclonedds_cpp::process_backend_registry_mutex());
    for (const auto & candidate : common_backend_types) {
      try {
        backend = rosidl_buffer_backend_registry::find_backend_by_type(
          rmw_cyclonedds_cpp::process_backend_registry(), candidate);
      } catch (const std::exception & e) {
        RCUTILS_LOG_ERROR_NAMED(
          "rmw_cyclonedds_cpp",
          "on_subscription_matched: find_backend_by_type threw for candidate '%s': %s, "
          "trying next candidate", candidate.c_str(), e.what());
        backend.reset();
        continue;
      }
      if (backend) {
        backend_type = candidate;
        break;
      }
    }
  }
  if (!backend) {
    std::string tried;
    for (const auto & candidate : common_backend_types) {
      tried += (tried.empty() ? "" : ",") + candidate;
    }
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_subscription_matched: no buffer backend plugin registered for any of the common "
      "set '%s', skipping private topic '%s'", tried.c_str(), private_name.c_str());
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }

  // Matches on_publication_matched's own reasoning exactly: the descriptor
  // type is an ordinary ROS message type, so this reader needs no marker
  // sertype of its own. See that function's own comment for why the call
  // below is guarded, and its round-4 comment for why it also needs
  // backend_instance_mutex().
  const rosidl_message_type_support_t * descriptor_type_supports;
  try {
    std::lock_guard<std::mutex> backend_lock(
      rmw_cyclonedds_cpp::backend_instance_mutex(backend->get_backend_type()));
    descriptor_type_supports = backend->get_descriptor_type_support();
  } catch (const std::exception & e) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_subscription_matched: backend '%s' get_descriptor_type_support threw: %s",
      backend_type.c_str(), e.what());
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }
  const rosidl_message_type_support_t * resolved = get_typesupport(descriptor_type_supports);
  if (resolved == nullptr) {
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
      "on_subscription_matched: backend '%s' descriptor type support not from this RMW: %s",
      backend_type.c_str(), rmw_get_error_string().str);
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "on_subscription_matched: %s", rmw_get_error_string().str);
    rmw_reset_error();
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }
  auto message_value_type = rmw_cyclonedds_cpp::make_message_value_type(descriptor_type_supports);
  const std::string type_name = get_message_type_name(resolved);
  auto * private_sertype = create_sertype(type_name, false, std::move(message_value_type));
  create_msg_dds_dynamic_type(
    resolved->typesupport_identifier, resolved->data, dds_get_participant(reader),
    private_sertype);
  dds_entity_t private_topic =
    create_topic(dds_get_participant(reader), private_name.c_str(), private_sertype);
  if (private_topic < 0) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_subscription_matched: failed to create private topic '%s'", private_name.c_str());
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }

  dds_qos_t * private_reader_qos = rmw_cyclonedds_cpp::make_private_endpoint_qos();
  dds_entity_t private_reader =
    dds_create_reader(dds_get_parent(reader), private_topic, private_reader_qos, nullptr);
  dds_delete_qos(private_reader_qos);
  dds_delete(private_topic);
  if (private_reader < 0) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_subscription_matched: failed to create private reader for '%s'", private_name.c_str());
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }

  // from_descriptor_with_endpoint() is still what consumes samples from this reader
  // (from_descriptor_with_endpoint() installing a real backend-aware
  // buffer) -- this PR only makes the two sides agree on the same wire
  // type, which `backend` and `endpoint_info` are stored for.
  std::unique_lock<std::shared_mutex> lock(sub->buffer_discovery.mutex);
  // Mirrors on_publication_matched's own construction: PrivateEndpoint's
  // reader_cache_mutex is not copyable or movable, so it must be
  // constructed in place via piecewise_construct.
  if (!sub->buffer_discovery.private_writers_or_readers.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(peer_guid),
      std::forward_as_tuple(private_reader, backend, endpoint_info)).second)
  {
    dds_delete(private_reader);
    rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
    return;
  }
  RCUTILS_LOG_INFO_NAMED(
    "rmw_cyclonedds_cpp",
    "on_subscription_matched: created private buffer-backend topic '%s' (backend '%s')",
    private_name.c_str(), backend_type.c_str());
}

// Mirrors on_publication_matched above. private_topic_name() is the same
// free function both sides call, on the
// same (base_topic_name, peer_guid) pair -- computed from `reader`'s own
// GID here, from the matched publication's GID on the publisher side --
// which is what makes the two sides converge on one DDS topic name without
// exchanging it out of band.
// Forward-declared, mirroring process_matched_publication -- lets
// on_subscription_matched hand a late-discovered (retried) ep to the
// same processing path a synchronously found one uses.
static void process_matched_subscription(
  CddsSubscription * sub, dds_entity_t reader, dds_instance_handle_t peer_handle,
  dds_builtintopic_endpoint_t * ep);

static void on_subscription_matched(
  dds_entity_t reader, const dds_subscription_matched_status_t status, void * arg)
{
  auto * sub = static_cast<CddsSubscription *>(arg);
  // Mirrors on_publication_matched's own fix, same round -- see its comment
  // for why the generic handler must be called explicitly here rather than
  // relying on listener_set_event_callbacks()'s own registration, which
  // this function's own dds_lset_subscription_matched_arg() call silently
  // replaced on the same listener.
  on_subscription_matched_fn(reader, status, &sub->user_callback_data);
  const dds_instance_handle_t peer_handle = status.last_publication_handle;
  dds_builtintopic_endpoint_t * ep = dds_get_matched_publication_data(reader, peer_handle);
  if (ep != nullptr) {
    process_matched_subscription(sub, reader, peer_handle, ep);
    return;
  }
  // Mirrors on_publication_matched's own fix -- a null ep here is
  // ambiguous between a genuine unmatch and the graph cache not having
  // caught up yet with a just-discovered peer. Retried on a detached
  // thread rather than blocking this callback (see
  // on_publication_matched's own comment for the full reasoning).
  if (!rmw_cyclonedds_cpp::begin_null_ep_retry(sub->buffer_discovery)) {
    return;
  }
  try {
    std::thread(
      [sub, reader, peer_handle]()
      {
        // Mirrors on_publication_matched's own null-ep retry thread --
        // bounded and short.
        constexpr int kNullEpRetries = 3;
        constexpr auto kNullEpRetryDelay = std::chrono::milliseconds(2);
        dds_builtintopic_endpoint_t * retry_ep = nullptr;
        for (int attempt = 0; attempt < kNullEpRetries && retry_ep == nullptr; attempt++) {
          std::this_thread::sleep_for(kNullEpRetryDelay);
          retry_ep = dds_get_matched_publication_data(reader, peer_handle);
        }
        if (retry_ep != nullptr) {
          process_matched_subscription(sub, reader, peer_handle, retry_ep);
        }
        rmw_cyclonedds_cpp::end_null_ep_retry(sub->buffer_discovery);
      }).detach();
  } catch (const std::exception & e) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_subscription_matched: failed to start null-ep retry thread: %s, giving up on "
      "this match", e.what());
    rmw_cyclonedds_cpp::end_null_ep_retry(sub->buffer_discovery);
  }
}

static void process_matched_subscription(
  CddsSubscription * sub, dds_entity_t reader, dds_instance_handle_t peer_handle,
  dds_builtintopic_endpoint_t * ep)
{
  if (!rmw_cyclonedds_cpp::advertises_non_cpu_backend(ep->qos)) {
    dds_builtintopic_free_endpoint(ep);
    return;
  }

  rmw_cyclonedds_cpp::PeerGuid peer_guid;
  memcpy(peer_guid.data(), ep->key.v, peer_guid.size());

  // Mirrors on_publication_matched's own early duplicate-check: the
  // cheapest possible gate, before any of the expensive backend-resolution
  // or retry-wrapped work below runs.
  {
    std::shared_lock<std::shared_mutex> lock(sub->buffer_discovery.mutex);
    if (sub->buffer_discovery.private_writers_or_readers.count(peer_guid) > 0) {
      dds_builtintopic_free_endpoint(ep);
      return;
    }
  }

  // This used to pass ep->key here -- for
  // on_subscription_matched, `ep` comes from dds_get_matched_publication_
  // data(reader, ...), so ep->key is the REMOTE WRITER's own GUID, not
  // this reader's. on_publication_matched (below) independently computes
  // the SAME name from ITS ep->key, which for dds_get_matched_subscription_
  // data(writer, ...) is the REMOTE READER's GUID -- i.e. THIS reader's
  // own GUID, from the writer's point of view. The two sides therefore
  // converged on private_topic_name(base, <this reader's GUID>) only if
  // THIS side also keys on its own reader's GUID, not on ep->key. Using
  // ep->key here computed private_topic_name(base, <the writer's GUID>)
  // instead -- a DIFFERENT topic than the one the writer actually
  // creates -- so the private reader and private writer never matched.
  // Measured live: both sides logged "created private buffer-backend
  // topic" with DIFFERENT hex suffixes for the same peer pair, and the
  // subscriber never observed backend_type() != "cpu" on a single sample
  // despite negotiation completing successfully on both sides.
  dds_guid_t self_guid;
  dds_get_guid(reader, &self_guid);
  const std::string private_name =
    rmw_cyclonedds_cpp::private_topic_name(sub->base_topic_name, self_guid);
  // on_publication_matched switches to the resolved backend's
  // descriptor-type sertype, and this side must build the private reader
  // from the SAME descriptor type, not the original message type --
  // otherwise the two sides agree on the topic name but not on the type,
  // which is a genuine "inconsistent topic" case. Mirroring the same
  // backend resolution here fixes the type mismatch. CONSUMING what
  // arrives on this reader
  // (from_descriptor_with_endpoint() installing a real buffer) is a
  // separate concern owned elsewhere -- nothing reads from private_reader
  // yet, so this only makes discovery agree.
  const std::vector<std::string> peer_backend_types =
    rmw_cyclonedds_cpp::all_non_cpu_backend_types(ep->qos);
  // Same canonical-common-set fix as on_publication_matched: intersect
  // with what THIS subscriber itself advertised and sort, so both sides
  // converge on the identical backend rather than each independently
  // trying the peer's list order against their own registry.
  std::vector<std::string> common_backend_types =
    rosidl_buffer_backend_registry::get_common_backends(
    peer_backend_types, own_non_cpu_backend_types(reader));
  std::sort(common_backend_types.begin(), common_backend_types.end());
  // Mirrors on_publication_matched's own early check: no common backend is
  // a normal case, not worth the endpoint-info build's cost.
  if (common_backend_types.empty()) {
    RCUTILS_LOG_INFO_NAMED(
      "rmw_cyclonedds_cpp",
      "on_subscription_matched: no backend in common with peer for '%s', staying on the "
      "shared topic", private_name.c_str());
    dds_builtintopic_free_endpoint(ep);
    return;
  }

  // Tried once, synchronously -- mirrors on_publication_matched's own
  // reasoning: the common case is the graph cache already has the peer.
  rmw_topic_endpoint_info_t endpoint_info = rmw_get_zero_initialized_topic_endpoint_info();
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  const rmw_ret_t info_ret =
    build_endpoint_info_from_match(sub->node, ep, /*peer_is_reader=*/ false, &endpoint_info);
  dds_builtintopic_free_endpoint(ep);

  if (RMW_RET_OK == info_ret) {
    // Calling finish_subscription_match() synchronously here
    // deadlocks for a SAME-PROCESS, SAME-PARTICIPANT match -- confirmed
    // live via instrumented tracing (not a hypothesis): CycloneDDS
    // dispatches this listener callback reentrant, on the same thread,
    // from INSIDE the outer dds_create_reader() call that is still
    // creating `reader` itself, and finish_subscription_match()'s own
    // nested dds_create_reader() (for the private topic) then tries to
    // re-enter CycloneDDS's internal entity-creation lock on the thread
    // that already holds it -- a lock this RMW does not own and cannot
    // make recursive. For a REMOTE peer this callback fires from
    // CycloneDDS's own separate discovery thread, where nested entity
    // creation is safe; the hazard is specific to a local match
    // discovered synchronously during this process's own entity
    // creation. Deferring unconditionally onto the same detached-thread
    // mechanism the retry path below already uses for a slower endpoint-
    // info lookup sidesteps this for every case, not just the retry one:
    // the private topic is then always created on a thread that is never
    // inside an in-progress dds_create_reader()/dds_create_writer() call.
    if (!rmw_cyclonedds_cpp::begin_async_retry(sub->buffer_discovery)) {
      rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
      return;
    }
    try {
      std::thread(
        [sub, reader, peer_guid, private_name, common_backend_types, endpoint_info]()
        {
          finish_subscription_match(
            sub, reader, peer_guid, private_name, common_backend_types, endpoint_info);
          rmw_cyclonedds_cpp::end_async_retry(sub->buffer_discovery);
        }).detach();
    } catch (const std::exception & e) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "on_subscription_matched: failed to start deferred-match thread for '%s': %s, "
        "giving up on this match", private_name.c_str(), e.what());
      rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
      rmw_cyclonedds_cpp::end_async_retry(sub->buffer_discovery);
    }
    return;
  }

  // Mirrors on_publication_matched's own deferred-retry mechanism -- see
  // its comment for the full reasoning.
  rmw_topic_endpoint_info_fini(&endpoint_info, &allocator);
  rmw_reset_error();
  // Mirrors on_publication_matched's own guard -- begin_async_retry()
  // refuses once destroy_subscription() has called begin_shutdown().
  if (!rmw_cyclonedds_cpp::begin_async_retry(sub->buffer_discovery)) {
    return;
  }
  // Mirrors on_publication_matched's own guard -- std::thread's
  // constructor can throw std::system_error; uncaught, that would
  // propagate out of this CycloneDDS C listener callback and leave
  // pending_async_retries incremented forever, hanging every future
  // destroy_subscription() on this subscription.
  try {
    std::thread(
      [sub, reader, peer_handle, peer_guid, private_name, common_backend_types]()
      {
        constexpr int kMaxAttempts = 4;
        constexpr auto kRetryDelay = std::chrono::milliseconds(20);
        rcutils_allocator_t retry_allocator = rcutils_get_default_allocator();
        rmw_topic_endpoint_info_t retry_info = rmw_get_zero_initialized_topic_endpoint_info();
        rmw_ret_t ret = RMW_RET_ERROR;
        for (int attempt = 0; attempt < kMaxAttempts; attempt++) {
          std::this_thread::sleep_for(kRetryDelay);
          dds_builtintopic_endpoint_t * retry_ep =
          dds_get_matched_publication_data(reader, peer_handle);
          if (retry_ep == nullptr) {
            ret = RMW_RET_ERROR;
            break;
          }
          ret = build_endpoint_info_from_match(
            sub->node, retry_ep, /*peer_is_reader=*/ false, &retry_info);
          dds_builtintopic_free_endpoint(retry_ep);
          if (RMW_RET_OK == ret) {
            break;
          }
          rmw_topic_endpoint_info_fini(&retry_info, &retry_allocator);
          retry_info = rmw_get_zero_initialized_topic_endpoint_info();
          rmw_reset_error();
        }
        if (RMW_RET_OK == ret) {
          finish_subscription_match(
            sub, reader, peer_guid, private_name, common_backend_types, retry_info);
        } else {
          RCUTILS_LOG_ERROR_NAMED(
            "rmw_cyclonedds_cpp",
            "on_subscription_matched: failed to build endpoint info for '%s' after "
            "backgrounded retries", private_name.c_str());
          rmw_topic_endpoint_info_fini(&retry_info, &retry_allocator);
        }
        rmw_cyclonedds_cpp::end_async_retry(sub->buffer_discovery);
      }).detach();
  } catch (const std::exception & e) {
    // See on_publication_matched's own comment: catching only
    // std::system_error missed that constructing std::thread also
    // decay-copies the lambda's captures, which can throw a plain
    // std::exception (e.g. std::bad_alloc), not necessarily a
    // system_error.
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "on_subscription_matched: failed to start background retry thread for '%s': %s, "
      "giving up on this match", private_name.c_str(), e.what());
    rmw_cyclonedds_cpp::end_async_retry(sub->buffer_discovery);
  }
}

static CddsSubscription * create_cdds_subscription(
  const rmw_node_t * node, dds_entity_t dds_ppant, dds_entity_t dds_sub,
  const rosidl_message_type_support_t * type_supports, const char * topic_name,
  const rmw_qos_profile_t * qos_policies, bool ignore_local_publications)
{
  if (!topic_name || topic_name[0] == '\0') {
    RMW_SET_ERROR_MSG("topic_name is null or empty string");
    return nullptr;
  }
  RET_NULL_X(qos_policies, return nullptr);
  const rosidl_message_type_support_t * type_support = get_typesupport(type_supports);
  RET_NULL_X(type_support, return nullptr);
  const std::string type_name = get_message_type_name(type_support);
  if (type_name.empty()) {
    return nullptr;
  }
  CddsSubscription * sub = new CddsSubscription();
  dds_entity_t topic;
  dds_qos_t * qos;

  std::string fqtopic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", qos_policies);
  auto message_type_support = rmw_cyclonedds_cpp::make_message_value_type(type_supports);
  bool is_self_contained = message_type_support->is_self_contained();
  // Computed before the std::move below, which invalidates
  // message_type_support.
  const bool has_buffer_fields =
    rmw_cyclonedds_cpp::has_buffer_backed_fields(message_type_support.get());

  auto sertype = create_sertype(
    type_name,
    false,
    std::move(message_type_support));
  create_msg_dds_dynamic_type(
    type_support->typesupport_identifier, type_support->data, dds_ppant,
    sertype);
  struct ddsi_sertype * stact = nullptr;
  topic = create_topic(dds_ppant, fqtopic_name.c_str(), sertype, &stact);
  // Set before dds_create_reader below, same reasoning as the
  // publisher side's create_cdds_publisher().
  sub->base_topic_name = fqtopic_name;
  sub->node = node;
  sub->has_buffer_fields = has_buffer_fields;
  sub->sertype = stact;

  dds_listener_t * listener = dds_create_listener(&sub->user_callback_data);
  // Set the callback to listen for new messages
  dds_lset_data_available_arg(listener, dds_listener_callback, &sub->user_callback_data, false);
  // Set the corresponding callbacks to listen for events
  listener_set_event_callbacks(listener, &sub->user_callback_data);
  if (has_buffer_fields) {
    dds_lset_subscription_matched_arg(listener, on_subscription_matched, sub, false);
  }

  if (topic < 0) {
    set_error_message_from_create_topic(topic, fqtopic_name);
    goto fail_topic;
  }
  if ((qos = create_readwrite_qos(
      qos_policies, *type_support->get_type_hash_func(type_support), ignore_local_publications, "",
      has_buffer_fields
    )) == nullptr)
  {
    goto fail_qos;
  }
  sub->enth = dds_create_reader(dds_sub, topic, qos, listener);
  if (sub->enth < 0) {
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
  // Mirrors create_cdds_publisher's own second-build cost and the reason
  // it is not fixed here -- see that function's own comment.
  if (has_buffer_fields) {
    sub->buffer_message_value_type =
      rmw_cyclonedds_cpp::make_message_value_type(&sub->type_supports);
  }
#if CDDS_VERSION == CDDS_VERSION_0_10
  sub->is_loaning_available = is_self_contained && dds_is_loan_available(sub->enth);
#else
  // Some form of loaning is always possible, but the exact behaviour depends on type and
  // whether or not Iceoryx can be used.  I'm not sure to what the expectations are
  // exactly, this should keep it essentially unchanged from the behaviour in Humble and
  // Iron.
  sub->is_loaning_available = is_self_contained && dds_is_shared_memory_available(sub->enth);
#endif
  dds_delete_qos(qos);
  dds_delete(topic);

  return sub;
fail_readcond:
  // dds_create_reader() above already made sub->enth and its
  // on_subscription_matched listener live, so a concurrent match in this
  // window can spawn a detached async-retry thread capturing raw
  // `sub`/`sub->enth` before this label ever runs -- the identical
  // use-after-free class destroy_subscription()/create_subscription()'s
  // own scope_exit close for their own failure paths, needed again here
  // because this is a different, deeper failure path inside
  // create_cdds_subscription() itself.
  rmw_cyclonedds_cpp::begin_shutdown(sub->buffer_discovery);
  rmw_cyclonedds_cpp::wait_for_async_retries(sub->buffer_discovery);
  if (dds_delete(sub->enth) < 0) {
    RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to delete reader during error handling");
  }
  // Mirrors create_cdds_publisher's fail_instance_handle drain -- a
  // synchronous match completed before this failure path ran can have
  // inserted a private reader into this map, and the sequence above does
  // not drain it on its own.
  rmw_cyclonedds_cpp::drain_private_endpoints(sub->buffer_discovery);
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
  const rmw_node_t * node, dds_entity_t dds_ppant, dds_entity_t dds_sub,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies,
  const rmw_subscription_options_t * subscription_options)
{
  CddsSubscription * sub;
  rmw_subscription_t * rmw_subscription;
  if (
    (sub = create_cdds_subscription(
      node, dds_ppant, dds_sub, type_supports, topic_name, qos_policies,
      subscription_options->ignore_local_publications)) == nullptr)
  {
    return nullptr;
  }
  auto cleanup_subscription = rcpputils::make_scope_exit(
    [sub]() {
      // on_subscription_matched can fire (and spawn a detached async-retry
      // thread capturing `sub`/`sub->enth`) the instant
      // create_cdds_subscription() returns, before this scope_exit ever
      // runs -- the same race destroy_subscription()'s
      // begin_shutdown()/wait_for_async_retries() ordering exists to
      // close. This construction-failure path must use that same pair,
      // or `sub` is deleted into the identical use-after-free window.
      rmw_cyclonedds_cpp::begin_shutdown(sub->buffer_discovery);
      rmw_cyclonedds_cpp::wait_for_async_retries(sub->buffer_discovery);
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
      // Mirrors create_publisher's cleanup_cdds_publisher drain -- a
      // synchronous match completed before this scope_exit runs can have
      // inserted a private reader into this map, and the sequence above
      // does not drain it on its own.
      rmw_cyclonedds_cpp::drain_private_endpoints(sub->buffer_discovery);
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
  rmw_subscription->is_cft_supported = false;

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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
  // Adapt any 'best available' QoS options
  rmw_qos_profile_t adapted_qos_policies = *qos_policies;
  rmw_ret_t ret = rmw_dds_common::qos_profile_get_best_available_for_topic_subscription(
    node, topic_name, &adapted_qos_policies, rmw_get_publishers_info_by_topic);
  if (RMW_RET_OK != ret) {
    return nullptr;
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
    node, node->context->impl->ppant, node->context->impl->dds_sub,
    type_supports, topic_name, &adapted_qos_policies,
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
  if (RMW_RET_OK != common->add_subscriber_graph(
      cddssub->gid,
      node->name, node->namespace_))
  {
    return nullptr;
  }

  cleanup_subscription.cancel();
  TRACETOOLS_TRACEPOINT(rmw_subscription_init, static_cast<const void *>(sub), cddssub->gid.data);
  return sub;
}

extern "C" rmw_ret_t rmw_subscription_count_matched_publishers(
  const rmw_subscription_t * subscription, size_t * publisher_count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(publisher_count, RMW_RET_INVALID_ARGUMENT);

  auto sub = static_cast<CddsSubscription *>(subscription->data);
  const dds_return_t count = dds_get_matched_publications(sub->enth, nullptr, 0);
  if (count < 0) {
    return RMW_RET_ERROR;
  }

  *publisher_count = static_cast<size_t>(count);
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_subscription_get_actual_qos(
  const rmw_subscription_t * subscription,
  rmw_qos_profile_t * qos)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(subscription, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
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
  // Mirrors destroy_publisher's own ordering -- dds_delete's "blocks
  // until any invocation already executing has returned" guarantee only
  // covers the SYNCHRONOUS listener callback, not a std::thread a PRIOR
  // match already detached
  // and returned from. That thread, still sleeping in its retry loop,
  // holds `reader` (== sub->enth) regardless of when sub->enth is deleted.
  //
  // begin_shutdown() (stops new spawns) THEN wait_for_async_retries()
  // (drains every retry thread already spawned, from this match or an
  // earlier one) THEN the deletes: by the time sub->enth is actually
  // deleted, no thread can still be touching it.
  //
  // The deletes stay in their ORIGINAL relative order (rdcondh, a
  // condition derived from sub->enth, before the reader itself --
  // reversing that broke ordinary, non-buffer-backed subscription
  // teardown, caught by rcl-tested-rmw-cyclonedds-cpp dropping from
  // 30/30 to 19/30 on an earlier attempt that deleted sub->enth first).
  rmw_cyclonedds_cpp::begin_shutdown(sub->buffer_discovery);
  rmw_cyclonedds_cpp::wait_for_async_retries(sub->buffer_discovery);
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
  // Mirrors destroy_publisher()'s own cleanup above.
  //
  // Mirrors destroy_publisher's own drain -- must be exclusive, not
  // shared, since on_subscription_matched/finish_subscription_match's own
  // shared_lock reads of this map must not overlap this drain's mutation.
  // drain_private_endpoints() takes that lock itself.
  rmw_cyclonedds_cpp::drain_private_endpoints(sub->buffer_discovery);
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

  rmw_ret_t ret = RMW_RET_OK;
  rmw_error_state_t error_state;
  rmw_error_string_t error_string;
  auto common = &node->context->impl->common;
  const auto cddssub = static_cast<const CddsSubscription *>(subscription->data);
  ret = common->remove_subscriber_graph(
    cddssub->gid,
    node->name, node->namespace_);
  if (RMW_RET_OK != ret) {
    error_state = *rmw_get_error_state();
    error_string = rmw_get_error_string();
    rmw_reset_error();
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
  // TODO(iluetkeb) get received_timestamp from Cyclone when implemented there
  message_info->received_timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  message_info->publication_sequence_number = RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED;
  message_info->reception_sequence_number = RMW_MESSAGE_INFO_SEQUENCE_NUMBER_UNSUPPORTED;
}

// rmw_take_int()'s consumer of the publish-side descriptor protocol --
// the receive-side mirror of rmw_publish()'s own has_buffer_fields block
// above. Reads the descriptor sample PAIRED WITH `ros_message` (see the
// correlation paragraph below) from the private reader belonging to the
// peer that actually published it, converts it via backend->
// from_descriptor_with_endpoint(), and installs the result into the first
// buffer-backed field found in `ros_message` (install_first_buffer_impl(),
// TypeSupport2.hpp -- same scope boundary find_buffer_impl() already has
// on the publish side: one descriptor per matched peer).
//
// The peer is identified from `info`'s own publication_handle --
// dds_get_matched_publication_data() resolves that to the peer's real
// 16-byte GUID, the same key on_subscription_matched() used to create the
// buffer_discovery entry -- rather than assumed to be whichever peer most
// recently matched, which would silently misattribute a descriptor on any
// subscription with more than one matched publisher.
//
// CORRELATION: a bare "take whatever is next in the private reader's
// queue" is not safe on its own. rmw_publish() can legitimately
// skip writing a descriptor for one specific message (create_descriptor_
// with_endpoint() returning nullptr is documented CPU-fallback, not an
// error) while the shared-topic write for that SAME message still goes
// out -- so the private-topic stream is a sparse subset of the shared-topic
// stream, with nothing marking which messages were skipped. Taking
// "whatever is next" would then hand a LATER message's descriptor to an
// EARLIER message's buffer field: silent cross-message misattribution, not
// the disclosed "stays on CPU" miss case. Fixed by correlating on the exact
// dds_write_ts() timestamp both writes share (rmw_publish() passes the
// identical `tstamp` to both calls) -- a descriptor is only ever installed
// when its own timestamp equals `info.source_timestamp`, never merely
// because it was the next thing in the queue. A descriptor drained early
// (its message hasn't been taken yet) or one whose message's descriptor
// write was skipped and will therefore never be claimed is cached on the
// PrivateEndpoint (bounded, see its own comment), not discarded and not
// blindly installed.
//
// KNOWN, DISCLOSED LIMITATION: the shared-topic message and its paired
// private-topic descriptor are still two independent DDS writes with no
// ordering guarantee reaching this process, so the exact match this
// function requires may simply not have arrived yet. On that genuine miss,
// `ros_message`'s buffer-backed field stays on whatever backend it was
// constructed with (the ordinary default CPU one) rather than blocking
// rmw_take() or retrying, logged at DEBUG rather than ERROR -- a benign,
// expected race under load, not a defect; this project's own gtest suite
// is what measures its actual hit rate.
//
// One condition worth naming explicitly: no buffer backend plugin
// registered locally for anything the peer advertised is already a
// disclosed, logged (ERROR) condition, not a silent downgrade --
// finish_subscription_match()'s existing "no buffer backend plugin
// registered for any of the common set" log already covers it. Nothing
// here needs to duplicate that -- a peer with no matching entry in
// buffer_discovery is exactly the case that log already covers.
static void consume_buffer_descriptor(
  CddsSubscription * sub, const dds_sample_info_t & info, void * ros_message)
{
  dds_builtintopic_endpoint_t * ep =
    dds_get_matched_publication_data(sub->enth, info.publication_handle);
  if (ep == nullptr) {
    return;
  }
  rmw_cyclonedds_cpp::PeerGuid peer_guid;
  memcpy(peer_guid.data(), ep->key.v, peer_guid.size());
  dds_builtintopic_free_endpoint(ep);

  // Mirrors rmw_publish()'s own shared_lock reasoning exactly -- see
  // BufferEndpointDiscovery.hpp's comment on the mutex. Held for the rest
  // of this function's body, across every backend call below -- the
  // identical shape rmw_publish()'s own shared_lock takes across its own
  // for-loop of backend calls, not a difference between the two paths.
  // This pattern can in principle starve a pending exclusive-lock writer
  // (a new match's emplace()) on Linux, where std::shared_mutex has no
  // default writer-priority guarantee. Not fixed here -- a narrower lock
  // needs either copying the map entry's contents out first or a
  // different container with stable element addresses, both real
  // redesigns of code that took several rounds to get correct the first
  // time.
  std::shared_lock<std::shared_mutex> lock(sub->buffer_discovery.mutex);
  auto it = sub->buffer_discovery.private_writers_or_readers.find(peer_guid);
  if (it == sub->buffer_discovery.private_writers_or_readers.end() || !it->second.backend) {
    // No common backend was negotiated with this peer (or negotiation is
    // still in flight, or this peer never advertised one at all) -- the
    // shared-topic message's field keeps its ordinary default CPU buffer.
    return;
  }
  auto & priv = it->second;

  // The catch handlers below must NOT re-invoke
  // priv.backend->get_backend_type() to build their own log message -- if
  // get_backend_type() itself is what threw, that second call throws
  // again, uncaught, escaping this function and rmw_take_int() across the
  // extern "C" rmw_take() ABI boundary (the exact hazard
  // finish_publication_match()/finish_subscription_match() already avoid
  // by using a pre-captured backend_type string instead of re-calling
  // get_backend_type()). Captured once, guarded, and reused for every
  // subsequent log/lock-key use in this function.
  std::string backend_type;
  try {
    backend_type = priv.backend->get_backend_type();
  } catch (const std::exception & e) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_take: backend get_backend_type threw: %s", e.what());
    return;
  }

  // Guards ONLY this peer's own private reader and its
  // pending_descriptors/pending_order cache -- see PrivateEndpoint's own
  // comment on reader_cache_mutex. It does NOT serialize calls into
  // `backend` itself; backend_instance_mutex() below does that.
  std::shared_ptr<void> matched_descriptor;
  {
    std::lock_guard<std::mutex> cache_lock(priv.reader_cache_mutex);
    auto cached = priv.pending_descriptors.find(info.source_timestamp);
    if (cached != priv.pending_descriptors.end()) {
      matched_descriptor = cached->second;
      priv.pending_descriptors.erase(cached);
      priv.pending_order.erase(
        std::remove(
          priv.pending_order.begin(), priv.pending_order.end(),
          info.source_timestamp), priv.pending_order.end());
    } else {
      // Drain whatever is currently available on the private reader,
      // looking for the exact timestamp match; anything else drained along
      // the way is cached (bounded) rather than discarded, since it likely
      // belongs to a message this subscription hasn't taken yet.
      for (size_t i = 0;
        i < rmw_cyclonedds_cpp::PrivateEndpoint::kMaxPendingDescriptors && !matched_descriptor;
        ++i)
      {
        // create_empty_descriptor() and from_descriptor_with_endpoint()
        // (below) are direct virtual calls into plugin code with no
        // noexcept guarantee -- the same hazard rmw_publish()'s
        // create_descriptor_with_endpoint() call is guarded for, and this
        // function is reached from the identical kind of boundary:
        // rmw_take_int() via the extern "C" rmw_take() entry point, on
        // every take for a buffer-backed subscription. An exception is
        // treated exactly like the documented null return.
        std::shared_ptr<void> descriptor;
        try {
          std::lock_guard<std::mutex> backend_lock(
            rmw_cyclonedds_cpp::backend_instance_mutex(backend_type));
          descriptor = priv.backend->create_empty_descriptor();
        } catch (const std::exception & e) {
          RCUTILS_LOG_ERROR_NAMED(
            "rmw_cyclonedds_cpp",
            "rmw_take: backend '%s' create_empty_descriptor threw: %s",
            backend_type.c_str(), e.what());
          break;
        }
        if (!descriptor) {
          RCUTILS_LOG_ERROR_NAMED(
            "rmw_cyclonedds_cpp",
            "rmw_take: backend '%s' failed to allocate an empty descriptor",
            backend_type.c_str());
          break;
        }
        void * descriptor_raw = descriptor.get();
        dds_sample_info_t descriptor_info;
        int rc = dds_take(priv.entity, &descriptor_raw, &descriptor_info, 1, 1);
        if (rc != 1 || !descriptor_info.valid_data) {
          // Nothing more available right now -- stop draining.
          break;
        }
        if (descriptor_info.source_timestamp == info.source_timestamp) {
          matched_descriptor = descriptor;
          break;
        }
        if (priv.pending_order.size() >=
          rmw_cyclonedds_cpp::PrivateEndpoint::kMaxPendingDescriptors)
        {
          dds_time_t oldest = priv.pending_order.front();
          priv.pending_order.pop_front();
          priv.pending_descriptors.erase(oldest);
        }
        priv.pending_descriptors[descriptor_info.source_timestamp] = descriptor;
        priv.pending_order.push_back(descriptor_info.source_timestamp);
      }
    }
  }
  if (!matched_descriptor) {
    RCUTILS_LOG_DEBUG_NAMED(
      "rmw_cyclonedds_cpp",
      "rmw_take: no paired descriptor yet on private reader for backend '%s' -- "
      "field stays on its current backend for this sample",
      backend_type.c_str());
    return;
  }

  std::unique_ptr<void, void (*)(void *)> impl(nullptr, nullptr);
  try {
    std::lock_guard<std::mutex> backend_lock(
      rmw_cyclonedds_cpp::backend_instance_mutex(backend_type));
    impl = priv.backend->from_descriptor_with_endpoint(
      matched_descriptor.get(), priv.endpoint_info);
  } catch (const std::exception & e) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "rmw_take: backend '%s' from_descriptor_with_endpoint threw: %s",
      backend_type.c_str(), e.what());
    return;
  }
  if (!impl) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "rmw_take: backend '%s' from_descriptor_with_endpoint() returned null -- "
      "field stays on its current backend for this sample",
      backend_type.c_str());
    return;
  }
  if (!sub->buffer_message_value_type ||
    !rmw_cyclonedds_cpp::install_first_buffer_impl(
      sub->buffer_message_value_type.get(), ros_message, impl))
  {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "rmw_take: received a '%s' backend descriptor but found no buffer-backed "
      "field to install it into", backend_type.c_str());
  }
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
    subscription handle, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
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
      // Consume this sample's paired descriptor, if any -- see
      // consume_buffer_descriptor()'s own comment.
      // has_buffer_fields is checked BEFORE touching buffer_discovery at
      // all -- same reasoning as rmw_publish()'s own gate; see
      // CddsPublisher::has_buffer_fields's comment for the full argument
      // and the naming collision it's careful to rule out.
      if (sub->has_buffer_fields) {
        consume_buffer_descriptor(sub, info, ros_message);
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
  TRACETOOLS_TRACEPOINT(
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

  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
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
      "Cannot take %zu samples at once, limit is %" PRIu32,
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

#if CDDS_VERSION > CDDS_VERSION_0_10
static bool rmw_take_ser_int_from_shm(
  struct ddsi_serdata * d,
  rmw_serialized_message_t * serialized_message)
{
  if (d->loan == nullptr) {
    return false;
  }
  if (d->loan->metadata->sample_state != DDS_LOANED_SAMPLE_STATE_SERIALIZED_DATA) {
    return false;
  }
  const size_t size = d->loan->metadata->sample_size;
  if (rmw_serialized_message_resize(serialized_message, size) != RMW_RET_OK) {
    return false;
  }
  std::memcpy(serialized_message->buffer, d->loan->sample_ptr, size);
  serialized_message->buffer_length = size;
  return true;
}
#elif defined DDS_HAS_SHM
static bool rmw_take_ser_int_from_shm(
  struct ddsi_serdata * d,
  rmw_serialized_message_t * serialized_message)
{
  if (d->iox_chunk == nullptr) {
    return false;
  }
  auto iox_header = iceoryx_header_from_chunk(d->iox_chunk);
  if (iox_header->shm_data_state != IOX_CHUNK_CONTAINS_SERIALIZED_DATA) {
    return false;
  }
  const size_t size = iox_header->data_size;
  if (rmw_serialized_message_resize(serialized_message, size) != RMW_RET_OK) {
    return false;
  }
  std::memcpy(serialized_message->buffer, d->iox_chunk, size);
  serialized_message->buffer_length = size;
  return true;
}
#else
static bool rmw_take_ser_int_from_shm(struct ddsi_serdata *, rmw_serialized_message_t *)
{
  return false;
}
#endif

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
    subscription handle, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  CddsSubscription * sub = static_cast<CddsSubscription *>(subscription->data);
  RET_NULL(sub);
  dds_sample_info_t info;
  struct ddsi_serdata * d;
  while (dds_takecdr(sub->enth, &d, 1, &info, DDS_ANY_STATE) == 1) {
    if (!info.valid_data) {
      ddsi_serdata_unref(d);
      continue;
    }

    if (message_info) {
      message_info_from_sample_info(info, message_info);
    }

    if (rmw_take_ser_int_from_shm(d, serialized_message)) {
      ddsi_serdata_unref(d);
      *taken = true;
      TRACETOOLS_TRACEPOINT(
        rmw_take,
        static_cast<const void *>(subscription),
        static_cast<const void *>(serialized_message),
        (message_info ? message_info->source_timestamp : 0LL),
        *taken);
      return RMW_RET_OK;
    } else {
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
      TRACETOOLS_TRACEPOINT(
        rmw_take,
        static_cast<const void *>(subscription),
        static_cast<const void *>(serialized_message),
        (message_info ? message_info->source_timestamp : 0LL),
        *taken);
      return RMW_RET_OK;
    }
  }
  *taken = false;
  TRACETOOLS_TRACEPOINT(
    rmw_take,
    static_cast<const void *>(subscription),
    static_cast<const void *>(serialized_message),
    0LL,
    *taken);
  return RMW_RET_OK;
}

#if CDDS_VERSION > CDDS_VERSION_0_10
static rmw_ret_t rmw_take_loan_int(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_message_info_t * message_info)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(
    subscription, RMW_RET_INVALID_ARGUMENT);
  if (!subscription->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(
    loaned_message, RMW_RET_INVALID_ARGUMENT);
  if (*loaned_message != nullptr) {
    RMW_SET_ERROR_MSG("Loaned message pointer on input must be NULL");
    return RMW_RET_INVALID_ARGUMENT;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(
    taken, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto cdds_subscription = static_cast<CddsSubscription *>(subscription->data);
  if (!cdds_subscription) {
    RMW_SET_ERROR_MSG("Subscription data is null");
    return RMW_RET_ERROR;
  }

  dds_sample_info_t info;
  int32_t nread;
  *taken = false;
  while ((nread = dds_take(cdds_subscription->enth, loaned_message, &info, 1, 1)) == 1) {
    if (!info.valid_data) {
      continue;
    }
    *taken = true;
    if (message_info) {
      message_info_from_sample_info(info, message_info);
    }
    break;
  }
  TRACETOOLS_TRACEPOINT(
    rmw_take,
    static_cast<const void *>(subscription),
    static_cast<const void *>(*loaned_message),
    (message_info ? message_info->source_timestamp : 0LL),
    *taken);
  return (nread < 0) ? RMW_RET_ERROR : RMW_RET_OK;
}
#elif defined DDS_HAS_SHM
static rmw_ret_t rmw_take_loan_int(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_message_info_t * message_info)
{
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
    subscription handle, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto cdds_subscription = static_cast<CddsSubscription *>(subscription->data);
  if (!cdds_subscription) {
    RMW_SET_ERROR_MSG("Subscription data is null");
    return RMW_RET_ERROR;
  }

  dds_sample_info_t info;
  struct ddsi_serdata * d;
  rmw_ret_t ret = RMW_RET_OK;
  int32_t nread = 0;
  *taken = false;
  while ((nread = dds_takecdr(cdds_subscription->enth, &d, 1, &info, DDS_ANY_STATE)) == 1) {
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
            ret = RMW_RET_ERROR;
            goto take_done;
          }
        } else if (iox_header->shm_data_state == IOX_CHUNK_CONTAINS_RAW_DATA) {
          *loaned_message = d->iox_chunk;
        } else {
          RMW_SET_ERROR_MSG("Received iox chunk is uninitialized");
          ddsi_serdata_unref(d);
          ret = RMW_RET_ERROR;
          goto take_done;
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
        goto take_done;
      } else if (d->type->iox_size > 0U) {
        auto sample_ptr = init_and_alloc_sample(cdds_subscription, d->type->iox_size, true);
        RET_NULL_X(sample_ptr, return RMW_RET_ERROR);
        ddsi_serdata_to_sample(d, sample_ptr, nullptr, nullptr);
        *loaned_message = sample_ptr;
        ddsi_serdata_unref(d);
        *taken = true;
        goto take_done;
      } else {
        RMW_SET_ERROR_MSG("Data nor loan is available to take");
        ddsi_serdata_unref(d);
        ret = RMW_RET_ERROR;
        goto take_done;
      }
    }
    ddsi_serdata_unref(d);
  }
  if (nread < 0) {
    ret = RMW_RET_ERROR;
  }
take_done:
  TRACETOOLS_TRACEPOINT(
    rmw_take,
    static_cast<const void *>(subscription),
    static_cast<const void *>(*loaned_message),
    (message_info ? message_info->source_timestamp : 0LL),
    *taken);
  return ret;
}
#else
static rmw_ret_t rmw_take_loan_int(
  const rmw_subscription_t * subscription,
  void ** loaned_message,
  bool * taken,
  rmw_message_info_t * message_info)
{
  static_cast<void>(subscription);
  static_cast<void>(loaned_message);
  static_cast<void>(taken);
  static_cast<void>(message_info);
  RMW_SET_ERROR_MSG("rmw_take_loaned_message not implemented for rmw_cyclonedds_cpp");
  return RMW_RET_UNSUPPORTED;
}
#endif

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

extern "C" rmw_ret_t rmw_return_loaned_message_from_subscription(
  const rmw_subscription_t * subscription,
  void * loaned_message)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(
    subscription, RMW_RET_INVALID_ARGUMENT);
  if (!subscription->can_loan_messages) {
    RMW_SET_ERROR_MSG("Loaning is not supported");
    return RMW_RET_UNSUPPORTED;
  }
  RMW_CHECK_ARGUMENT_FOR_NULL(
    loaned_message, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription handle, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto cdds_subscription = static_cast<CddsSubscription *>(subscription->data);
  if (!cdds_subscription) {
    RMW_SET_ERROR_MSG("Subscription data is null");
    return RMW_RET_ERROR;
  }

  // if the subscription allow loaning
  if (cdds_subscription->is_loaning_available) {
#if CDDS_VERSION > CDDS_VERSION_0_10 || defined DDS_HAS_SHM
    return fini_and_free_sample(cdds_subscription, loaned_message);
#else
    RMW_SET_ERROR_MSG(
      "rmw_return_loaned_message_from_subscription not implemented for rmw_cyclonedds_cpp");
    return RMW_RET_UNSUPPORTED;
#endif
  } else {
    RMW_SET_ERROR_MSG("returning loan for a non fixed type is not allowed");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}


/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    DYNAMIC MESSAGE TYPESUPPORT                                    ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

extern "C" rmw_ret_t rmw_take_dynamic_message(
  const rmw_subscription_t * subscription,
  rosidl_dynamic_typesupport_dynamic_data_t * dynamic_message,
  bool * taken,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(dynamic_message);
  static_cast<void>(taken);
  static_cast<void>(allocation);

  RMW_SET_ERROR_MSG("rmw_take_dynamic_message: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_take_dynamic_message_with_info(
  const rmw_subscription_t * subscription,
  rosidl_dynamic_typesupport_dynamic_data_t * dynamic_message,
  bool * taken,
  rmw_message_info_t * message_info,
  rmw_subscription_allocation_t * allocation)
{
  static_cast<void>(subscription);
  static_cast<void>(dynamic_message);
  static_cast<void>(taken);
  static_cast<void>(message_info);
  static_cast<void>(allocation);

  RMW_SET_ERROR_MSG("rmw_take_dynamic_message_with_info: unimplemented");
  return RMW_RET_UNSUPPORTED;
}

extern "C" rmw_ret_t rmw_serialization_support_init(
  const char * serialization_lib_name,
  rcutils_allocator_t * allocator,
  rosidl_dynamic_typesupport_serialization_support_t * serialization_support)
{
  static_cast<void>(serialization_lib_name);
  static_cast<void>(allocator);
  static_cast<void>(serialization_support);

  RMW_SET_ERROR_MSG("rmw_serialization_support_init: unimplemented");
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
  {RMW_EVENT_MESSAGE_LOST, DDS_SAMPLE_LOST_STATUS},
  {RMW_EVENT_PUBLISHER_INCOMPATIBLE_TYPE, DDS_INCONSISTENT_TOPIC_STATUS},
  {RMW_EVENT_SUBSCRIPTION_INCOMPATIBLE_TYPE, DDS_INCONSISTENT_TOPIC_STATUS},
  {RMW_EVENT_SUBSCRIPTION_MATCHED, DDS_SUBSCRIPTION_MATCHED_STATUS},
  {RMW_EVENT_PUBLICATION_MATCHED, DDS_PUBLICATION_MATCHED_STATUS}
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
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    publisher, publisher->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);

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
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    subscription, subscription->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  return init_rmw_event(
    rmw_event,
    subscription->implementation_identifier,
    subscription->data,
    event_type);
}

extern "C" bool rmw_event_type_is_supported(rmw_event_type_t rmw_event_type)
{
  return is_event_supported(rmw_event_type);
}

extern "C" rmw_ret_t rmw_take_event(
  const rmw_event_t * event_handle, void * event_info,
  bool * taken)
{
  RET_NULL(event_handle);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    event_handle, event_handle->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
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

    case RMW_EVENT_SUBSCRIPTION_MATCHED: {
        auto ei = static_cast<rmw_matched_status_t *>(event_info);
        auto sub = static_cast<CddsSubscription *>(event_handle->data);

        dds_subscription_matched_status_t st;
        if (dds_get_subscription_matched_status(sub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        }
        ei->total_count = static_cast<size_t>(st.total_count);
        ei->total_count_change = static_cast<size_t>(st.total_count_change);
        ei->current_count = static_cast<size_t>(st.current_count);
        ei->current_count_change = st.current_count_change;
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

    case RMW_EVENT_PUBLISHER_INCOMPATIBLE_TYPE: {
        auto it = static_cast<rmw_incompatible_type_status_t *>(event_info);
        auto pub = static_cast<CddsPublisher *>(event_handle->data);

        const dds_entity_t topic = dds_get_topic(pub->enth);
        dds_inconsistent_topic_status_t st;
        if (dds_get_inconsistent_topic_status(topic, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        } else {
          it->total_count = static_cast<int32_t>(st.total_count);
          it->total_count_change = st.total_count_change;
          *taken = true;
          return RMW_RET_OK;
        }
      }

    case RMW_EVENT_SUBSCRIPTION_INCOMPATIBLE_TYPE: {
        auto it = static_cast<rmw_incompatible_type_status_t *>(event_info);
        auto sub = static_cast<CddsSubscription *>(event_handle->data);

        const dds_entity_t topic = dds_get_topic(sub->enth);
        dds_inconsistent_topic_status_t st;
        if (dds_get_inconsistent_topic_status(topic, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        } else {
          it->total_count = static_cast<int32_t>(st.total_count);
          it->total_count_change = st.total_count_change;
          *taken = true;
          return RMW_RET_OK;
        }
      }

    case RMW_EVENT_PUBLICATION_MATCHED: {
        auto ei = static_cast<rmw_matched_status_t *>(event_info);
        auto pub = static_cast<CddsPublisher *>(event_handle->data);

        dds_publication_matched_status st;
        if (dds_get_publication_matched_status(pub->enth, &st) < 0) {
          *taken = false;
          return RMW_RET_ERROR;
        }
        ei->total_count = static_cast<size_t>(st.total_count);
        ei->total_count_change = static_cast<size_t>(st.total_count_change);
        ei->current_count = static_cast<size_t>(st.current_count);
        ei->current_count_change = st.current_count_change;
        *taken = true;
        return RMW_RET_OK;
      }
    case RMW_EVENT_INVALID:
    case RMW_EVENT_TYPE_MAX: {
        break;
      }
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
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    guard_condition_handle, guard_condition_handle->implementation_identifier,
    eclipse_cyclonedds_identifier, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(wait_set, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    wait_set, wait_set->implementation_identifier,
    eclipse_cyclonedds_identifier, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
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

      uint32_t status_kind = get_status_kind_from_rmw(current_event->event_type);
      // TODO(clalancette): This should be reenabled when Cyclone supports reporting inconsistent
      // topic as an event
      if (status_kind != DDS_INCONSISTENT_TOPIC_STATUS) {
        status_mask_map[dds_entity] |= get_status_kind_from_rmw(current_event->event_type);
      }
    }
  }
  for (auto & pair : status_mask_map) {
    // set the status condition's mask with the supported type
    dds_return_t ret = dds_set_status_mask(pair.first, pair.second);
    if (ret != DDS_RETCODE_OK) {
      RMW_SET_ERROR_MSG("Failed setting the status mask");
      return RMW_RET_ERROR;
    }
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
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    wait_set, wait_set->implementation_identifier,
    eclipse_cyclonedds_identifier, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
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

    // Attach subscriptions
    ws->subs.resize(0);
    if (subs) {
      ws->subs.reserve(subs->subscriber_count);
      for (size_t i = 0; i < subs->subscriber_count; i++) {
        auto x = static_cast<CddsSubscription *>(subs->subscribers[i]);
        ws->subs.push_back(x);
        dds_waitset_attach(ws->waitseth, x->rdcondh, nelems++);
      }
    }

    // Attach guard conditions
    ws->gcs.resize(0);
    if (gcs) {
      ws->gcs.reserve(gcs->guard_condition_count);
      for (size_t i = 0; i < gcs->guard_condition_count; i++) {
        auto x = static_cast<CddsGuardCondition *>(gcs->guard_conditions[i]);
        ws->gcs.push_back(x);
        dds_waitset_attach(ws->waitseth, x->gcondh, nelems++);
      }
    }

    // Attach service servers
    ws->srvs.resize(0);
    if (srvs) {
      ws->srvs.reserve(srvs->service_count);
      for (size_t i = 0; i < srvs->service_count; i++) {
        auto x = static_cast<CddsService *>(srvs->services[i]);
        ws->srvs.push_back(x);
        dds_waitset_attach(ws->waitseth, x->service.sub->rdcondh, nelems++);
      }
    }

    // Attach service clients
    ws->cls.resize(0);
    if (cls) {
      ws->cls.reserve(cls->client_count);
      for (size_t i = 0; i < cls->client_count; i++) {
        auto x = static_cast<CddsClient *>(cls->clients[i]);
        ws->cls.push_back(x);
        dds_waitset_attach(ws->waitseth, x->client.sub->rdcondh, nelems++);
      }
    }

    // Attach events
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
    (wait_timeout == nullptr) ?
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
    size_t nelems = 0;

    // Detach subscriptions
    if (subs) {
      for (size_t i = 0; i < subs->subscriber_count; i++) {
        if (ws->trigs[trig_idx] == static_cast<dds_attach_t>(nelems)) {
          trig_idx++;
        } else {
          subs->subscribers[i] = nullptr;
        }
        nelems++;
      }
    }

    // Detach guard conditions
    if (gcs) {
      for (size_t i = 0; i < gcs->guard_condition_count; i++) {
        auto x = static_cast<CddsGuardCondition *>(gcs->guard_conditions[i]);
        if (ws->trigs[trig_idx] == static_cast<dds_attach_t>(nelems)) {
          bool dummy;
          dds_take_guardcondition(x->gcondh, &dummy);
          trig_idx++;
        } else {
          gcs->guard_conditions[i] = nullptr;
        }
        nelems++;
      }
    }

    // Detach service servers
    if (srvs) {
      for (size_t i = 0; i < srvs->service_count; i++) {
        if (ws->trigs[trig_idx] == static_cast<dds_attach_t>(nelems)) {
          trig_idx++;
        } else {
          srvs->services[i] = nullptr;
        }
        nelems++;
      }
    }

    // Detach service clients
    if (cls) {
      for (size_t i = 0; i < cls->client_count; i++) {
        if (ws->trigs[trig_idx] == static_cast<dds_attach_t>(nelems)) {
          trig_idx++;
        } else {
          cls->clients[i] = nullptr;
        }
        nelems++;
      }
    }

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
      // TODO(iluetkeb) get received_timestamp from Cyclone when implemented there
      request_header->received_timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
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
    client, client->implementation_identifier, eclipse_cyclonedds_identifier,
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
  TRACETOOLS_TRACEPOINT(
    rmw_take_response,
    static_cast<const void *>(client),
    static_cast<const void *>(ros_response),
    (nullptr != request_header ? request_header->request_id.sequence_number : 0LL),
    (nullptr != request_header ? request_header->source_timestamp : 0LL),
    // rmw_take_response_request() will not take if taken==nullptr
    (nullptr != taken ? *taken : false));
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
    service, service->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto info = static_cast<CddsService *>(service->data);
  rmw_ret_t ret = rmw_take_response_request(
    &info->service, request_header, ros_request, taken, nullptr,
    false);
  if (TRACETOOLS_TRACEPOINT_ENABLED(rmw_take_request)) {
    // Do not use the whole request_header->writer_guid, see the rmw_client_init tracepoint trigger
    rmw_gid_t gid{};
    memcpy(gid.data, &request_header->request_id.writer_guid, sizeof(info->service.pub->pubiid));
    TRACETOOLS_DO_TRACEPOINT(
      rmw_take_request,
      static_cast<const void *>(service),
      static_cast<const void *>(ros_request),
      gid.data,
      (nullptr != request_header ? request_header->request_id.sequence_number : 0LL),
      *taken);
  }
  return ret;
}

static rmw_ret_t rmw_send_response_request(
  CddsCS * cs, const cdds_request_header_t & header,
  const void * ros_data, const dds_time_t timestamp)
{
  const cdds_request_wrapper_t wrap = {header, const_cast<void *>(ros_data)};
  if (dds_write_ts(cs->pub->enth, static_cast<const void *>(&wrap), timestamp) >= 0) {
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
    service, service->implementation_identifier, eclipse_cyclonedds_identifier,
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
      {
        const dds_time_t timestamp = dds_time();
        if (TRACETOOLS_TRACEPOINT_ENABLED(rmw_send_response)) {
          // Do not use request_header->writer_guid, see the rmw_client_init tracepoint trigger
          rmw_gid_t gid{};
          memcpy(gid.data, &header.guid, sizeof(header.guid));
          TRACETOOLS_DO_TRACEPOINT(
            rmw_send_response,
            static_cast<const void *>(service),
            static_cast<const void *>(ros_response),
            gid.data,
            header.seq,
            timestamp);
        }
        return rmw_send_response_request(&info->service, header, ros_response, timestamp);
      }
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
    client, client->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(ros_request, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(sequence_id, RMW_RET_INVALID_ARGUMENT);

  auto info = static_cast<CddsClient *>(client->data);
  cdds_request_header_t header;
  header.guid = info->client.pub->pubiid;
  header.seq = *sequence_id = ++next_request_id;
  const dds_time_t timestamp = dds_time();

#if REPORT_BLOCKED_REQUESTS
  {
    std::lock_guard<std::mutex> lock(info->lock);
    info->reqtime[header.seq] = timestamp;
  }
#endif

  TRACETOOLS_TRACEPOINT(
    rmw_send_request,
    static_cast<const void *>(client),
    static_cast<const void *>(ros_request),
    header.seq);
  return rmw_send_response_request(&info->client, header, ros_request, timestamp);
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
    sizeof(dds_guid_t) <= RMW_GID_STORAGE_SIZE,
    "client/service id assumed it can hold a DDSI GUID");
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
  const std::string request_type_name = get_request_type_name(type_support);
  if (request_type_name.empty()) {
    return RMW_RET_ERROR;
  }
  const std::string response_type_name = get_response_type_name(type_support);
  if (response_type_name.empty()) {
    return RMW_RET_ERROR;
  }

  auto pub = std::make_unique<CddsPublisher>();
  auto sub = std::make_unique<CddsSubscription>();
  std::string subtopic_name, pubtopic_name;
  dds_qos_t * pub_qos, * sub_qos;
  const rosidl_type_hash_t * pub_type_hash;
  const rosidl_type_hash_t * sub_type_hash;
  std::string user_data;
  const rosidl_type_hash_t * ser_type_hash;
  std::string ser_typehash_str;

  std::unique_ptr<rmw_cyclonedds_cpp::StructValueType> pub_msg_ts, sub_msg_ts;
  struct sertype_rmw * pub_st, * sub_st;

  dds_listener_t * listener = dds_create_listener(cb_data);
  dds_lset_data_available_arg(listener, dds_listener_callback, cb_data, false);

  if (is_service) {
    std::tie(sub_msg_ts, pub_msg_ts) =
      rmw_cyclonedds_cpp::make_request_response_value_types(type_supports);

    sub_type_hash = type_supports->request_typesupport->get_type_hash_func(
      type_supports->request_typesupport);
    pub_type_hash = type_supports->response_typesupport->get_type_hash_func(
      type_supports->response_typesupport);
    subtopic_name =
      make_fqtopic(ROS_SERVICE_REQUESTER_PREFIX, service_name, "Request", qos_policies);
    pubtopic_name = make_fqtopic(ROS_SERVICE_RESPONSE_PREFIX, service_name, "Reply", qos_policies);

    pub_st = create_sertype(
      response_type_name,
      true,
      std::move(pub_msg_ts));
    create_res_dds_dynamic_type(
      type_support->typesupport_identifier, type_support->data,
      node->context->impl->ppant, pub_st);
    sub_st = create_sertype(
      request_type_name,
      true,
      std::move(sub_msg_ts));
    create_req_dds_dynamic_type(
      type_support->typesupport_identifier, type_support->data,
      node->context->impl->ppant, sub_st);
  } else {
    std::tie(pub_msg_ts, sub_msg_ts) =
      rmw_cyclonedds_cpp::make_request_response_value_types(type_supports);

    pub_type_hash = type_supports->request_typesupport->get_type_hash_func(
      type_supports->request_typesupport);
    sub_type_hash = type_supports->response_typesupport->get_type_hash_func(
      type_supports->response_typesupport);
    pubtopic_name =
      make_fqtopic(ROS_SERVICE_REQUESTER_PREFIX, service_name, "Request", qos_policies);
    subtopic_name = make_fqtopic(ROS_SERVICE_RESPONSE_PREFIX, service_name, "Reply", qos_policies);

    pub_st = create_sertype(
      request_type_name,
      true,
      std::move(pub_msg_ts));
    create_req_dds_dynamic_type(
      type_support->typesupport_identifier, type_support->data,
      node->context->impl->ppant, pub_st);
    sub_st = create_sertype(
      response_type_name,
      true,
      std::move(sub_msg_ts));
    create_res_dds_dynamic_type(
      type_support->typesupport_identifier, type_support->data,
      node->context->impl->ppant, sub_st);
  }

  RCUTILS_LOG_DEBUG_NAMED(
    "rmw_cyclonedds_cpp", "************ %s Details *********",
    is_service ? "Service" : "Client");
  RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "Sub Topic %s", subtopic_name.c_str());
  RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "Pub Topic %s", pubtopic_name.c_str());
  RCUTILS_LOG_DEBUG_NAMED("rmw_cyclonedds_cpp", "***********");

  dds_entity_t pubtopic, subtopic;

  struct ddsi_sertype * pub_stact;
  pubtopic = create_topic(node->context->impl->ppant, pubtopic_name.c_str(), pub_st, &pub_stact);
  if (pubtopic < 0) {
    set_error_message_from_create_topic(pubtopic, pubtopic_name);
    goto fail_pubtopic;
  }

  subtopic = create_topic(node->context->impl->ppant, subtopic_name.c_str(), sub_st);
  if (subtopic < 0) {
    set_error_message_from_create_topic(subtopic, subtopic_name);
    goto fail_subtopic;
  }

  // store a unique identifier for this client/service in the user
  // data of the reader and writer so that we can always determine
  // which pairs belong together
  get_unique_csid(node, cs->id);
  user_data = std::string(is_service ? "serviceid=" : "clientid=") + csid_to_string(
    cs->id) + std::string(";");
  ser_type_hash = type_supports->get_type_hash_func(type_supports);
  if (RMW_RET_OK != rmw_dds_common::encode_sertype_hash_for_user_data_qos(
      *ser_type_hash, ser_typehash_str))
  {
    RCUTILS_LOG_WARN_NAMED(
      "rmw_cyclonedds_cpp",
      "Failed to encode service type hash for service, will not distribute it in USER_DATA.");
    ser_typehash_str.clear();
    // We've handled the error, so clear it out.
    rmw_reset_error();
  }
  user_data += ser_typehash_str;

  if ((pub_qos = create_readwrite_qos(qos_policies, *pub_type_hash, false, user_data)) == nullptr) {
    goto fail_pub_qos;
  }
  if ((sub_qos = create_readwrite_qos(qos_policies, *sub_type_hash, false, user_data)) == nullptr) {
    goto fail_sub_qos;
  }

  if ((pub->enth =
    dds_create_writer(node->context->impl->dds_pub, pubtopic, pub_qos, nullptr)) < 0)
  {
    RMW_SET_ERROR_MSG("failed to create writer");
    goto fail_writer;
  }
  get_entity_gid(pub->enth, pub->gid);
  pub->sertype = pub_stact;
  if ((sub->enth =
    dds_create_reader(node->context->impl->dds_sub, subtopic, sub_qos, listener)) < 0)
  {
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
  dds_delete_qos(pub_qos);
  dds_delete_qos(sub_qos);
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
  dds_delete_qos(sub_qos);
fail_sub_qos:
  dds_delete_qos(pub_qos);
fail_pub_qos:
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

extern "C" rmw_client_t * rmw_create_client(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name,
  const rmw_qos_profile_t * qos_policies)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_policies, nullptr);
  CddsClient * info = new CddsClient();
  auto cleanup_info = rcpputils::make_scope_exit(
    [info]() {
      delete (info);
    });

#if REPORT_BLOCKED_REQUESTS
  info->lastcheck = 0;
#endif
  rmw_qos_profile_t adapted_qos_policies =
    rmw_dds_common::qos_profile_update_best_available_for_services(*qos_policies);
  if (
    rmw_init_cs(
      &info->client, &info->user_callback_data,
      node, type_supports, service_name, &adapted_qos_policies, false) != RMW_RET_OK)
  {
    return nullptr;
  }
  auto cleanup_fini_cs = rcpputils::make_scope_exit(
    [info]() {
      rmw_fini_cs(&info->client);
    });

  rmw_client_t * rmw_client = rmw_client_allocate();
  if (!rmw_client) {
    return nullptr;
  }
  auto cleanup_client = rcpputils::make_scope_exit(
    [rmw_client]() {
      rmw_client_free(rmw_client);
    });

  auto common = &node->context->impl->common;
  rmw_client->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_client->data = info;
  rmw_client->service_name = reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
  if (!rmw_client->service_name) {
    return nullptr;
  }
  auto cleanup_service_name = rcpputils::make_scope_exit(
    [rmw_client]() {
      rmw_free(const_cast<char *>(rmw_client->service_name));
    });
  memcpy(const_cast<char *>(rmw_client->service_name), service_name, strlen(service_name) + 1);

  // Update graph
  if (RMW_RET_OK != common->add_client_graph(
      info->client.pub->gid,
      info->client.sub->gid,
      node->name, node->namespace_))
  {
    return nullptr;
  }

  cleanup_service_name.cancel();
  cleanup_client.cancel();
  cleanup_fini_cs.cancel();
  cleanup_info.cancel();
  if (TRACETOOLS_TRACEPOINT_ENABLED(rmw_client_init)) {
    // rmw_cyclonedds uses info->client.pub->pubiid as the internal request header GUID, which is
    // the first half (8 bytes out of 16 bytes) of the rmw_request_id_t's writer_guid. The second
    // half doesn't match when read from the client side and the service side, so only use the first
    // half. The second half will be zeros on both client side and service side.
    rmw_gid_t gid{};
    memcpy(gid.data, &info->client.pub->pubiid, sizeof(info->client.pub->pubiid));
    TRACETOOLS_DO_TRACEPOINT(rmw_client_init, static_cast<const void *>(rmw_client), gid.data);
  }
  return rmw_client;
}

extern "C" rmw_ret_t rmw_destroy_client(rmw_node_t * node, rmw_client_t * client)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client, client->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto info = static_cast<CddsClient *>(client->data);
  clean_waitset_caches();

  // Update graph
  auto common = &node->context->impl->common;
  if (RMW_RET_OK != common->remove_client_graph(
      info->client.pub->gid,
      info->client.sub->gid,
      node->name, node->namespace_))
  {
    RMW_SET_ERROR_MSG("failed to publish ParticipantEntitiesInfo when destroying client");
  }

  rmw_fini_cs(&info->client);
  delete info;
  rmw_free(const_cast<char *>(client->service_name));
  rmw_client_free(client);
  return RMW_RET_OK;
}

extern "C" rmw_service_t * rmw_create_service(
  const rmw_node_t * node,
  const rosidl_service_type_support_t * type_supports,
  const char * service_name,
  const rmw_qos_profile_t * qos_policies)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(qos_policies, nullptr);
  CddsService * info = new CddsService();
  auto cleanup_info = rcpputils::make_scope_exit(
    [info]() {
      delete (info);
    });
  rmw_qos_profile_t adapted_qos_policies =
    rmw_dds_common::qos_profile_update_best_available_for_services(*qos_policies);
  if (
    rmw_init_cs(
      &info->service, &info->user_callback_data,
      node, type_supports, service_name, &adapted_qos_policies, true) != RMW_RET_OK)
  {
    return nullptr;
  }
  auto cleanup_fini_cs = rcpputils::make_scope_exit(
    [info]() {
      rmw_fini_cs(&info->service);
    });
  rmw_service_t * rmw_service = rmw_service_allocate();
  if (!rmw_service) {
    return nullptr;
  }
  auto cleanup_service = rcpputils::make_scope_exit(
    [rmw_service]() {
      rmw_service_free(rmw_service);
    });
  auto common = &node->context->impl->common;
  rmw_service->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_service->data = info;
  rmw_service->service_name =
    reinterpret_cast<const char *>(rmw_allocate(strlen(service_name) + 1));
  if (!rmw_service->service_name) {
    return nullptr;
  }
  auto cleanup_service_name = rcpputils::make_scope_exit(
    [rmw_service]() {
      rmw_free(const_cast<char *>(rmw_service->service_name));
    });
  memcpy(const_cast<char *>(rmw_service->service_name), service_name, strlen(service_name) + 1);

  // Update graph
  if (RMW_RET_OK != common->add_service_graph(
      info->service.sub->gid,
      info->service.pub->gid,
      node->name, node->namespace_))
  {
    return nullptr;
  }

  cleanup_service_name.cancel();
  cleanup_service.cancel();
  cleanup_fini_cs.cancel();
  cleanup_info.cancel();
  return rmw_service;
}

extern "C" rmw_ret_t rmw_destroy_service(rmw_node_t * node, rmw_service_t * service)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(service, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    service, service->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  auto info = static_cast<CddsService *>(service->data);
  clean_waitset_caches();

  // Update graph
  auto common = &node->context->impl->common;
  if (RMW_RET_OK != common->remove_service_graph(
      info->service.sub->gid,
      info->service.pub->gid,
      node->name, node->namespace_))
  {
    RMW_SET_ERROR_MSG("failed to publish ParticipantEntitiesInfo when destroying service");
  }

  rmw_fini_cs(&info->service);
  delete info;
  rmw_free(const_cast<char *>(service->service_name));
  rmw_service_free(service);
  return RMW_RET_OK;
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node, node->implementation_identifier,
    eclipse_cyclonedds_identifier, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(client, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    client, client->implementation_identifier,
    eclipse_cyclonedds_identifier, return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(is_available, RMW_RET_INVALID_ARGUMENT);
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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

extern "C" rmw_ret_t rmw_count_clients(
  const rmw_node_t * node, const char * service_name,
  size_t * count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(service_name, RMW_RET_INVALID_ARGUMENT);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(count, RMW_RET_INVALID_ARGUMENT);
  auto common_context = &node->context->impl->common;
  const std::string mangled_rp_service_name =
    make_fqtopic(ROS_SERVICE_RESPONSE_PREFIX, service_name, "Reply", false);
  return common_context->graph_cache.get_reader_count(mangled_rp_service_name, count);
}

extern "C" rmw_ret_t rmw_count_services(
  const rmw_node_t * node, const char * service_name,
  size_t * count)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RMW_CHECK_ARGUMENT_FOR_NULL(service_name, RMW_RET_INVALID_ARGUMENT);
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
  RMW_CHECK_ARGUMENT_FOR_NULL(count, RMW_RET_INVALID_ARGUMENT);
  auto common_context = &node->context->impl->common;
  const std::string mangled_rq_topic_name =
    make_fqtopic(ROS_SERVICE_REQUESTER_PREFIX, service_name, "Request", false);
  return common_context->graph_cache.get_reader_count(mangled_rq_topic_name, count);
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
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

extern "C" rmw_ret_t rmw_get_clients_info_by_service(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * service_name,
  bool no_mangle,
  rmw_service_endpoint_info_array_t * clients_info)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(service_name, RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_service_endpoint_info_array_check_zero(clients_info)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (no_mangle) {
    // Services in DDS require mangled topic names
    // because they internally use separate readers and writers.
    // Therefore, this function cannot support the 'no_mangle' option.
    // If user need to query raw topic information without mangling,
    // use`rmw_get_publishers_info_by_topic` or `rmw_get_subscriptions_info_by_topic` instead.
    RMW_SET_ERROR_MSG(
      "'no_mangle' is not supported for services"
      " because they rely on internally mangled topic names.\n"
      "Use 'rmw_get_publishers_info_by_topic' or 'rmw_get_subscriptions_info_by_topic'"
      " instead to access unmangled topic information.");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto common_context = &node->context->impl->common;
  std::string mangled_rq_topic_name = \
    make_fqtopic(ROS_SERVICE_REQUESTER_PREFIX, service_name, "Request", false);
  std::string mangled_rp_topic_name = \
    make_fqtopic(ROS_SERVICE_RESPONSE_PREFIX, service_name, "Reply", false);
  DemangleFunction demangle_type = _demangle_service_type_only;

  rmw_topic_endpoint_info_array_t subscriptions_info =
    rmw_get_zero_initialized_topic_endpoint_info_array();
  std::unique_ptr<
    rmw_topic_endpoint_info_array_t,
    std::function<void(rmw_topic_endpoint_info_array_t *)>>
  subscriptions_info_delete(
    &subscriptions_info,
    [allocator](rmw_topic_endpoint_info_array_t * p) {
      rmw_ret_t ret = rmw_topic_endpoint_info_array_fini(
        p,
        allocator
      );
      if (RMW_RET_OK != ret) {
        RMW_SET_ERROR_MSG("Failed to destroy subscriptions_info when function ended.");
      }
    }
  );
  rmw_ret_t ret = common_context->graph_cache.get_readers_info_by_topic(
    mangled_rp_topic_name,
    demangle_type,
    allocator,
    &subscriptions_info);
  if (RMW_RET_OK != ret) {
    return ret;
  }

  rmw_topic_endpoint_info_array_t publishers_info =
    rmw_get_zero_initialized_topic_endpoint_info_array();
  std::unique_ptr<
    rmw_topic_endpoint_info_array_t,
    std::function<void(rmw_topic_endpoint_info_array_t *)>>
  publishers_info_delete(
    &publishers_info,
    [allocator](rmw_topic_endpoint_info_array_t * p) {
      rmw_ret_t ret = rmw_topic_endpoint_info_array_fini(
        p,
        allocator
      );
      if (RMW_RET_OK != ret) {
        RMW_SET_ERROR_MSG("Failed to destroy publishers_info when function ended.");
      }
    }
  );
  ret = common_context->graph_cache.get_writers_info_by_topic(
    mangled_rq_topic_name,
    demangle_type,
    allocator,
    &publishers_info);
  if (RMW_RET_OK != ret) {
    return ret;
  }
  return common_context->graph_cache.get_clients_info_by_service(
    &subscriptions_info,
    &publishers_info,
    allocator,
    clients_info);
}

extern "C" rmw_ret_t rmw_get_servers_info_by_service(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * service_name,
  bool no_mangle,
  rmw_service_endpoint_info_array_t * servers_info)
{
  RMW_CHECK_ARGUMENT_FOR_NULL(node, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    node, node->implementation_identifier, eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  RCUTILS_CHECK_ALLOCATOR_WITH_MSG(
    allocator, "allocator argument is invalid", return RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_ARGUMENT_FOR_NULL(service_name, RMW_RET_INVALID_ARGUMENT);
  if (RMW_RET_OK != rmw_service_endpoint_info_array_check_zero(servers_info)) {
    return RMW_RET_INVALID_ARGUMENT;
  }
  if (no_mangle) {
    // Services in DDS require mangled topic names
    // because they internally use separate readers and writers.
    // Therefore, this function cannot support the 'no_mangle' option.
    // If user need to query raw topic information without mangling,
    // use`rmw_get_publishers_info_by_topic` or `rmw_get_subscriptions_info_by_topic` instead.
    RMW_SET_ERROR_MSG(
      "'no_mangle' is not supported for services"
      " because they rely on internally mangled topic names.\n"
      "Use 'rmw_get_publishers_info_by_topic' or 'rmw_get_subscriptions_info_by_topic'"
      " instead to access unmangled topic information.");
    return RMW_RET_INVALID_ARGUMENT;
  }
  auto common_context = &node->context->impl->common;
  std::string mangled_rq_topic_name = \
    make_fqtopic(ROS_SERVICE_REQUESTER_PREFIX, service_name, "Request", false);
  std::string mangled_rp_topic_name = \
    make_fqtopic(ROS_SERVICE_RESPONSE_PREFIX, service_name, "Reply", false);
  DemangleFunction demangle_type = _demangle_service_type_only;

  rmw_topic_endpoint_info_array_t subscriptions_info =
    rmw_get_zero_initialized_topic_endpoint_info_array();
  std::unique_ptr<
    rmw_topic_endpoint_info_array_t,
    std::function<void(rmw_topic_endpoint_info_array_t *)>>
  subscriptions_info_delete(
    &subscriptions_info,
    [allocator](rmw_topic_endpoint_info_array_t * p) {
      rmw_ret_t ret = rmw_topic_endpoint_info_array_fini(
        p,
        allocator
      );
      if (RMW_RET_OK != ret) {
        RMW_SET_ERROR_MSG("Failed to destroy subscriptions_info when function failed.");
      }
    }
  );
  rmw_ret_t ret = common_context->graph_cache.get_readers_info_by_topic(
    mangled_rq_topic_name,
    demangle_type,
    allocator,
    &subscriptions_info);
  if (RMW_RET_OK != ret) {
    return ret;
  }

  rmw_topic_endpoint_info_array_t publishers_info =
    rmw_get_zero_initialized_topic_endpoint_info_array();
  std::unique_ptr<
    rmw_topic_endpoint_info_array_t,
    std::function<void(rmw_topic_endpoint_info_array_t *)>>
  publishers_info_delete(
    &publishers_info,
    [allocator](rmw_topic_endpoint_info_array_t * p) {
      rmw_ret_t ret = rmw_topic_endpoint_info_array_fini(
        p,
        allocator
      );
      if (RMW_RET_OK != ret) {
        RMW_SET_ERROR_MSG("Failed to destroy publishers_info when function failed.");
      }
    }
  );
  ret = common_context->graph_cache.get_writers_info_by_topic(
    mangled_rp_topic_name,
    demangle_type,
    allocator,
    &publishers_info);
  if (RMW_RET_OK != ret) {
    return ret;
  }
  return common_context->graph_cache.get_servers_info_by_service(
    &subscriptions_info,
    &publishers_info,
    allocator,
    servers_info);
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
