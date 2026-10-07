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

#include "BufferEndpointDiscovery.hpp"

#include <cstdio>
#include <exception>
#include <unordered_map>
#include <vector>

#include "rmw/impl/cpp/key_value.hpp"

namespace rmw_cyclonedds_cpp
{

rosidl_buffer_backend_registry::BufferBackendRegistry & process_backend_registry()
{
  static rosidl_buffer_backend_registry::BufferBackendRegistry registry;
  return registry;
}

std::mutex & process_backend_registry_mutex()
{
  static std::mutex m;
  return m;
}

std::mutex & backend_instance_mutex(const std::string & backend_type)
{
  static std::mutex map_mutex;
  static std::map<std::string, std::unique_ptr<std::mutex>> mutexes;
  std::lock_guard<std::mutex> lock(map_mutex);
  auto it = mutexes.find(backend_type);
  if (it == mutexes.end()) {
    it = mutexes.emplace(backend_type, std::make_unique<std::mutex>()).first;
  }
  return *it->second;
}

bool backend_locally_available(const std::string & type)
{
  // The whole call below is guarded by try/catch rather than reasoning
  // about which part of it can throw. BufferBackendRegistry::
  // create_backend_instance() (find_backend_by_type()'s only pluginlib
  // call) already catches std::exception internally, which might suggest
  // a wrapping try/catch here is dead code -- but find_backend_by_type()
  // itself (backend_utils.hpp) also calls the resulting backend's own
  // get_backend_type() AFTER create_backend_instance() returns, and that
  // virtual call is NOT inside create_backend_instance()'s try/catch -- a
  // plugin whose get_backend_type() throws propagates uncaught right back
  // here, at an extern "C" API boundary (reached from
  // rmw_create_publisher/rmw_create_subscription).
  //
  // The result is cached by type name. Without this, find_backend_by_type()
  // would run unconditionally on every call, i.e. once per candidate name
  // in RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS, on EVERY publisher/subscription
  // creation (create_readwrite_qos() in rmw_node.cpp). pluginlib's
  // createSharedInstance() (what find_backend_by_type() resolves through)
  // already caches an instance by class name process-wide, so this was
  // never a repeated full construction -- but it was still a repeated
  // registry lookup under process_backend_registry_mutex() for a result
  // (does this process have a plugin for this type name) that cannot
  // change over the process's lifetime once first resolved. Caching it
  // explicitly here means the conclusion doesn't depend on pluginlib's own
  // caching behavior: finish_publication_match()/finish_subscription_
  // match()'s own later find_backend_by_type() call (a different
  // question -- resolving an actual backend INSTANCE to use, not just
  // checking availability) is untouched by this cache and still runs at
  // match time as before.
  static std::unordered_map<std::string, bool> available_cache;
  {
    std::lock_guard<std::mutex> lock(process_backend_registry_mutex());
    auto it = available_cache.find(type);
    if (it != available_cache.end()) {
      return it->second;
    }
  }
  bool available;
  try {
    std::lock_guard<std::mutex> lock(process_backend_registry_mutex());
    available = rosidl_buffer_backend_registry::find_backend_by_type(
      process_backend_registry(), type) != nullptr;
  } catch (const std::exception & e) {
    std::fprintf(
      stderr, "rmw_cyclonedds_cpp: find_backend_by_type threw for type '%s': %s\n",
      type.c_str(), e.what());
    // The exception path's `false` result is never cached, unlike a
    // genuine not-registered answer. Caching is justified above only
    // because "does this process have a plugin for this type name"
    // cannot change once first resolved -- true for a clean resolution,
    // not for a caught exception, which signals an abnormal, potentially
    // transient failure (e.g. a momentary pluginlib/dlopen hiccup during
    // process start). Returning early here, before the cache write below,
    // means the next call for the same type retries the real lookup
    // instead of being permanently stuck on one bad attempt.
    return false;
  }
  std::lock_guard<std::mutex> lock(process_backend_registry_mutex());
  available_cache.emplace(type, available);
  return available;
}

bool advertises_non_cpu_backend(const dds_qos_t * qos)
{
  void * ud = nullptr;
  size_t udsz = 0;
  if (!dds_qget_userdata(qos, &ud, &udsz) || ud == nullptr) {
    return false;
  }
  std::vector<uint8_t> udvec(static_cast<uint8_t *>(ud), static_cast<uint8_t *>(ud) + udsz);
  dds_free(ud);

  // Real upstream format, confirmed from rmw's own source: "key=value;" --
  // parse_key_value() is the exact function rmw_cyclonedds_cpp's own
  // parse_user_data()/get_user_data_key() already use for the type-hash and
  // enclave entries.
  auto map = rmw::impl::cpp::parse_key_value(udvec);
  auto it = map.find("bufbackends");
  if (it == map.end()) {
    return false;
  }
  std::string value(it->second.begin(), it->second.end());

  // "cpu" alone is the baseline, present on every buffer-backed endpoint
  // regardless of RMW_CYCLONEDDS_CPP_BUFFER_BACKENDS -- is not a
  // private-topic candidate. Anything else in the comma-separated list is.
  size_t start = 0;
  while (start < value.size()) {
    size_t comma = value.find(',', start);
    std::string name = value.substr(
      start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!name.empty() && name != "cpu") {
      return true;
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return false;
}

std::vector<std::string> all_non_cpu_backend_types(const dds_qos_t * qos)
{
  std::vector<std::string> names;
  void * ud = nullptr;
  size_t udsz = 0;
  if (!dds_qget_userdata(qos, &ud, &udsz) || ud == nullptr) {
    return names;
  }
  std::vector<uint8_t> udvec(static_cast<uint8_t *>(ud), static_cast<uint8_t *>(ud) + udsz);
  dds_free(ud);

  auto map = rmw::impl::cpp::parse_key_value(udvec);
  auto it = map.find("bufbackends");
  if (it == map.end()) {
    return names;
  }
  std::string value(it->second.begin(), it->second.end());

  size_t start = 0;
  while (start < value.size()) {
    size_t comma = value.find(',', start);
    std::string name = value.substr(
      start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!name.empty() && name != "cpu") {
      names.push_back(name);
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return names;
}

std::string guid_to_hex(const dds_guid_t & guid)
{
  char buf[33];
  for (size_t i = 0; i < 16; i++) {
    // cpplint's runtime/printf check wants sizeof() here, which does not
    // apply: buf + (i * 2) is a pointer, and sizeof() of it would be the
    // pointer's own size (8), not the 3 bytes (2 hex digits + NUL) each
    // call actually writes.
    std::snprintf(buf + (i * 2), 3, "%02x", guid.v[i]);  // NOLINT
  }
  return std::string(buf, 32);
}

std::string private_topic_name(const std::string & base_topic, const dds_guid_t & peer_guid)
{
  return base_topic + "/_buf/" + guid_to_hex(peer_guid);
}

}  // namespace rmw_cyclonedds_cpp
