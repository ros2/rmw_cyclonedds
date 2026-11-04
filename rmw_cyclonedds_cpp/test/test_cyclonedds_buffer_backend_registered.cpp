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
// Mirrors cuda_buffer_backend's own test_cuda_backend_registered.cpp --
// proves DYNAMIC discovery via the real BufferBackendRegistry/pluginlib
// ClassLoader, not direct instantiation. Deliberately never names
// rmw_cyclonedds_cpp::test::CyclonddsTestBufferBackend anywhere: if this
// file ever needs to #include that header to pass, the thing it is
// supposed to prove (that a plugin is discoverable purely via the ament
// index) has stopped being proven.

#include <gtest/gtest.h>

#include <algorithm>

#include "rosidl_buffer_backend_registry/backend_utils.hpp"
#include "rosidl_buffer_backend_registry/buffer_backend_registry.hpp"

TEST(CyclonddsTestBackendRegistered, discoverable_by_name)
{
  rosidl_buffer_backend_registry::BufferBackendRegistry registry;
  auto names = registry.get_backend_names();
  auto found = std::find(
    names.begin(), names.end(), "rmw_cyclonedds_cpp::test::CyclonddsTestBufferBackend");
  ASSERT_NE(found, names.end())
    << "rmw_cyclonedds_cpp::test::CyclonddsTestBufferBackend not in get_backend_names(); "
    << names.size() << " backend(s) found";
}

TEST(CyclonddsTestBackendRegistered, instantiable_and_reports_its_own_type)
{
  rosidl_buffer_backend_registry::BufferBackendRegistry registry;
  auto backend = registry.create_backend_instance(
    "rmw_cyclonedds_cpp::test::CyclonddsTestBufferBackend");
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->get_backend_type(), "cyclonedds_test");
}

// find_backend_by_type() is what on_publication_matched()/
// on_subscription_matched() actually call to resolve a peer's advertised
// backend TYPE ("cyclonedds_test") to a live instance -- this is its one
// live test against a genuinely registered, non-CUDA plugin.
TEST(CyclonddsTestBackendRegistered, findable_by_type)
{
  rosidl_buffer_backend_registry::BufferBackendRegistry registry;
  auto backend =
    rosidl_buffer_backend_registry::find_backend_by_type(registry, "cyclonedds_test");
  ASSERT_NE(backend, nullptr);
  EXPECT_EQ(backend->get_backend_type(), "cyclonedds_test");
  EXPECT_EQ(nullptr, rosidl_buffer_backend_registry::find_backend_by_type(registry, "rocm"));
}
