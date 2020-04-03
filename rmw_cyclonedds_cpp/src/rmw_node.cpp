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

#include "Serialization.hpp"
#include "rmw/impl/cpp/macros.hpp"

#include "TypeSupport2.hpp"

#include "rmw_cyclonedds_cpp/rmw_version_test.hpp"
#include "rmw_cyclonedds_cpp/MessageTypeSupport.hpp"
#include "rmw_cyclonedds_cpp/ServiceTypeSupport.hpp"

#if RMW_VERSION_GTE(0, 8, 2)
#include "rmw/get_topic_endpoint_info.h"
#include "rmw/incompatible_qos_events_statuses.h"
#include "rmw/topic_endpoint_info_array.h"
#endif

#include "namespace_prefix.hpp"

#include "dds/dds.h"
#include "dds/ddsi/ddsi_sertopic.h"
#include "rmw_cyclonedds_cpp/serdes.hpp"
#include "serdata.hpp"

/* Proper multi-domain support requires eliminating the "extra" participant, which in turn relies on
   the promotion of the Cyclone DDS library instance and the daomsin to full-fledged entities.  The
   custom RHC was introduced at essentially the same time */
#ifdef DDS_CYCLONEDDS_HANDLE
#define MULTIDOMAIN 1
#include "rmw_cyclonedds_cpp/graphrhc.hpp"
#else
#define MULTIDOMAIN 0
#endif

#if RMW_VERSION_GTE(0, 8, 1) && MULTIDOMAIN
#define SUPPORT_LOCALHOST 1
#else
#define SUPPORT_LOCALHOST 0
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

static const dds_entity_t builtin_topics[] = {
  DDS_BUILTIN_TOPIC_DCPSPARTICIPANT,
  DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION,
  DDS_BUILTIN_TOPIC_DCPSPUBLICATION
};

struct builtin_readers
{
  dds_entity_t rds[sizeof(builtin_topics) / sizeof(builtin_topics[0])];
};

struct CddsEntity
{
  dds_entity_t enth;
};

struct CddsNode : CddsEntity
{
  dds_entity_t pub;
  dds_entity_t sub;
  rmw_guard_condition_t * graph_guard_condition;
#if MULTIDOMAIN
  builtin_readers brd;
#endif
  /* domain id can be retrieved from the entities, but that requires assuming the call
     succeeds, storing it here means we are sure to have it */
  dds_domainid_t domain_id;
};

struct CddsPublisher : CddsEntity
{
  dds_instance_handle_t pubiid;
  struct ddsi_sertopic * sertopic;
};

struct CddsSubscription : CddsEntity
{
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

#if SUPPORT_LOCALHOST
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
  uint32_t n_nodes;

  /* handle of the domain entity, for versions of Cyclone that have an updated version of
     dds_create_domain that properly returns a handle; the original version has a value of
     0 in here (DDS_RETCODE_OK) which is never a valid handle */
  dds_entity_t domain_handle;

  /* Default constructor so operator[] can be safely be used to look one up */
  CddsDomain()
  : localhost_only(false), n_nodes(0), domain_handle(0)
  {}
};
#endif

struct Cdds
{
  std::mutex lock;

#if SUPPORT_LOCALHOST
  /* Map of domain id to per-domain state, used by create/destroy node */
  std::mutex domains_lock;
  std::map<dds_domainid_t, CddsDomain> domains;
#endif

#if !MULTIDOMAIN
  uint32_t refcount;
  dds_entity_t ppant;
  builtin_readers brd;
#endif

  /* special guard condition that gets attached to every waitset but that is never triggered:
     this way, we can avoid Cyclone's behaviour of always returning immediately when no
     entities are attached to a waitset */
  dds_entity_t gc_for_empty_waitset;

  /* set of waitsets protected by lock, used to invalidate all waitsets caches when an entity is
     deleted */
  std::unordered_set<CddsWaitset *> waitsets;

#if !MULTIDOMAIN
  /* set of nodes is used to trigger graph guard conditions when something changes, but that
   something also changes when creating the built-in readers when Cdds::lock is already held */
  std::mutex nodes_lock;
  std::unordered_set<CddsNode *> nodes;
#endif

#if MULTIDOMAIN
  Cdds()
  : gc_for_empty_waitset(0)
  {}
#else
  Cdds()
  : refcount(0), ppant(0), gc_for_empty_waitset(0)
  {}
#endif
};

static Cdds gcdds;

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
  static_cast<void>(severity);
  RMW_SET_ERROR_MSG("unimplemented");
  return RMW_RET_ERROR;
}

