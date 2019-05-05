# Copyright 2018 ADLINK Technology Limited
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

###############################################################################
#
# CMake module for finding Eclipse Cyclone DDS.
#
# Output variables:
#
# - CycloneDDS_FOUND: flag indicating if the package was found
# - CycloneDDS_INCLUDE_DIR: Paths to the header files
#
# Example usage:
#
#   find_package(CycloneDDS_cmake_module REQUIRED)
#   find_package(CycloneDDS MODULE)
#   # use CycloneDDS_* variables
#
###############################################################################

# lint_cmake: -convention/filename, -package/stdargs

set(CycloneDDS_FOUND FALSE)

find_package(CycloneDDS REQUIRED CONFIG)

#find_library(CycloneDDS_LIBRARY_RELEASE
#  NAMES cyclonedds-${cyclonedds_MAJOR_MINOR_VERSION} cyclonedds)
find_library(CycloneDDS_LIBRARY_RELEASE
  NAMES cdds cdds)
#find_library(CycloneDDS_LIBRARY_DEBUG
#  NAMES cycloneddsd-${cyclonedds_MAJOR_MINOR_VERSION})

set(CycloneDDS_INCLUDE_DIR get_target_property(VAR CycloneDDS::ddsc INTERFACE_INCLUDE_DIRECTORIES))

if(CycloneDDS_LIBRARY_RELEASE AND CycloneDDS_LIBRARY_DEBUG)
  set(CycloneDDS_LIBRARIES
    optimized ${CycloneDDS_LIBRARY_RELEASE}
    debug ${CycloneDDS_LIBRARY_DEBUG}
  )
elseif(CycloneDDS_LIBRARY_RELEASE)
  set(CycloneDDS_LIBRARIES
    ${CycloneDDS_LIBRARY_RELEASE}
  )
elseif(CycloneDDS_LIBRARY_DEBUG)
  set(CycloneDDS_LIBRARIES
    ${CycloneDDS_LIBRARY_DEBUG}
  )
else()
  set(CycloneDDS_LIBRARIES "")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CycloneDDS
  FOUND_VAR CycloneDDS_FOUND
  REQUIRED_VARS
    CycloneDDS_INCLUDE_DIR
    CycloneDDS_LIBRARIES
)
