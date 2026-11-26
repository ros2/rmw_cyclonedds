// Copyright 2026 Open Source Robotics Foundation, Inc.
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
//
// Ports rmw_fastrtps_cpp's own released per-endpoint-topic mechanism to
// rmw_cyclonedds_cpp. A publisher/subscriber whose matched peer advertises
// wanting a non-CPU buffer backend gets a private, uniquely-named topic --
// base_topic + "/_buf/" + peer-guid-hex -- instead of only ever sharing the
// ordinary topic every CPU-only peer uses.

// This header uses a bare, filename-derived guard with no package prefix,
// matching the convention used by every other header in this package
// (namespace_prefix.hpp -> NAMESPACE_PREFIX_HPP_, dyntype_helper.hpp ->
// DYNTYPE_HELPER_HPP_, and so on, including the two headers test/ adds:
// cyclonedds_test_buffer_backend.hpp -> CYCLONEDDS_TEST_BUFFER_BACKEND_
// HPP_, cyclonedds_test_buffer_impl.hpp -> CYCLONEDDS_TEST_BUFFER_IMPL_
// HPP_). There is no package-prefixed convention anywhere in this package
// to deviate from, and this is also the form ament_cpplint's own
// build/header_guard check expects.
#ifndef BUFFERENDPOINTDISCOVERY_HPP_
#define BUFFERENDPOINTDISCOVERY_HPP_

#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dds/dds.h"
#include "rcutils/allocator.h"
#include "rmw/topic_endpoint_info.h"
#include "rosidl_buffer_backend/buffer_backend.hpp"
#include "rosidl_buffer_backend_registry/backend_utils.hpp"
#include "rosidl_buffer_backend_registry/buffer_backend_registry.hpp"

namespace rmw_cyclonedds_cpp
{

using PeerGuid = std::array<uint8_t, 16>;

/// What's known about one peer's private buffer-backend topic, replacing
/// the bare dds_entity_t this map used to hold under an earlier, now-
/// superseded marker mechanism. `backend` is resolved once, at match time,
/// from the peer's advertised backend type name -- rmw_publish()/rmw_take()
/// need it again on every sample to call create_descriptor_with_endpoint()/
/// from_descriptor_with_endpoint(). `endpoint_info` is owned here
/// (rmw_topic_endpoint_info_fini() in the owning CddsPublisher/
/// CddsSubscription's destroy path) because those two backend calls take it
/// by const reference, not by pointer -- there is no other place for it to
/// live once build_endpoint_info_from_match() returns.
struct PrivateEndpoint
{
  // Built via an explicit constructor rather than left as an aggregate.
  // emplace() at this struct's call sites builds it via piecewise_construct,
  // which parenthesis-initializes -- e.g. PrivateEndpoint(entity, backend,
  // endpoint_info) -- and parenthesized direct-init of an AGGREGATE (which
  // this struct would be without this constructor, since reader_cache_mutex
  // below has no user-declared constructor of its own) is only valid in
  // C++20 (P0960). GCC accepts it as a longstanding extension even under
  // -std=gnu++17, but relying on that is not portable. An explicit
  // constructor makes this an ordinary class, not an aggregate, so
  // PrivateEndpoint(...) is a normal constructor call needing no C++20
  // feature on any conforming compiler.
  PrivateEndpoint(
    dds_entity_t entity_in, std::shared_ptr<rosidl::BufferBackend> backend_in,
    rmw_topic_endpoint_info_t endpoint_info_in)
  : entity(entity_in), backend(std::move(backend_in)), endpoint_info(endpoint_info_in) {}

  dds_entity_t entity;
  std::shared_ptr<rosidl::BufferBackend> backend;
  rmw_topic_endpoint_info_t endpoint_info;
  // This field originally was a per-peer mutex intended to serialize calls
  // into `backend`. That does not work: rosidl_buffer_backend_registry::
  // find_backend_by_type() resolves through pluginlib's
  // createSharedInstance(), which CACHES and returns the SAME shared_ptr
  // for a given class name every time -- so any two peers (or a
  // publisher/subscriber pair) that negotiate the same backend TYPE STRING
  // share the IDENTICAL rosidl::BufferBackend C++ object, and a mutex
  // scoped to one PrivateEndpoint cannot serialize against a different
  // PrivateEndpoint calling into that same shared object. See
  // backend_instance_mutex() below -- keyed by backend TYPE, not by peer --
  // which is what every caller of create_descriptor_with_endpoint()/
  // create_empty_descriptor()/from_descriptor_with_endpoint() now holds
  // instead.
  //
  // This field is repurposed, not removed: it now guards ONLY this peer's
  // own private reader and its pending_descriptors/pending_order cache
  // below (subscription side only -- rmw_publish() never touches either),
  // which is a genuinely per-peer resource unrelated to the backend
  // instance question above.
  mutable std::mutex reader_cache_mutex;