extern "C" rmw_ret_t rmw_context_fini(rmw_context_t * context)
{
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    context,
    context->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  // context impl is explicitly supposed to be nullptr for now, see rmw_init's code
  // RCUTILS_CHECK_ARGUMENT_FOR_NULL(context->impl, RMW_RET_INVALID_ARGUMENT);
  *context = rmw_get_zero_initialized_context();
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
#if RMW_VERSION_GTE(0, 8, 2)
  init_options->localhost_only = RMW_LOCALHOST_ONLY_DEFAULT;
  init_options->domain_id = RMW_DEFAULT_DOMAIN_ID;
  init_options->security_context = NULL;
  init_options->security_options = rmw_get_zero_initialized_security_options();
#endif
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
#if RMW_VERSION_GTE(0, 8, 2)
  const rcutils_allocator_t * allocator = &src->allocator;
  rmw_ret_t ret = RMW_RET_OK;

  allocator->deallocate(dst->security_context, allocator->state);
  *dst = *src;
  dst->security_context = NULL;
  dst->security_options = rmw_get_zero_initialized_security_options();

  dst->security_context = rcutils_strdup(src->security_context, *allocator);
  if (src->security_context && !dst->security_context) {
    ret = RMW_RET_BAD_ALLOC;
    goto fail;
  }
  return rmw_security_options_copy(&src->security_options, allocator, &dst->security_options);
fail:
  allocator->deallocate(dst->security_context, allocator->state);
  return ret;
#else
  *dst = *src;
  return RMW_RET_OK;
#endif
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
#if RMW_VERSION_GTE(0, 8, 2)
  allocator.deallocate(init_options->security_context, allocator.state);
  rmw_security_options_fini(&init_options->security_options, &allocator);
#endif
  *init_options = rmw_get_zero_initialized_init_options();
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_init(const rmw_init_options_t * options, rmw_context_t * context)
{
  static_cast<void>(options);
  static_cast<void>(context);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(options, RMW_RET_INVALID_ARGUMENT);
  RCUTILS_CHECK_ARGUMENT_FOR_NULL(context, RMW_RET_INVALID_ARGUMENT);
  RMW_CHECK_TYPE_IDENTIFIERS_MATCH(
    options,
    options->implementation_identifier,
    eclipse_cyclonedds_identifier,
    return RMW_RET_INCORRECT_RMW_IMPLEMENTATION);
  context->instance_id = options->instance_id;
  context->implementation_identifier = eclipse_cyclonedds_identifier;
  context->impl = nullptr;
#if RMW_VERSION_GTE(0, 8, 2)
  return rmw_init_options_copy(options, &context->options);
#else
  return RMW_RET_OK;
#endif
}

extern "C" rmw_ret_t rmw_node_assert_liveliness(const rmw_node_t * node)
{
  RET_WRONG_IMPLID(node);
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
  // context impl is explicitly supposed to be nullptr for now, see rmw_init's code
  // RCUTILS_CHECK_ARGUMENT_FOR_NULL (context->impl, RMW_RET_INVALID_ARGUMENT);
  *context = rmw_get_zero_initialized_context();
  return RMW_RET_OK;
}

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    GRAPH GUARD                                                    ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

static void ggcallback(dds_entity_t rd, void * varg)
{
#if MULTIDOMAIN
  auto gg = static_cast<rmw_guard_condition_t *>(varg);
  void * msg = 0;
  dds_sample_info_t info;
  while (dds_take(rd, &msg, &info, 1, 1) > 0) {
    dds_return_loan(rd, &msg, 1);
  }
  if (rmw_trigger_guard_condition(gg) != RMW_RET_OK) {
    RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to trigger graph guard condition");
  }
#else
  static_cast<void>(varg);
  void * msg = 0;
  dds_sample_info_t info;
  while (dds_take(rd, &msg, &info, 1, 1) > 0) {
    dds_return_loan(rd, &msg, 1);
  }

  {
    std::lock_guard<std::mutex> lock(gcdds.nodes_lock);
    for (auto && node_impl : gcdds.nodes) {
      if (rmw_trigger_guard_condition(node_impl->graph_guard_condition) != RMW_RET_OK) {
        RCUTILS_LOG_ERROR_NAMED("rmw_cyclonedds_cpp", "failed to trigger graph guard condition");
      }
    }
  }
#endif
}

static void builtin_readers_fini(builtin_readers & brd)
{
  for (size_t i = 0; i < sizeof(builtin_topics) / sizeof(builtin_topics[0]); i++) {
    if (brd.rds[i] > 0) {
      dds_delete(brd.rds[i]);
    }
  }
}

static bool builtin_readers_init(builtin_readers & brd, dds_entity_t pp, rmw_guard_condition_t * gg)
{
  /* Built-in topics readers: have to be per-node or the graph guard condition support becomes a
     real mess. */
#if MULTIDOMAIN
  assert(gg != nullptr);
#else
  assert(gg == nullptr);
#endif
  dds_listener_t * gglistener = dds_create_listener(static_cast<void *>(gg));
  dds_lset_data_available(gglistener, ggcallback);
  for (size_t i = 0; i < sizeof(builtin_topics) / sizeof(builtin_topics[0]); i++) {
    brd.rds[i] = 0;
  }
  for (size_t i = 0; i < sizeof(builtin_topics) / sizeof(builtin_topics[0]); i++) {
#if MULTIDOMAIN
    struct dds_rhc * rhc = graphrhc_new();
    dds_entity_t rd = dds_create_reader_rhc(pp, builtin_topics[i], nullptr, gglistener, rhc);
#else
    dds_entity_t rd = dds_create_reader(pp, builtin_topics[i], nullptr, gglistener);
#endif
    if (rd < 0) {
#if MULTIDOMAIN
      dds_rhc_free(rhc);
#endif
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DDS built-in reader");
      goto fail;
    }
    brd.rds[i] = rd;
  }
  dds_delete_listener(gglistener);
  return true;

fail:
  builtin_readers_fini(brd);
  dds_delete_listener(gglistener);
  return false;
}

#if !MULTIDOMAIN
static dds_entity_t ref_ppant()
{
  std::lock_guard<std::mutex> lock(gcdds.lock);
  if (gcdds.refcount == 0) {
    if ((gcdds.ppant = dds_create_participant(DDS_DOMAIN_DEFAULT, nullptr, nullptr)) < 0) {
      RMW_SET_ERROR_MSG("failed to create participant");
      return gcdds.ppant;
    }

    if ((gcdds.gc_for_empty_waitset = dds_create_guardcondition(gcdds.ppant)) < 0) {
      RMW_SET_ERROR_MSG("failed to create guardcondition for handling empty waitsets");
      dds_delete(gcdds.ppant);
      gcdds.ppant = 0;
      return DDS_RETCODE_ERROR;
    }

    if (!builtin_readers_init(gcdds.brd, gcdds.ppant, nullptr)) {
      dds_delete(gcdds.ppant);
      gcdds.ppant = 0;
      return DDS_RETCODE_ERROR;
    }
  }
  gcdds.refcount++;
  return gcdds.ppant;
}

static void unref_ppant()
{
  std::lock_guard<std::mutex> lock(gcdds.lock);
  if (--gcdds.refcount == 0) {
    builtin_readers_fini(gcdds.brd);
    dds_delete(gcdds.ppant);
    gcdds.ppant = 0;
  }
}
#endif

/////////////////////////////////////////////////////////////////////////////////////////
///////////                                                                   ///////////
///////////    NODES                                                          ///////////
///////////                                                                   ///////////
/////////////////////////////////////////////////////////////////////////////////////////

#if SUPPORT_LOCALHOST
static void node_gone_from_domain_locked(dds_domainid_t did)
{
  CddsDomain & dom = gcdds.domains[did];
  assert(dom.n_nodes > 0);
  if (--dom.n_nodes == 0) {
    if (dom.domain_handle > 0) {
      dds_delete(dom.domain_handle);
    }
    gcdds.domains.erase(did);
  }
}

static bool check_create_domain_locked(dds_domainid_t did, bool localhost_only)
{
  /* return true: n_nodes incremented, localhost_only set correctly, domain exists
     "      false: n_nodes unchanged, domain left intact if it already existed */
  CddsDomain & dom = gcdds.domains[did];
  if (dom.n_nodes != 0) {
    /* Localhost setting must match */
    if (localhost_only == dom.localhost_only) {
      dom.n_nodes++;
      return true;
    } else {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "rmw_create_node: attempt at creating localhost-only and non-localhost-only nodes "
        "in the same domain");
      return false;
    }
  } else {
    dom.n_nodes = 1;
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
      node_gone_from_domain_locked(did);
      return false;
    }

    if ((dom.domain_handle = dds_create_domain(did, config.c_str())) < 0) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "rmw_create_node: failed to create domain, error %s", dds_strretcode(dom.domain_handle));
      node_gone_from_domain_locked(did);
      return false;
    } else {
      return true;
    }
  }
}
#else
/* Dummy implementation to keep the rest of the code simpler */
static void node_gone_from_domain_locked(dds_domainid_t did)
{
  static_cast<void>(did);
}
#endif

static std::string get_node_user_data_name_ns(const char * node_name, const char * node_namespace)
{
  return std::string("name=") + std::string(node_name) +
         std::string(";namespace=") + std::string(node_namespace) +
         std::string(";");
}

#if RMW_VERSION_GTE(0, 8, 2)
static std::string get_node_user_data(
  const char * node_name, const char * node_namespace, const char * security_context)
{
  return get_node_user_data_name_ns(node_name, node_namespace) +
         std::string("securitycontext=") + std::string(security_context) +
         std::string(";");
}
#else
#define get_node_user_data get_node_user_data_name_ns
#endif

extern "C" rmw_node_t * rmw_create_node(
  rmw_context_t * context, const char * name,
  const char * namespace_, size_t domain_id
#if !RMW_VERSION_GTE(0, 8, 2)
  , const rmw_node_security_options_t * security_options
#endif
#if RMW_VERSION_GTE(0, 8, 1)
  , bool localhost_only
#endif
)
{
#if !RMW_VERSION_GTE(0, 8, 2)
  static_cast<void>(context);
#endif
  RET_NULL_X(name, return nullptr);
  RET_NULL_X(namespace_, return nullptr);
#if MULTIDOMAIN
  /* domain_id = UINT32_MAX = Cyclone DDS' "default domain id".*/
  if (domain_id >= UINT32_MAX) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: domain id out of range");
    return nullptr;
  }
  const dds_domainid_t did = static_cast<dds_domainid_t>(domain_id);
#else
  static_cast<void>(domain_id);
  const dds_domainid_t did = DDS_DOMAIN_DEFAULT;
#endif
#if !RMW_VERSION_GTE(0, 8, 2)
  (void) security_options;
#endif
  rmw_ret_t ret;
  int dummy_validation_result;
  size_t dummy_invalid_index;
  if ((ret =
    rmw_validate_node_name(name, &dummy_validation_result, &dummy_invalid_index)) != RMW_RET_OK)
  {
    return nullptr;
  }

#if SUPPORT_LOCALHOST
  /* Take domains_lock and hold it until after the participant creation succeeded or
     failed: otherwise there is a race with rmw_destroy_node deleting the last participant
     and tearing down the domain for versions of Cyclone that implement the original
     version of dds_create_domain that doesn't return a handle.  */
  std::lock_guard<std::mutex> lock(gcdds.domains_lock);
  if (!check_create_domain_locked(did, localhost_only)) {
    return nullptr;
  }
#endif

  dds_qos_t * qos = dds_create_qos();
#if RMW_VERSION_GTE(0, 8, 2)
  std::string user_data = get_node_user_data(name, namespace_, context->options.security_context);
#else
  std::string user_data = get_node_user_data(name, namespace_);
#endif
  dds_qset_userdata(qos, user_data.c_str(), user_data.size());
  dds_entity_t pp = dds_create_participant(did, qos, nullptr);
  dds_delete_qos(qos);
  if (pp < 0) {
    node_gone_from_domain_locked(did);
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DDS participant");
    return nullptr;
  }
