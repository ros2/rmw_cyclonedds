// Copyright 2020 Open Source Robotics Foundation, Inc.
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

#ifndef UNIQUE_VOID_HPP_
#define UNIQUE_VOID_HPP_

// https://stackoverflow.com/a/39288979

using unique_void_ptr_t = std::unique_ptr<void, void(*)(void const*)>;

template<typename T>
unique_void_ptr_t
make_unique_void(T * ptr)
{
    return unique_void_ptr_t(ptr, [](void const * data) {
         auto p = static_cast<T const *>(data);
         delete p;
    });
}

inline
unique_void_ptr_t
make_null_unique_void()
{
    return unique_void_ptr_t(nullptr, [](void const *) {});
}
#endif  // UNIQUE_VOID_HPP_