  // The shared-topic message and its paired private-topic descriptor are
  // two independent DDS writes (rmw_publish(), same dds_write_ts()
  // timestamp, two separate calls) with no ordering guarantee between
  // them, AND rmw_publish() can legitimately skip writing a descriptor for
  // one specific message (create_descriptor_with_endpoint() returning
  // nullptr is documented CPU-fallback, not an error) while the
  // shared-topic write for that same message still goes out. A bare "take
  // whatever is next in the private reader's queue" therefore risks
  // installing message N+1's descriptor into message N's buffer -- silent
  // cross-message misattribution, not the disclosed "stays on CPU" miss
  // case.
  //
  // The fix: both writes share ONE `dds_time_t` timestamp per message
  // (rmw_publish() passes the identical `tstamp` to both dds_write_ts()
  // calls), so consume_buffer_descriptor() in rmw_node.cpp correlates by
  // EXACT timestamp equality rather than queue order, never installing a
  // descriptor whose timestamp does not match the message being
  // processed. A descriptor drained out of order (arrived before its
  // message was taken, or belongs to a message whose own descriptor write
  // was skipped and is therefore never claimed) is cached here, keyed by
  // timestamp, so a later call for the message it actually belongs to can
  // still find it. Bounded by kMaxPendingDescriptors and evicted
  // oldest-first, so a peer whose descriptors are never claimed (e.g.
  // every publish takes the CPU fallback) cannot grow this without bound.
  static constexpr size_t kMaxPendingDescriptors = 8;
  std::unordered_map<dds_time_t, std::shared_ptr<void>> pending_descriptors;
  std::deque<dds_time_t> pending_order;
};

/// QoS for a private descriptor-exchange writer/reader pair. CycloneDDS's
/// own default (passed by giving dds_create_writer()/dds_create_reader() a
/// null QoS) is BEST_EFFORT reliability with KEEP_LAST depth 1 -- and depth
/// 1 means the DDS layer itself overwrites or drops any sample not yet
/// taken the moment the next one is written, independent of and before
/// PrivateEndpoint's own kMaxPendingDescriptors cache ever gets a chance to
/// drain it. That silently defeats the cache: it can only ever hold what
/// one dds_take() burst pulls off the reader, which with depth 1 is at
/// most one live sample. RELIABLE plus KEEP_LAST at exactly
/// kMaxPendingDescriptors gives the cache the window its own eviction
/// logic was written to use, on both the write and the read side.
inline dds_qos_t * make_private_endpoint_qos()
{
  dds_qos_t * qos = dds_create_qos();
  dds_qset_reliability(qos, DDS_RELIABILITY_RELIABLE, DDS_INFINITY);
  dds_qset_history(
    qos, DDS_HISTORY_KEEP_LAST,
    static_cast<int32_t>(PrivateEndpoint::kMaxPendingDescriptors));
  return qos;
}

/// Real state for the per-endpoint private-topic discovery below. One
/// instance lives on each CddsPublisher/CddsSubscription carrying a
/// buffer-backed message type. Keyed by the matched peer's own GUID so a
/// redundant publication_matched/subscription_matched callback -- DDS fires
/// these on unmatch too, and can re-fire on a transient network blip --
/// never creates a second private topic for the same peer.
struct BufferEndpointDiscovery
{
  // A shared_mutex, not a plain mutex, because rmw_publish() used to
  // release the lock before dereferencing pointers into the map,
  // reasoning that "nothing ever erases an entry" -- true of the match
  // callbacks, false of destroy_publisher()/destroy_subscription(), which
  // DO clear() this map under the same lock. A plain mutex forced a
  // choice between that use-after-free and re-serializing rmw_publish
  // against the match callbacks' own cheap reads. A shared_mutex avoids
  // the tradeoff: rmw_publish and the match callbacks' read-only
  // duplicate checks take a SHARED lock (compatible with each other),
  // while the callbacks' emplace() and destroy's clear() take an
  // EXCLUSIVE lock -- so rmw_publish can no longer race a concurrent
  // destroy, without being serialized against unrelated concurrent
  // publishes or match checks.
  std::shared_mutex mutex;
  std::map<PeerGuid, PrivateEndpoint> private_writers_or_readers;