#if SUPPORT_LOCALHOST
  if (gcdds.domains[did].domain_handle == 0) {
    /* Original implementation of dds_create_domain(): returned value is 0 (DDS_RETCODE_OK)
       and a reference to the domain is leaked, necessitating an explicit call to
       dds_delete().  For this, we must recover the handle from the participant.  Yikes!

       Pending fixes to Cyclone (https://github.com/eclipse-cyclonedds/cyclonedds/pull/304):
       a handle is returned by dds_create_domain() and dds_delete() must be called (so
       entirely in line with other entity types).  In this case, we don't have to do this
       (though there would be no harm in it either).

       Note that "domains_lock" is held all this time. */
    gcdds.domains[did].domain_handle = dds_get_parent(pp);
  }
#endif

  /* Since ROS2 doesn't require anything fancy from DDS Subscribers or Publishers, create a single
     pair & reuse that */
  dds_entity_t pub, sub;
  if ((pub = dds_create_publisher(pp, nullptr, nullptr)) < 0) {
    node_gone_from_domain_locked(did);
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DDS publisher");
    dds_delete(pp);
    return nullptr;
  }
  if ((sub = dds_create_subscriber(pp, nullptr, nullptr)) < 0) {
    node_gone_from_domain_locked(did);
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "rmw_create_node: failed to create DDS subscriber");
    dds_delete(pp);
    return nullptr;
  }

  auto * node_impl = new CddsNode();
  rmw_node_t * node_handle = nullptr;
  RET_ALLOC_X(node_impl, goto fail_node_impl);
  rmw_guard_condition_t * graph_guard_condition;
  if (!(graph_guard_condition = rmw_create_guard_condition(context))) {
    goto fail_ggc;
  }
  node_impl->enth = pp;
  node_impl->pub = pub;
  node_impl->sub = sub;
  node_impl->graph_guard_condition = graph_guard_condition;
  node_impl->domain_id = did;

#if MULTIDOMAIN
  if (!builtin_readers_init(node_impl->brd, pp, graph_guard_condition)) {
    goto fail_builtin_reader;
  }
#else
  {
    std::lock_guard<std::mutex> lock(gcdds.nodes_lock);
    gcdds.nodes.insert(node_impl);
  }
#endif

  node_handle = rmw_node_allocate();
  RET_ALLOC_X(node_handle, goto fail_node_handle);
  node_handle->implementation_identifier = eclipse_cyclonedds_identifier;
  node_handle->data = node_impl;

  node_handle->name = static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(name) + 1));
  RET_ALLOC_X(node_handle->name, goto fail_node_handle_name);
  memcpy(const_cast<char *>(node_handle->name), name, strlen(name) + 1);

  node_handle->namespace_ =
    static_cast<const char *>(rmw_allocate(sizeof(char) * strlen(namespace_) + 1));
  RET_ALLOC_X(node_handle->namespace_, goto fail_node_handle_namespace);
  memcpy(const_cast<char *>(node_handle->namespace_), namespace_, strlen(namespace_) + 1);
  return node_handle;

fail_node_handle_namespace:
  rmw_free(const_cast<char *>(node_handle->name));
fail_node_handle_name:
  rmw_node_free(node_handle);
fail_node_handle:
#if !MULTIDOMAIN
  {
    std::lock_guard<std::mutex> lock(gcdds.nodes_lock);
    gcdds.nodes.erase(node_impl);
  }
#else
  builtin_readers_fini(node_impl->brd);
fail_builtin_reader:
#endif
  if (RMW_RET_OK != rmw_destroy_guard_condition(graph_guard_condition)) {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp", "failed to destroy guard condition during error handling");
  }
fail_ggc:
  delete node_impl;
fail_node_impl:
  dds_delete(pp);
  node_gone_from_domain_locked(did);
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_node(rmw_node_t * node)
{
  rmw_ret_t result_ret = RMW_RET_OK;
  RET_WRONG_IMPLID(node);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL(node_impl);
  rmw_free(const_cast<char *>(node->name));
  rmw_free(const_cast<char *>(node->namespace_));
  rmw_node_free(node);

#if MULTIDOMAIN
  /* Must prevent the built-in topic listener from triggering before deleting the graph guard
     condition.  Deleting them first is the easiest. */
  builtin_readers_fini(node_impl->brd);
#else
  {
    std::lock_guard<std::mutex> lock(gcdds.nodes_lock);
    gcdds.nodes.erase(node_impl);
  }
#endif
  if (RMW_RET_OK != rmw_destroy_guard_condition(node_impl->graph_guard_condition)) {
    RMW_SET_ERROR_MSG("failed to destroy graph guard condition");
    result_ret = RMW_RET_ERROR;
  }
#if SUPPORT_LOCALHOST
  /* prevent race with rmw_create_node (see there) */
  std::lock_guard<std::mutex> lock(gcdds.domains_lock);
#endif
  if (dds_delete(node_impl->enth) < 0) {
    RMW_SET_ERROR_MSG("failed to destroy DDS participant");
    result_ret = RMW_RET_ERROR;
  }
  node_gone_from_domain_locked(node_impl->domain_id);
  delete node_impl;
  return result_ret;
}

extern "C" const rmw_guard_condition_t * rmw_node_get_graph_guard_condition(const rmw_node_t * node)
{
  RET_WRONG_IMPLID_X(node, return nullptr);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL_X(node_impl, return nullptr);
  return node_impl->graph_guard_condition;
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
  const rosidl_message_bounds_t * message_bounds, size_t * size)
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
#ifdef DDS_HAS_CREATE_TOPIC_GENERIC
  tp = dds_create_topic_generic(pp, &sertopic, nullptr, nullptr, nullptr);
#else
  tp = dds_create_topic_arbitrary(pp, sertopic, nullptr, nullptr, nullptr);
#endif
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
#ifndef DDS_HAS_CREATE_TOPIC_GENERIC
  if (tp > 0) {
    ddsi_sertopic_unref(sertopic);
  }
#endif
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
    return std::string(topic_name) + "__" + std::string(suffix);
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
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_NODE:
      dds_qset_liveliness(qos, DDS_LIVELINESS_MANUAL_BY_PARTICIPANT, ldur);
      break;
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC:
      dds_qset_liveliness(qos, DDS_LIVELINESS_MANUAL_BY_TOPIC, ldur);
      break;
  }
  if (ignore_local_publications) {
    dds_qset_ignorelocal(qos, DDS_IGNORELOCAL_PARTICIPANT);
  }
  return qos;
}

