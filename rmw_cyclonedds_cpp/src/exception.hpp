// Copyright 2018 to 2019 ADLINK Technology
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

#ifndef EXCEPTION_HPP_
#define EXCEPTION_HPP_

#include <stdexcept>
#include <string>

namespace rmw_cyclonedds_cpp
{

class Exception : public std::exception
{
public:
  virtual ~Exception() throw();
  virtual const char * what() const throw();

protected:
  explicit Exception(const char * const & message);
  Exception(const Exception & ex);
  Exception & operator=(const Exception & ex);

  std::string m_message;
};

/// Stub for code that should never be reachable by design.
/// If it is possible to reach the code due to bad data or other runtime conditions,
/// use a runtime_error instead
[[noreturn]] inline void unreachable()
{
#if defined(__has_builtin)
#if __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#endif
#elif (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 5))
  __builtin_unreachable();
#endif
  throw std::logic_error("This code should be unreachable.");
}
}  // namespace rmw_cyclonedds_cpp

#endif  // EXCEPTION_HPP_
