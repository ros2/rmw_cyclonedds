// Copyright 2019 ADLINK Technology
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
#ifndef RMW_VERSION_TEST_HPP_
#define RMW_VERSION_TEST_HPP_

/* True if the version of RMW is at least major.minor.patch */
#define RMW_VERSION_GTE(major, minor, patch) ( \
    major < RMW_VERSION_MAJOR || ( \
      major == RMW_VERSION_MAJOR && ( \
        minor < RMW_VERSION_MINOR || ( \
          minor == RMW_VERSION_MINOR && patch <= RMW_VERSION_PATCH))))

#endif  // RMW_VERSION_TEST_HPP_