#if RMW_VERSION_GTE(0, 8, 2)
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
#endif

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
        qos_policies->liveliness = RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_NODE;
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
  const rmw_node_t * node,
  const rosidl_message_type_support_t * type_supports,
  const char * topic_name,
  const rmw_qos_profile_t * qos_policies)
{
  RET_WRONG_IMPLID_X(node, return nullptr);
  RET_NULL_OR_EMPTYSTR_X(topic_name, return nullptr);
  RET_NULL_X(qos_policies, return nullptr);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL_X(node_impl, return nullptr);
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
  topic = create_topic(node_impl->enth, sertopic, &stact);
  if (topic < 0) {
    RMW_SET_ERROR_MSG("failed to create topic");
    goto fail_topic;
  }
  if ((qos = create_readwrite_qos(qos_policies, false)) == nullptr) {
    goto fail_qos;
  }
  if ((pub->enth = dds_create_writer(node_impl->pub, topic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create writer");
    goto fail_writer;
  }
  if (dds_get_instance_handle(pub->enth, &pub->pubiid) < 0) {
    RMW_SET_ERROR_MSG("failed to get instance handle for writer");
    goto fail_instance_handle;
  }
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
#ifndef DDS_HAS_CREATE_TOPIC_GENERIC
  ddsi_sertopic_unref(stact);
#endif
fail_topic:
  delete pub;
  return nullptr;
}

extern "C" rmw_ret_t rmw_init_publisher_allocation(
  const rosidl_message_type_support_t * type_support,
  const rosidl_message_bounds_t * message_bounds, rmw_publisher_allocation_t * allocation)
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

extern "C" rmw_publisher_t * rmw_create_publisher(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies
#if RMW_VERSION_GTE(0, 8, 1)
  , const rmw_publisher_options_t * publisher_options
#endif
)
{
  CddsPublisher * pub;
  rmw_publisher_t * rmw_publisher;
#if RMW_VERSION_GTE(0, 8, 1)
  RET_NULL_X(publisher_options, return nullptr);
#endif
  if ((pub = create_cdds_publisher(node, type_supports, topic_name, qos_policies)) == nullptr) {
    goto fail_common_init;
  }
  rmw_publisher = rmw_publisher_allocate();
  RET_ALLOC_X(rmw_publisher, goto fail_publisher);
  rmw_publisher->implementation_identifier = eclipse_cyclonedds_identifier;
  rmw_publisher->data = pub;
  rmw_publisher->topic_name = reinterpret_cast<char *>(rmw_allocate(strlen(topic_name) + 1));
  RET_ALLOC_X(rmw_publisher->topic_name, goto fail_topic_name);
  memcpy(const_cast<char *>(rmw_publisher->topic_name), topic_name, strlen(topic_name) + 1);
#if RMW_VERSION_GTE(0, 8, 1)
  rmw_publisher->options = *publisher_options;
  rmw_publisher->can_loan_messages = false;
#endif
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

extern "C" rmw_ret_t rmw_destroy_publisher(rmw_node_t * node, rmw_publisher_t * publisher)
{
  RET_WRONG_IMPLID(node);
  RET_WRONG_IMPLID(publisher);
  auto pub = static_cast<CddsPublisher *>(publisher->data);
  if (pub != nullptr) {
    if (dds_delete(pub->enth) < 0) {
      RMW_SET_ERROR_MSG("failed to delete writer");
    }
#ifndef DDS_HAS_CREATE_TOPIC_GENERIC
    ddsi_sertopic_unref(pub->sertopic);
#endif
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

static CddsSubscription * create_cdds_subscription(
  const rmw_node_t * node,
  const rosidl_message_type_support_t * type_supports, const char * topic_name,
  const rmw_qos_profile_t * qos_policies, bool ignore_local_publications)
{
  RET_WRONG_IMPLID_X(node, return nullptr);
  RET_NULL_OR_EMPTYSTR_X(topic_name, return nullptr);
  RET_NULL_X(qos_policies, return nullptr);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL_X(node_impl, return nullptr);
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
  topic = create_topic(node_impl->enth, sertopic);
  if (topic < 0) {
    RMW_SET_ERROR_MSG("failed to create topic");
    goto fail_topic;
  }
  if ((qos = create_readwrite_qos(qos_policies, ignore_local_publications)) == nullptr) {
    goto fail_qos;
  }
  if ((sub->enth = dds_create_reader(node_impl->sub, topic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create reader");
    goto fail_reader;
  }
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
  const rosidl_message_bounds_t * message_bounds, rmw_subscription_allocation_t * allocation)
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

extern "C" rmw_subscription_t * rmw_create_subscription(
  const rmw_node_t * node, const rosidl_message_type_support_t * type_supports,
  const char * topic_name, const rmw_qos_profile_t * qos_policies
#if RMW_VERSION_GTE(0, 8, 1)
  , const rmw_subscription_options_t * subscription_options
#else
  , bool ignore_local_publications
#endif
)
{
  CddsSubscription * sub;
  rmw_subscription_t * rmw_subscription;
#if RMW_VERSION_GTE(0, 8, 1)
  RET_NULL_X(subscription_options, return nullptr);
#endif
  if (
    (sub = create_cdds_subscription(
      node, type_supports, topic_name, qos_policies,
#if RMW_VERSION_GTE(0, 8, 1)
      subscription_options->ignore_local_publications
#else
      ignore_local_publications
#endif
    )) == nullptr)
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
#if RMW_VERSION_GTE(0, 8, 1)
  rmw_subscription->options = *subscription_options;
  rmw_subscription->can_loan_messages = false;
#endif
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

extern "C" rmw_ret_t rmw_destroy_subscription(rmw_node_t * node, rmw_subscription_t * subscription)
{
  RET_WRONG_IMPLID(node);
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
      if (message_info) {
        message_info->publisher_gid.implementation_identifier = eclipse_cyclonedds_identifier;
        memset(message_info->publisher_gid.data, 0, sizeof(message_info->publisher_gid.data));
        assert(sizeof(info.publication_handle) <= sizeof(message_info->publisher_gid.data));
        memcpy(
          message_info->publisher_gid.data, &info.publication_handle,
          sizeof(info.publication_handle));
      }
#if REPORT_LATE_MESSAGES > 0
      dds_time_t tnow = dds_time();
      dds_time_t dt = tnow - info.source_timestamp;
      if (dt >= DDS_MSECS(REPORT_LATE_MESSAGES)) {
        fprintf(stderr, "** sample in history for %.fms\n", static_cast<double>(dt) / 1e6);
      }
#endif
      *taken = true;
      return RMW_RET_OK;
    }
  }
  *taken = false;
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
#if RMW_VERSION_GTE(0, 8, 2)
  {RMW_EVENT_REQUESTED_QOS_INCOMPATIBLE, DDS_REQUESTED_INCOMPATIBLE_QOS_STATUS},
  {RMW_EVENT_OFFERED_QOS_INCOMPATIBLE, DDS_OFFERED_INCOMPATIBLE_QOS_STATUS},
#endif
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

#if RMW_VERSION_GTE(0, 8, 2)
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
#endif

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

#if RMW_VERSION_GTE(0, 8, 2)
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
#endif

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

extern "C" rmw_guard_condition_t * rmw_create_guard_condition(rmw_context_t * context)
{
  static_cast<void>(context);
  rmw_guard_condition_t * guard_condition_handle;
  auto * gcond_impl = new CddsGuardCondition();
#if MULTIDOMAIN
  const dds_entity_t owner = DDS_CYCLONEDDS_HANDLE;
#else
  const dds_entity_t owner = ref_ppant();
  if (owner < 0) {
    goto fail_ppant;
  }
#endif
  if ((gcond_impl->gcondh = dds_create_guardcondition(owner)) < 0) {
    RMW_SET_ERROR_MSG("failed to create guardcondition");
    goto fail_guardcond;
  }
  guard_condition_handle = new rmw_guard_condition_t;
  guard_condition_handle->implementation_identifier = eclipse_cyclonedds_identifier;
  guard_condition_handle->data = gcond_impl;
  return guard_condition_handle;

fail_guardcond:
#if !MULTIDOMAIN
  unref_ppant();
fail_ppant:
#endif
  delete (gcond_impl);
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_guard_condition(rmw_guard_condition_t * guard_condition_handle)
{
  RET_NULL(guard_condition_handle);
  auto * gcond_impl = static_cast<CddsGuardCondition *>(guard_condition_handle->data);
  clean_waitset_caches();
  dds_delete(gcond_impl->gcondh);
  delete gcond_impl;
  delete guard_condition_handle;
  return RMW_RET_OK;
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
  static_cast<void>(context);
  (void) max_conditions;
  rmw_wait_set_t * wait_set = rmw_wait_set_allocate();
  CddsWaitset * ws = nullptr;
  dds_entity_t owner = 0;
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
#if MULTIDOMAIN
  owner = DDS_CYCLONEDDS_HANDLE;
#else
  owner = ref_ppant();
  if (owner < 0) {
    goto fail_ppant;
  }
#endif

  if ((ws->waitseth = dds_create_waitset(owner)) < 0) {
    RMW_SET_ERROR_MSG("failed to create waitset");
    goto fail_waitset;
  }

  {
    std::lock_guard<std::mutex> lock(gcdds.lock);
#if MULTIDOMAIN
    // Lazily create dummy guard condition
    if (gcdds.waitsets.size() == 0) {
      if ((gcdds.gc_for_empty_waitset = dds_create_guardcondition(owner)) < 0) {
        RMW_SET_ERROR_MSG("failed to create guardcondition for handling empty waitsets");
        goto fail_create_dummy;
      }
    }
#endif
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
#if MULTIDOMAIN
fail_create_dummy:
#endif
  dds_delete(ws->waitseth);
fail_waitset:
#if !MULTIDOMAIN
  unref_ppant();
fail_ppant:
#endif
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
#if MULTIDOMAIN
    if (gcdds.waitsets.size() == 0) {
      dds_delete(gcdds.gc_for_empty_waitset);
      gcdds.gc_for_empty_waitset = 0;
    }
#endif
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
    std::lock_guard<std::mutex> lock(ws->lock);
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
  CddsCS * cs, rmw_request_id_t * request_header,
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
      assert(sizeof(wrap.header.guid) <= sizeof(request_header->writer_guid));
      memcpy(request_header->writer_guid, &wrap.header.guid, sizeof(wrap.header.guid));
      request_header->sequence_number = wrap.header.seq;
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
  rmw_request_id_t * request_header, void * ros_response,
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
  rmw_request_id_t * request_header, void * ros_request,
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
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL(node_impl);
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
  pubtopic = create_topic(node_impl->enth, pub_st, &pub_stact);
  if (pubtopic < 0) {
    RMW_SET_ERROR_MSG("failed to create topic");
    goto fail_pubtopic;
  }

  sub_st = create_sertopic(
    subtopic_name.c_str(), type_support->typesupport_identifier, sub_type_support, true,
    std::move(sub_msg_ts));
  subtopic = create_topic(node_impl->enth, sub_st);
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
  if ((pub->enth = dds_create_writer(node_impl->pub, pubtopic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create writer");
    goto fail_writer;
  }
  pub->sertopic = pub_stact;
  if ((sub->enth = dds_create_reader(node_impl->sub, subtopic, qos, nullptr)) < 0) {
    RMW_SET_ERROR_MSG("failed to create reader");
    goto fail_reader;
  }
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
#ifndef DDS_HAS_CREATE_TOPIC_GENERIC
  ddsi_sertopic_unref(pub_stact);
#endif
fail_pubtopic:
  return RMW_RET_ERROR;
}

static void rmw_fini_cs(CddsCS * cs)
{
#ifndef DDS_HAS_CREATE_TOPIC_GENERIC
  ddsi_sertopic_unref(cs->pub->sertopic);
#endif
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
  return rmw_client;
fail_service_name:
  rmw_client_free(rmw_client);
fail_client:
  rmw_fini_cs(&info->client);
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_client(rmw_node_t * node, rmw_client_t * client)
{
  RET_WRONG_IMPLID(node);
  RET_WRONG_IMPLID(client);
  auto info = static_cast<CddsClient *>(client->data);
  clean_waitset_caches();
  rmw_fini_cs(&info->client);
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
  return rmw_service;
fail_service_name:
  rmw_service_free(rmw_service);
fail_service:
  rmw_fini_cs(&info->service);
  return nullptr;
}

extern "C" rmw_ret_t rmw_destroy_service(rmw_node_t * node, rmw_service_t * service)
{
  RET_WRONG_IMPLID(node);
  RET_WRONG_IMPLID(service);
  auto info = static_cast<CddsService *>(service->data);
  clean_waitset_caches();
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

enum topic_kind
{
  DEFAULT,
  REQUEST_RESPONSE
};

static bool demangle_topic_name(
  const char * topic_name,
  topic_kind topic_kind,
  std::string & demangled_topic_name,
  bool * is_request)
{
  std::string re;
  bool is_def_kind = topic_kind == topic_kind::DEFAULT;
  if (is_def_kind) {
    re = "^" + std::string(ROS_TOPIC_PREFIX) + "(/.*)";
  } else {
    re = "^(" + std::string(ROS_SERVICE_REQUESTER_PREFIX) + "|" +
      std::string(ROS_SERVICE_RESPONSE_PREFIX) + ")(/.*)(Request|Reply)$";
  }
  const auto re_tp = std::regex(re, std::regex::extended);
  std::cmatch cm_tp;
  if (std::regex_search(topic_name, cm_tp, re_tp)) {
    demangled_topic_name = std::string(cm_tp[is_def_kind ? 1 : 2]);
    if (is_request) {
      assert(!is_def_kind);
      *is_request = (std::string(cm_tp[3]) == "Request");
    }
    return true;
  }
  return false;
}

static bool demangle_topic_name(
  const char * topic_name,
  topic_kind topic_kind,
  std::string & demangled_topic_name)
{
  return demangle_topic_name(topic_name, topic_kind, demangled_topic_name, NULL);
}

static bool demangle_topic_type(
  const char * type_name,
  topic_kind topic_kind,
  std::string & demangled_type_name)
{
  bool is_def_kind = topic_kind == topic_kind::DEFAULT;
  const auto re_typ = std::regex(
    "^(.*::)dds_::(.*)_" +
    (is_def_kind ? std::string() : std::string("(Response|Request)_")) +
    std::string("$"),
    std::regex::extended);
  std::cmatch cm_typ;
  if (std::regex_search(type_name, cm_typ, re_typ)) {
    demangled_type_name =
      std::regex_replace(std::string(cm_typ[1]), std::regex("::"), "/") +
      std::string(cm_typ[2]);
    return true;
  }
  return false;
}

static rmw_ret_t do_for_node(
  const CddsNode * node_impl,
  std::function<bool(const dds_builtintopic_participant_t & sample)> oper)
{
  dds_entity_t rd;
  if (
    (rd = dds_create_reader(
      node_impl->enth, DDS_BUILTIN_TOPIC_DCPSPARTICIPANT, NULL, NULL)) < 0)
  {
    RMW_SET_ERROR_MSG("rmw_get_node_names: failed to create reader");
    return RMW_RET_ERROR;
  }
  dds_sample_info_t info;
  void * msg = NULL;
  int32_t n;
  bool cont = true;
  while (cont && (n = dds_take(rd, &msg, &info, 1, 1)) == 1) {
    if (info.valid_data && info.instance_state == DDS_IST_ALIVE) {
      auto sample = static_cast<const dds_builtintopic_participant_t *>(msg);
      cont = oper(*sample);
    }
    dds_return_loan(rd, &msg, n);
  }
  dds_delete(rd);
  if (n < 0) {
    RMW_SET_ERROR_MSG("rmw_get_node_names: error reading participants");
    return RMW_RET_ERROR;
  }
  return RMW_RET_OK;
}

static rmw_ret_t do_for_node_user_data(
  const CddsNode * node_impl,
  std::function<bool(const dds_builtintopic_participant_t & sample, const char * user_data)> oper)
{
  auto f = [oper](const dds_builtintopic_participant_t & sample) -> bool {
      void * ud;
      size_t udsz;
      if (dds_qget_userdata(sample.qos, &ud, &udsz) && ud != nullptr) {
        /* CycloneDDS guarantees a null-terminated user data so we pretend it's a
           string */
        bool ret = oper(sample, static_cast<char *>(ud));
        dds_free(ud);
        return ret;
      } else {
        /* If no user data present treat it as an empty string */
        return oper(sample, "");
      }
    };
  return do_for_node(node_impl, f);
}

extern "C" rmw_ret_t rmw_get_node_names_impl(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces,
  rcutils_string_array_t * security_contexts)
{
  RET_WRONG_IMPLID(node);
  auto node_impl = static_cast<CddsNode *>(node->data);
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(node_names) ||
    RMW_RET_OK != rmw_check_zero_rmw_string_array(node_namespaces))
  {
    return RMW_RET_ERROR;
  }

  std::regex re {
    "^name=([^;]*);namespace=([^;]*);(securitycontext=([^;]*);)?",
    std::regex_constants::extended
  };
  std::vector<std::tuple<std::string, std::string, std::string>> ns;
  auto oper =
    [&ns, re](const dds_builtintopic_participant_t & sample, const char * ud) -> bool {
      std::cmatch cm;
      static_cast<void>(sample);
      if (std::regex_search(ud, cm, re)) {
        ns.push_back(std::make_tuple(std::string(cm[1]), std::string(cm[2]), std::string(cm[4])));
      }
      return true;
    };
  rmw_ret_t ret;
  if ((ret = do_for_node_user_data(node_impl, oper)) != RMW_RET_OK) {
    return ret;
  }

  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  if (rcutils_string_array_init(node_names, ns.size(), &allocator) != RCUTILS_RET_OK ||
    rcutils_string_array_init(node_namespaces, ns.size(), &allocator) != RCUTILS_RET_OK)
  {
    RMW_SET_ERROR_MSG(rcutils_get_error_string().str);
    goto fail_alloc;
  }
  if (security_contexts &&
    rcutils_string_array_init(security_contexts, ns.size(), &allocator) != RCUTILS_RET_OK)
  {
    RMW_SET_ERROR_MSG(rcutils_get_error_string().str);
    goto fail_alloc;
  }
  {
    size_t i = 0;
    for (auto & n : ns) {
      node_names->data[i] = rcutils_strdup(std::get<0>(n).c_str(), allocator);
      node_namespaces->data[i] = rcutils_strdup(std::get<1>(n).c_str(), allocator);
      if (!node_names->data[i] || !node_namespaces->data[i]) {
        RMW_SET_ERROR_MSG("rmw_get_node_names for name/namespace");
        goto fail_alloc;
      }
      if (security_contexts) {
        security_contexts->data[i] = rcutils_strdup(std::get<2>(n).c_str(), allocator);
        if (!security_contexts->data[i]) {
          RMW_SET_ERROR_MSG("rmw_get_node_names for security_context");
          goto fail_alloc;
        }
      }
      i++;
    }
  }
  return RMW_RET_OK;

fail_alloc:
  if (node_names) {
    if (rcutils_string_array_fini(node_names) != RCUTILS_RET_OK) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "failed to cleanup during error handling: %s", rcutils_get_error_string().str);
      rcutils_reset_error();
    }
  }
  if (node_namespaces) {
    if (rcutils_string_array_fini(node_namespaces) != RCUTILS_RET_OK) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "failed to cleanup during error handling: %s", rcutils_get_error_string().str);
      rcutils_reset_error();
    }
  }
  if (security_contexts) {
    if (rcutils_string_array_fini(security_contexts) != RCUTILS_RET_OK) {
      RCUTILS_LOG_ERROR_NAMED(
        "rmw_cyclonedds_cpp",
        "failed to cleanup during error handling: %s", rcutils_get_error_string().str);
      rcutils_reset_error();
    }
  }
  return RMW_RET_BAD_ALLOC;
}

extern "C" rmw_ret_t rmw_get_node_names(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces)
{
  return rmw_get_node_names_impl(
    node,
    node_names,
    node_namespaces,
    nullptr);
}

#if RMW_VERSION_GTE(0, 8, 2)
extern "C" rmw_ret_t rmw_get_node_names_with_security_contexts(
  const rmw_node_t * node,
  rcutils_string_array_t * node_names,
  rcutils_string_array_t * node_namespaces,
  rcutils_string_array_t * security_contexts)
{
  if (RMW_RET_OK != rmw_check_zero_rmw_string_array(security_contexts)) {
    return RMW_RET_ERROR;
  }
  return rmw_get_node_names_impl(
    node,
    node_names,
    node_namespaces,
    security_contexts);
}
#endif

static rmw_ret_t rmw_collect_data_for_endpoint(
  CddsNode * node_impl,
  dds_entity_t builtin_topic,
  std::function<void(const dds_builtintopic_endpoint_t & sample)> filter_and_map)
{
  assert(
    builtin_topic == DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION ||
    builtin_topic == DDS_BUILTIN_TOPIC_DCPSPUBLICATION);
  dds_entity_t rd;
  if ((rd = dds_create_reader(node_impl->enth, builtin_topic, NULL, NULL)) < 0) {
    RMW_SET_ERROR_MSG("rmw_collect_data_for_endpoint failed to create reader");
    return RMW_RET_ERROR;
  }
  dds_sample_info_t info;
  void * msg = NULL;
  int32_t n;
  while ((n = dds_take(rd, &msg, &info, 1, 1)) == 1) {
    if (info.valid_data && info.instance_state == DDS_IST_ALIVE) {
      auto sample = static_cast<const dds_builtintopic_endpoint_t *>(msg);
      filter_and_map(*sample);
    }
    dds_return_loan(rd, &msg, n);
  }
  dds_delete(rd);
  if (n == 0) {
    return RMW_RET_OK;
  } else {
    RMW_SET_ERROR_MSG("rmw_collect_data_for_endpoint dds_take failed");
    return RMW_RET_ERROR;
  }
}

#if RMW_VERSION_GTE(0, 8, 2)
struct endpoint_info_t
{
  std::string topic_type;
  dds_builtintopic_guid_t endpoint_key;
  dds_builtintopic_guid_t node_key;
  dds_instance_handle_t node_insth;
  rmw_qos_profile_t qos_profile;
};

typedef std::map<dds_builtintopic_guid_t, std::pair<std::string, std::string>> node_cache_t;

static void get_node_name(
  dds_entity_t ppant_rd,
  const dds_builtintopic_guid_t & pp_guid,
  const dds_instance_handle_t pp_insth,
  std::string & node_name,
  std::string & node_ns)
{
  static_cast<void>(pp_guid);  // only used in assert()
  bool node_found = false;
  const auto re_ud = std::regex("^name=([^;]*);namespace=([^;]*);", std::regex::extended);
  size_t udsz;
  dds_sample_info_t info;
  void * msg = NULL;
  int32_t n;
  if ((n = dds_read_instance(ppant_rd, &msg, &info, 1, 1, pp_insth)) == 1) {
    void * ud;
    auto sample = static_cast<const dds_builtintopic_participant_t *>(msg);
    assert(!memcmp(&sample->key, &pp_guid, sizeof(dds_builtintopic_guid_t)));
    if (dds_qget_userdata(sample->qos, &ud, &udsz) && ud != nullptr) {
      std::cmatch cm;
      if (std::regex_search(reinterpret_cast<const char *>(ud), cm, re_ud)) {
        node_found = true;
        node_name = std::string(cm[1]);
        node_ns = std::string(cm[2]);
      }
    }
    dds_return_loan(ppant_rd, &msg, n);
  }
  if (!node_found) {
    node_name = "_NODE_NAME_UNKNOWN_";
    node_ns = "_NODE_NAMESPACE_UNKNOWN_";
  }
}

static rmw_ret_t
set_rmw_topic_endpoint_info(
  node_cache_t & node_cache,
  dds_entity_t ppant_rd,
  rcutils_allocator_t * allocator,
  const endpoint_info_t & epinfo,
  bool is_publisher,
  rmw_topic_endpoint_info_t & topic_endpoint_info)
{
  static_assert(
    sizeof(dds_builtintopic_guid_t) <= RMW_GID_STORAGE_SIZE,
    "RMW_GID_STORAGE_SIZE insufficient to store the rmw_cyclonedds_cpp GID implementation."
  );
  rmw_ret_t ret;
  std::string node_name;
  std::string node_ns;
  const auto it = node_cache.find(epinfo.node_key);
  if (it != node_cache.end()) {
    node_name = it->second.first;
    node_ns = it->second.second;
  } else {
    get_node_name(ppant_rd, epinfo.node_key, epinfo.node_insth, node_name, node_ns);
    node_cache[epinfo.node_key] = std::make_pair(node_name, node_ns);
  }
  if ((ret = rmw_topic_endpoint_info_set_node_name(
      &topic_endpoint_info,
      node_name.c_str(),
      allocator)) != RMW_RET_OK)
  {
    return ret;
  }
  if ((ret = rmw_topic_endpoint_info_set_node_namespace(
      &topic_endpoint_info,
      node_ns.c_str(),
      allocator)) != RMW_RET_OK)
  {
    return ret;
  }
  if ((ret = rmw_topic_endpoint_info_set_topic_type(
      &topic_endpoint_info,
      epinfo.topic_type.c_str(),
      allocator)) != RMW_RET_OK)
  {
    return ret;
  }
  if ((ret = rmw_topic_endpoint_info_set_endpoint_type(
      &topic_endpoint_info,
      is_publisher ? RMW_ENDPOINT_PUBLISHER : RMW_ENDPOINT_SUBSCRIPTION)) != RMW_RET_OK)
  {
    return ret;
  }
  if ((ret = rmw_topic_endpoint_info_set_gid(
      &topic_endpoint_info,
      epinfo.endpoint_key.v,
      sizeof(dds_builtintopic_guid_t))) != RMW_RET_OK)
  {
    return ret;
  }
  if ((ret = rmw_topic_endpoint_info_set_qos_profile(
      &topic_endpoint_info,
      &epinfo.qos_profile)) != RMW_RET_OK)
  {
    return ret;
  }
  return RMW_RET_OK;
}

static void handle_topic_endpoint_info_array_fini(
  rmw_topic_endpoint_info_array_t * topic_endpoint_info_array,
  rcutils_allocator_t * allocator)
{
  std::string err_msg;
  rmw_ret_t ret;
  bool err_set = rmw_error_is_set();
  if (err_set) {
    err_msg = rmw_get_error_string().str;
  }
  rmw_reset_error();
  if ((ret =
    rmw_topic_endpoint_info_array_fini(topic_endpoint_info_array, allocator)) != RMW_RET_OK)
  {
    RCUTILS_LOG_ERROR_NAMED(
      "rmw_cyclonedds_cpp",
      "handle_topic_endpoint_info_array_fini failed: %s",
      rmw_get_error_string().str);
    rmw_reset_error();
  }
  if (err_set) {
    RMW_SET_ERROR_MSG(err_msg.c_str());
  }
}

static rmw_ret_t
get_endpoint_info_by_topic(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * topic_name,
  bool no_mangle,
  bool is_publisher,
  rmw_topic_endpoint_info_array_t * endpoint_info)
{
  auto node_impl = static_cast<CddsNode *>(node->data);
  const std::string fqtopic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", no_mangle);
  std::vector<endpoint_info_t> endpoint_info_vector;
  rmw_ret_t ret;

  const auto filter_and_map =
    [fqtopic_name, no_mangle, &endpoint_info_vector](
    const dds_builtintopic_endpoint_t & sample) -> void
    {
      endpoint_info_t info;
      if (std::string(sample.topic_name) == fqtopic_name) {
        info.endpoint_key = sample.key;
        dds_qos_to_rmw_qos(sample.qos, &info.qos_profile);
        if (no_mangle) {
          info.topic_type = std::string(sample.type_name);
        } else if (!demangle_topic_type(sample.type_name, topic_kind::DEFAULT, info.topic_type)) {
          // skip a non-ROS2 endpoint
          return;
        }
        info.node_key = sample.participant_key;
        info.node_insth = sample.participant_instance_handle;
        endpoint_info_vector.push_back(info);
      }
    };
  if ((ret = rmw_collect_data_for_endpoint(
      node_impl,
      is_publisher ? DDS_BUILTIN_TOPIC_DCPSPUBLICATION : DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION,
      filter_and_map)) != RMW_RET_OK)
  {
    return ret;
  }

  size_t epi_count = endpoint_info_vector.size();
  if ((ret = rmw_topic_endpoint_info_array_init_with_size(
      endpoint_info,
      epi_count,
      allocator)) != RMW_RET_OK)
  {
    rmw_error_string_t err_msg = rmw_get_error_string();
    rmw_reset_error();
    RMW_SET_ERROR_MSG_WITH_FORMAT_STRING(
      "rmw_topic_endpoint_info_array_init_with_size failed to allocate memory: %s",
      err_msg.str);
    return ret;
  }

  node_cache_t node_cache;
  dds_entity_t ppant_rd;
  if (
    (ppant_rd = dds_create_reader(
      node_impl->enth, DDS_BUILTIN_TOPIC_DCPSPARTICIPANT, NULL, NULL)) < 0)
  {
    RMW_SET_ERROR_MSG("get_endpoint_info_by_topic: failed to create reader");
    handle_topic_endpoint_info_array_fini(endpoint_info, allocator);
    return RMW_RET_ERROR;
  }
  size_t i = 0;
  for (const auto & epi : endpoint_info_vector) {
    if ((ret = set_rmw_topic_endpoint_info(
        node_cache,
        ppant_rd,
        allocator,
        epi,
        is_publisher,
        endpoint_info->info_array[i])) != RMW_RET_OK)
    {
      handle_topic_endpoint_info_array_fini(endpoint_info, allocator);
      dds_delete(ppant_rd);
      return ret;
    }
    i++;
  }
  assert(i == epi_count);
  dds_delete(ppant_rd);
  return RMW_RET_OK;
}
#endif  // RMW_VERSION_GTE(0, 8, 2)

static rmw_ret_t make_names_and_types(
  rmw_names_and_types_t * tptyp, const std::map<std::string,
  std::set<std::string>> & source,
  rcutils_allocator_t * allocator)
{
  if (source.size() == 0) {
    return RMW_RET_OK;
  }
  rmw_ret_t ret;
  if ((ret = rmw_names_and_types_init(tptyp, source.size(), allocator)) != RMW_RET_OK) {
    return ret;
  }
  size_t index = 0;
  for (const auto & tp : source) {
    if ((tptyp->names.data[index] = rcutils_strdup(tp.first.c_str(), *allocator)) == NULL) {
      goto fail_mem;
    }
    if (
      rcutils_string_array_init(
        &tptyp->types[index], tp.second.size(), allocator) != RCUTILS_RET_OK)
    {
      goto fail_mem;
    }
    size_t type_index = 0;
    for (const auto & type : tp.second) {
      if ((tptyp->types[index].data[type_index] =
        rcutils_strdup(type.c_str(), *allocator)) == NULL)
      {
        goto fail_mem;
      }
      type_index++;
    }
    index++;
  }
  return RMW_RET_OK;

fail_mem:
  if (rmw_names_and_types_fini(tptyp) != RMW_RET_OK) {
    RMW_SET_ERROR_MSG("make_names_and_types: rmw_names_and_types_fini failed");
  }
  return RMW_RET_BAD_ALLOC;
}

static rmw_ret_t get_node_guids(
  CddsNode * node_impl,
  const char * node_name, const char * node_namespace,
  std::set<dds_builtintopic_guid_t> & guids)
{
  std::string needle = get_node_user_data_name_ns(node_name, node_namespace);
  auto oper =
    [&guids, needle](const dds_builtintopic_participant_t & sample, const char * ud) -> bool {
      if (0u == std::string(ud).find(needle)) {
        guids.insert(sample.key);
      }
      return true;               /* do keep looking - what if there are many? */
    };
  rmw_ret_t ret = do_for_node_user_data(node_impl, oper);
  if (ret != RMW_RET_OK) {
    return ret;
  } else if (guids.size() == 0) {
    /* It appears the get_..._by_node operations are supposed to return
       NODE_NAME_NON_EXISTENT (newly introduced in Eloquent) or ERROR
       (on Dashing and earlier) if no such node exists */
#ifdef RMW_RET_NODE_NAME_NON_EXISTENT
    return RMW_RET_NODE_NAME_NON_EXISTENT;
#else
    return RMW_RET_ERROR;
#endif
  } else {
    return RMW_RET_OK;
  }
}

static rmw_ret_t get_endpoint_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace, bool no_demangle,
  rmw_names_and_types_t * tptyp, bool subs,
  bool pubs)
{
  RET_WRONG_IMPLID(node);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL(allocator);
  rmw_ret_t ret = rmw_names_and_types_check_zero(tptyp);
  if (ret != RMW_RET_OK) {
    return ret;
  }
  if (node_name) {
    int dummy_validation_result;
    size_t dummy_invalid_index;
    if ((ret =
      rmw_validate_node_name(
        node_name, &dummy_validation_result, &dummy_invalid_index)) != RMW_RET_OK)
    {
      return ret;
    }
  }
  std::set<dds_builtintopic_guid_t> guids;
  if (node_name != nullptr &&
    (ret = get_node_guids(node_impl, node_name, node_namespace, guids)) != RMW_RET_OK)
  {
    return ret;
  }
  std::map<std::string, std::set<std::string>> tt;
  const auto filter_and_map =
    [&tt, guids, node_name, no_demangle](
    const dds_builtintopic_endpoint_t & sample) -> void
    {
      std::string demangled_topic_name, demangled_type_name;
      if (
        (node_name == nullptr || guids.count(sample.participant_key) != 0) &&
        demangle_topic_name(sample.topic_name, topic_kind::DEFAULT, demangled_topic_name) &&
        demangle_topic_type(sample.type_name, topic_kind::DEFAULT, demangled_type_name))
      {
        tt[demangled_topic_name].insert(
          no_demangle ? std::string(sample.type_name) : demangled_type_name);
      }
    };
  if (subs &&
    (ret =
    rmw_collect_data_for_endpoint(
      node_impl, DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION, filter_and_map)) != RMW_RET_OK)
  {
    return ret;
  }
  if (pubs &&
    (ret =
    rmw_collect_data_for_endpoint(
      node_impl, DDS_BUILTIN_TOPIC_DCPSPUBLICATION, filter_and_map)) != RMW_RET_OK)
  {
    return ret;
  }
  return make_names_and_types(tptyp, tt, allocator);
}

static bool endpoint_is_from_service(bool is_request, const dds_builtintopic_guid_t & guid)
{
  /* Final byte of GUID is entity type, application writers use 0x02 (write with key) and 0x03
     (writer without key); readers use 0x04 and 0x07.  See DDSI specification, table 9.1.

     A service has a reader for a request and a writer for the reply; a client has it the other
     way round.  Therefore, it is a service iff is_request is set and the GUID indicates it is a
     writer. */
  if (is_request) {
    return guid.v[15] == 0x04 || guid.v[15] == 0x07;
  } else {
    return guid.v[15] == 0x02 || guid.v[15] == 0x03;
  }
}

static rmw_ret_t get_cs_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name, const char * node_namespace,
  rmw_names_and_types_t * sntyp,
  bool looking_for_services)
{
  RET_WRONG_IMPLID(node);
  auto node_impl = static_cast<CddsNode *>(node->data);
  RET_NULL(allocator);
  rmw_ret_t ret = rmw_names_and_types_check_zero(sntyp);
  if (ret != RMW_RET_OK) {
    return ret;
  }
  if (node_name) {
    int dummy_validation_result;
    size_t dummy_invalid_index;
    if ((ret =
      rmw_validate_node_name(
        node_name, &dummy_validation_result, &dummy_invalid_index)) != RMW_RET_OK)
    {
      return ret;
    }
  }
  std::set<dds_builtintopic_guid_t> guids;
  if (
    node_name != nullptr &&
    (ret = get_node_guids(node_impl, node_name, node_namespace, guids)) != RMW_RET_OK)
  {
    return ret;
  }
  std::map<std::string, std::set<std::string>> tt;
  const auto filter_and_map = [&tt, guids, node_name, looking_for_services](
    const dds_builtintopic_endpoint_t & sample) -> void {
      std::string topic_name, type_name;
      bool is_request;
      if ((node_name == nullptr || guids.count(sample.participant_key) != 0) &&
        demangle_topic_name(
          sample.topic_name, topic_kind::REQUEST_RESPONSE, topic_name, &is_request) &&
        demangle_topic_type(sample.type_name, topic_kind::REQUEST_RESPONSE, type_name) &&
        looking_for_services == endpoint_is_from_service(is_request, sample.key))
      {
        tt[topic_name].insert(type_name);
      }
    };
  if ((ret =
    rmw_collect_data_for_endpoint(
      node_impl, DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION, filter_and_map)) != RMW_RET_OK ||
    (ret =
    rmw_collect_data_for_endpoint(
      node_impl, DDS_BUILTIN_TOPIC_DCPSPUBLICATION, filter_and_map)) != RMW_RET_OK)
  {
    return ret;
  }
  return make_names_and_types(sntyp, tt, allocator);
}

extern "C" rmw_ret_t rmw_get_topic_names_and_types(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  bool no_demangle, rmw_names_and_types_t * tptyp)
{
  return get_endpoint_names_and_types_by_node(
    node, allocator, nullptr, nullptr, no_demangle, tptyp, true, true);
}

extern "C" rmw_ret_t rmw_get_service_names_and_types(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  rmw_names_and_types_t * sntyp)
{
  return get_cs_names_and_types_by_node(node, allocator, nullptr, nullptr, sntyp, true);
}

extern "C" rmw_ret_t rmw_service_server_is_available(
  const rmw_node_t * node,
  const rmw_client_t * client,
  bool * is_available)
{
  RET_WRONG_IMPLID(node);
  RET_WRONG_IMPLID(client);
  RET_NULL(is_available);
  auto info = static_cast<CddsClient *>(client->data);
  dds_publication_matched_status_t ps;
  dds_subscription_matched_status_t cs;
  if (dds_get_publication_matched_status(info->client.pub->enth, &ps) < 0 ||
    dds_get_subscription_matched_status(info->client.sub->enth, &cs) < 0)
  {
    RMW_SET_ERROR_MSG("rmw_service_server_is_available: get_..._matched_status failed");
    return RMW_RET_ERROR;
  }
  *is_available = ps.current_count > 0 && cs.current_count > 0;
  return RMW_RET_OK;
}

static rmw_ret_t rmw_count_pubs_or_subs(
  const rmw_node_t * node, dds_entity_t builtin_topic,
  const char * topic_name, size_t * count)
{
  assert(
    builtin_topic == DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION ||
    builtin_topic == DDS_BUILTIN_TOPIC_DCPSPUBLICATION);
  RET_NULL(topic_name);
  RET_NULL(count);
  RET_WRONG_IMPLID(node);
  auto node_impl = static_cast<CddsNode *>(node->data);

  std::string fqtopic_name = make_fqtopic(ROS_TOPIC_PREFIX, topic_name, "", false);
  dds_entity_t rd;
  if ((rd = dds_create_reader(node_impl->enth, builtin_topic, NULL, NULL)) < 0) {
    RMW_SET_ERROR_MSG("rmw_count_pubs_or_subs failed to create reader");
    return RMW_RET_ERROR;
  }
  dds_sample_info_t info;
  void * msg = NULL;
  int32_t n;
  *count = 0;
  while ((n = dds_take(rd, &msg, &info, 1, 1)) == 1) {
    if (info.valid_data && info.instance_state == DDS_IST_ALIVE) {
      auto sample = static_cast<const dds_builtintopic_endpoint_t *>(msg);
      if (fqtopic_name == std::string(sample->topic_name)) {
        (*count)++;
      }
    }
    dds_return_loan(rd, &msg, n);
  }
  dds_delete(rd);
  return RMW_RET_OK;
}

extern "C" rmw_ret_t rmw_count_publishers(
  const rmw_node_t * node, const char * topic_name,
  size_t * count)
{
  return rmw_count_pubs_or_subs(node, DDS_BUILTIN_TOPIC_DCPSPUBLICATION, topic_name, count);
}

extern "C" rmw_ret_t rmw_count_subscribers(
  const rmw_node_t * node, const char * topic_name,
  size_t * count)
{
  return rmw_count_pubs_or_subs(node, DDS_BUILTIN_TOPIC_DCPSSUBSCRIPTION, topic_name, count);
}

extern "C" rmw_ret_t rmw_get_subscriber_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  bool no_demangle,
  rmw_names_and_types_t * tptyp)
{
  return get_endpoint_names_and_types_by_node(
    node, allocator, node_name, node_namespace, no_demangle, tptyp, true, false);
}

extern "C" rmw_ret_t rmw_get_publisher_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  bool no_demangle,
  rmw_names_and_types_t * tptyp)
{
  return get_endpoint_names_and_types_by_node(
    node, allocator, node_name, node_namespace, no_demangle, tptyp, false, true);
}

extern "C" rmw_ret_t rmw_get_service_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  rmw_names_and_types_t * sntyp)
{
  return get_cs_names_and_types_by_node(node, allocator, node_name, node_namespace, sntyp, true);
}

extern "C" rmw_ret_t rmw_get_client_names_and_types_by_node(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * node_name,
  const char * node_namespace,
  rmw_names_and_types_t * sntyp)
{
  return get_cs_names_and_types_by_node(node, allocator, node_name, node_namespace, sntyp, false);
}


#if RMW_VERSION_GTE(0, 8, 2)

extern "C" rmw_ret_t rmw_get_publishers_info_by_topic(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * topic_name,
  bool no_mangle,
  rmw_topic_endpoint_info_array_t * publishers_info)
{
  return get_endpoint_info_by_topic(
    node,
    allocator,
    topic_name,
    no_mangle,
    true,
    publishers_info);
}

extern "C" rmw_ret_t rmw_get_subscriptions_info_by_topic(
  const rmw_node_t * node,
  rcutils_allocator_t * allocator,
  const char * topic_name,
  bool no_mangle,
  rmw_topic_endpoint_info_array_t * subscriptions_info)
{
  return get_endpoint_info_by_topic(
    node,
    allocator,
    topic_name,
    no_mangle,
    false,
    subscriptions_info);
}

#endif
