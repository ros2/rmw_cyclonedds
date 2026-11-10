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
// A minimal, self-contained BufferImplBase<T> used only by this package's
// own gtest suite. Deliberately does NOT reuse test_rosidl_buffer's own
// TestBufferImpl<T> (ros2/system_tests) -- that header is not part of that
// package's exported CMake interface (only its pluginlib SHARED library
// and generated message headers are), so depending on it from an external
// package's test/ directory would be relying on an export ros2/system_tests
// never promised. This keeps the buffer-backed producer side entirely
// local to rmw_cyclonedds_cpp's own test tree.

#ifndef CYCLONEDDS_TEST_BUFFER_IMPL_HPP_
#define CYCLONEDDS_TEST_BUFFER_IMPL_HPP_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "rosidl_buffer/buffer_impl_base.hpp"
#include "rosidl_buffer/cpu_buffer_impl.hpp"

namespace rmw_cyclonedds_cpp
{
namespace test
{

/// A trivial non-CPU BufferImplBase<T>, distinguishable from rosidl::
/// CpuBufferImpl<T> only by get_backend_type() -- storage is an ordinary
/// std::vector<T>, same as the CPU impl. That is deliberate: this test does
/// not exercise a real device transfer, only the RMW/backend-plugin
/// negotiation and descriptor-serialization contract, so nothing here needs
/// to actually be off-CPU memory.
template<typename T>
class CyclonddsTestBufferImpl : public rosidl::BufferImplBase<T>
{
public:
  CyclonddsTestBufferImpl() = default;

  explicit CyclonddsTestBufferImpl(std::vector<T> data)
  : storage_(std::move(data)) {}

  std::string get_backend_type() const override {return "cyclonedds_test";}

  size_t size() const override {return storage_.size();}

  std::unique_ptr<rosidl::BufferImplBase<T>> to_cpu() const override
  {
    auto cpu = std::make_unique<rosidl::CpuBufferImpl<T>>();
    cpu->get_storage() = storage_;
    return cpu;
  }

  std::unique_ptr<rosidl::BufferImplBase<T>> clone() const override
  {
    return std::make_unique<CyclonddsTestBufferImpl<T>>(storage_);
  }

  const void * descriptor() const override {return storage_.data();}

  const std::vector<T> & get_storage() const {return storage_;}

private:
  std::vector<T> storage_;
};

}  // namespace test
}  // namespace rmw_cyclonedds_cpp

#endif  // CYCLONEDDS_TEST_BUFFER_IMPL_HPP_