  // On a graph-cache-lag retry, on_publication_matched/
  // on_subscription_matched defer the retry loop to a detached background
  // thread instead of blocking CycloneDDS's shared listener-callback
  // thread. That thread captures a raw pointer to the owning
  // CddsPublisher/CddsSubscription with no other lifetime guard, so
  // destroy_publisher()/destroy_subscription() must wait for
  // pending_async_retries to reach zero (async_retry_cv) before freeing
  // that object -- otherwise a retry finishing after the object is freed
  // dereferences it.
  //
  // Waiting for the counter alone is not sufficient -- it only accounts
  // for retries ALREADY spawned. A match callback still attached (the
  // listener isn't detached until dds_delete(pub->enth)/
  // dds_delete(sub->enth) actually runs) can spawn a BRAND NEW retry
  // thread after the counter was observed at zero. `shutting_down` closes
  // that: begin_async_retry() refuses to spawn once it is set, and it is
  // set under the SAME mutex the counter check uses, so the two can never
  // race past each other.
  std::mutex async_retry_mutex;
  std::condition_variable async_retry_cv;
  int pending_async_retries {0};
  bool shutting_down {false};

  // on_publication_matched's/on_subscription_matched's null-ep retry
  // thread (see their own comments) fires on every ordinary unmatch
  // event -- the common, frequent case -- not only the rare
  // endpoint-info-build failure the retry above guards. Counted
  // separately so it can be capped at its own, tighter limit
  // (begin_null_ep_retry()'s kMaxPendingNullEpRetries) independent of
  // the retry above's own, separate cap (begin_async_retry()'s
  // kMaxPendingAsyncRetries) -- both retries share pending_async_retries
  // itself (begin_null_ep_retry() increments both counters), but each
  // has a distinct ceiling on how much of that shared total it may use.
  int pending_null_ep_retries {0};
};

/// Call once, before dds_delete(pub->enth)/dds_delete(sub->enth), so any
/// match callback still able to fire (the listener is not yet detached)
/// that decides to retry will see shutting_down and refuse to spawn --
/// see BufferEndpointDiscovery's own comment on why the flag and the
/// counter must share one mutex.
inline void begin_shutdown(BufferEndpointDiscovery & bd)
{
  std::lock_guard<std::mutex> lock(bd.async_retry_mutex);
  bd.shutting_down = true;
}

/// Capped for the same reason begin_null_ep_retry() below is: an unmatch
/// storm there is the common case this bound was written for, but a
/// discovery storm (many peers matching at once while the graph cache
/// lags, e.g. fleet startup) can drive this path just as hard -- and each
/// thread here does strictly more work per retry (full backend
/// resolution through pluginlib, private topic/writer/reader creation)
/// than a null-ep retry's single lookup. Set higher than
/// kMaxPendingNullEpRetries because this path is the legitimate,
/// necessary one (the null-ep path is a workaround for a race, this one
/// is the real discovery handshake), not because the per-thread cost is
/// smaller -- it is larger.
constexpr int kMaxPendingAsyncRetries = 16;

/// Begin one background discovery retry. Returns false -- meaning the
/// caller must NOT spawn the thread it was about to -- when begin_shutdown()
/// has already run or kMaxPendingAsyncRetries concurrent retries (of
/// either kind -- begin_null_ep_retry() increments this same counter) are
/// already outstanding; true (with pending_async_retries incremented,
/// paired with a later end_async_retry()) otherwise. A caller that gets
/// false here drops the retry rather than blocking for one: the match
/// callback this is called from must not block CycloneDDS's shared
/// listener thread, so there is nowhere to queue it.
inline bool begin_async_retry(BufferEndpointDiscovery & bd)
{
  std::lock_guard<std::mutex> lock(bd.async_retry_mutex);
  if (bd.shutting_down || bd.pending_async_retries >= kMaxPendingAsyncRetries) {
    return false;
  }
  bd.pending_async_retries++;
  return true;
}

/// notify_all() is called WHILE STILL HOLDING the lock, not after releasing
/// it. This function runs on every match, not only on a rare
/// graph-cache-lag retry, so the ordering here matters generally: if the
/// notify happened after unlock, the thread calling
/// wait_for_async_retries() could observe pending_async_retries==0,
/// return, and let its caller free the very BufferEndpointDiscovery (and
/// its condition_variable) this function is still about to call
/// notify_all() on, a use-after-free on freed memory. Holding the lock
/// across notify_all() means the waiter cannot acquire the mutex -- and
/// therefore cannot return from wait() -- until this function (and its
/// notify_all() call) has already finished.
inline void end_async_retry(BufferEndpointDiscovery & bd)
{
  std::lock_guard<std::mutex> lock(bd.async_retry_mutex);
  bd.pending_async_retries--;
  bd.async_retry_cv.notify_all();
}

/// Like begin_async_retry(), for on_publication_matched's/
/// on_subscription_matched's null-ep retry thread specifically. That
/// thread fires on every null ep, including an ordinary unmatch -- the
/// common, frequent case -- not only the rare endpoint-info-build
/// failure begin_async_retry() above guards. Bounded at
/// kMaxPendingNullEpRetries so topic churn cannot spawn an unbounded
/// number of these; a caller refused here must treat the null ep as an
/// unmatch, exactly as if the retry itself had found nothing. Also
/// increments pending_async_retries (paired with end_null_ep_retry()'s
/// matching decrement), so wait_for_async_retries() -- which only
/// watches that one counter -- still waits for these threads too.
inline bool begin_null_ep_retry(BufferEndpointDiscovery & bd)
{
  constexpr int kMaxPendingNullEpRetries = 4;
  std::lock_guard<std::mutex> lock(bd.async_retry_mutex);
  if (bd.shutting_down || bd.pending_null_ep_retries >= kMaxPendingNullEpRetries) {
    return false;
  }
  bd.pending_null_ep_retries++;
  bd.pending_async_retries++;
  return true;
}

/// Pairs with begin_null_ep_retry().
inline void end_null_ep_retry(BufferEndpointDiscovery & bd)
{
  std::lock_guard<std::mutex> lock(bd.async_retry_mutex);
  bd.pending_null_ep_retries--;
  bd.pending_async_retries--;
  bd.async_retry_cv.notify_all();
}

/// Blocks until every background retry begin_async_retry() started has
/// called end_async_retry(). Call after begin_shutdown() and BEFORE
/// dds_delete(pub->enth)/dds_delete(sub->enth) -- both real call sites do
/// this, in exactly this order: a retry thread that began before
/// begin_shutdown() ran is still executing and still dereferences
/// pub->enth/sub->enth (e.g. to create or write the private topic), so
/// deleting that entity before the retry has finished racing against it
/// is a use-after-free of the DDS entity, independent of whether the
/// owning CddsPublisher/CddsSubscription C++ object itself is still
/// alive. begin_shutdown() stops any FUTURE begin_async_retry() call from
/// spawning (mutex-ordered against this function's own predicate, so a
/// call that raced ahead of begin_shutdown() already incremented
/// pending_async_retries before this function is ever entered, and one
/// that raced behind it observes shutting_down and refuses); this
/// function then drains every retry already spawned, of either kind,
/// before the entity delete that follows it is safe to issue.
inline void wait_for_async_retries(BufferEndpointDiscovery & bd)
{
  std::unique_lock<std::mutex> lock(bd.async_retry_mutex);
  bd.async_retry_cv.wait(lock, [&bd] {return bd.pending_async_retries == 0;});
}

/// A per-CddsPublisher/CddsSubscription registry would make it possible
/// for two entities in the SAME process to disagree about backend
/// availability, and for "advertised" (via
/// RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS) to silently diverge from "locally
/// resolvable" (via find_backend_by_type()) with nothing to catch it.
/// Backend plugin availability is a property of the PROCESS, not of one
/// endpoint, so this is process-wide: one BufferBackendRegistry, lazily
/// constructed on first genuine use (still never paid by a process with no
/// buffer-backed endpoints at all), guarded by its own mutex since
/// create_backend_instance() is not documented safe for concurrent calls.
rosidl_buffer_backend_registry::BufferBackendRegistry & process_backend_registry();
std::mutex & process_backend_registry_mutex();

/// pluginlib's createSharedInstance() (what find_backend_by_type()
/// resolves through) caches backend instances BY CLASS NAME, so every
/// PrivateEndpoint that negotiated the same backend type shares one
/// rosidl::BufferBackend object, process-wide, regardless of which peer or
/// which publisher/subscription resolved it. A per-PrivateEndpoint mutex
/// cannot serialize calls into a shared object; this is the ONE mutex,
/// keyed by backend type name, every caller of
/// create_descriptor_with_endpoint()/create_empty_descriptor()/
/// from_descriptor_with_endpoint() must hold for the duration of that
/// call. Lazily created per type name, guarded by its own small map mutex
/// -- deliberately NOT process_backend_registry_mutex(), which guards
/// backend RESOLUTION (a much less frequently taken lock); reusing it
/// here would serialize every publish/take against match-time resolution
/// for no reason.
std::mutex & backend_instance_mutex(const std::string & backend_type);

/// True if `type` resolves to a real, locally-loadable backend via
/// find_backend_by_type() against the process-wide registry above. Used at
/// QoS-creation time (create_readwrite_qos() in rmw_node.cpp) so this
/// process only ever ADVERTISES a name it can actually satisfy -- closing
/// the gap where two peers' mutually-advertised intersection contains a
/// name that only one side can actually resolve, which reproduces the
/// exact sertype-mismatch "inconsistent topic" class this PR fixes, from a
/// different cause than the ordering bug it already fixes.
bool backend_locally_available(const std::string & type);

/// True if `qos`'s user_data (as create_readwrite_qos() in rmw_node.cpp
/// writes it) names any backend besides "cpu" in its "bufbackends="
/// key=value entry -- the real, if minimal, signal distinguishing a peer
/// that only ever gets the ordinary CPU-fallback path from one that has
/// been told, via RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS, to participate in
/// the private-topic mechanism.
bool advertises_non_cpu_backend(const dds_qos_t * qos);

/// Delete every private writer/reader entity a match callback created and
/// finalize its endpoint_info, then clear the map -- under the map's own
/// exclusive lock, since rmw_publish()/consume_buffer_descriptor()'s shared
/// reads of it must not overlap this mutation. Call after
/// begin_shutdown()/wait_for_async_retries()/dds_delete(pub->enth or
/// sub->enth), exactly like destroy_publisher()/destroy_subscription()
/// already do inline.
///
/// Four construction-failure paths (create_cdds_publisher()'s
/// fail_instance_handle, create_publisher()'s cleanup_cdds_publisher
/// scope_exit, and their subscription-side mirrors) run the
/// shutdown/wait/delete sequence above but must also drain this map, or
/// any private writer/reader a SYNCHRONOUS match completed before the
/// failure leaks. Extracted here so all six call sites (the four
/// construction-failure paths plus destroy_publisher()/
/// destroy_subscription()'s own inline copies) share one implementation
/// instead of six independently-maintained copies of the same three
/// lines.
inline void drain_private_endpoints(BufferEndpointDiscovery & bd)
{
  std::unique_lock<std::shared_mutex> lock(bd.mutex);
  rcutils_allocator_t allocator = rcutils_get_default_allocator();
  for (auto & entry : bd.private_writers_or_readers) {
    dds_delete(entry.second.entity);
    rmw_topic_endpoint_info_fini(&entry.second.endpoint_info, &allocator);
  }
  bd.private_writers_or_readers.clear();
}

/// Every backend name besides "cpu" named in `qos`'s "bufbackends=" entry,
/// in the order the peer listed them, or empty if there is none. Companion
/// to advertises_non_cpu_backend() above -- that answers "is there one",
/// this answers "which ones", plural: a peer may advertise several
/// alternatives (e.g. "cuda,shm"), and the caller must try each in turn
/// via find_backend_by_type() rather than giving up after the first one
/// this process happens not to have a plugin for.
std::vector<std::string> all_non_cpu_backend_types(const dds_qos_t * qos);

/// Lowercase hex of `guid.v` (32 hex characters), used as the private
/// topic's name suffix.
std::string guid_to_hex(const dds_guid_t & guid);

/// The private topic name for one (base_topic, peer) pair. A free function
/// rather than inlined at each call site so the publisher and subscriber
/// sides can never independently drift on the naming scheme -- they MUST
/// agree exactly, or the two never actually match on DDS's own topic-name
/// based discovery.
std::string private_topic_name(const std::string & base_topic, const dds_guid_t & peer_guid);

}  // namespace rmw_cyclonedds_cpp

#endif  // BUFFERENDPOINTDISCOVERY_HPP_
