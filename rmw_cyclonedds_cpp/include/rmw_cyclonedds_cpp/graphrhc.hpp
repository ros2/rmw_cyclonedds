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
#ifndef RMW_CYCLONEDDS_CPP__GRAPHRHC_HPP_
#define RMW_CYCLONEDDS_CPP__GRAPHRHC_HPP_

#include "dds/dds.h"

/* Introduction of custom RHC coincides with promoting the library instance & domains to entities,
   and so with the introduction of DDS_CYCLONEDDS_HANDLE. */
#ifdef DDS_CYCLONEDDS_HANDLE
#include "dds/ddsc/dds_rhc.h"

struct dds_rhc * graphrhc_new();
#endif  // DDS_CYCLONEDDS_HANDLE

#endif  // RMW_CYCLONEDDS_CPP__GRAPHRHC_HPP_
