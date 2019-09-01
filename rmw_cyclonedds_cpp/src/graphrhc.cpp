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

struct graphrhc : dds_rhc
{
  struct dds_reader * reader;
};

static dds_return_t graphrhc_associate(
  struct dds_rhc * rhc_cmn, struct dds_reader * reader,
  const struct ddsi_sertopic * topic,
  struct ddsi_tkmap * tkmap)
{
  // C++ doesn't grok the fake inheritance in C, so static_cast won't work
  struct graphrhc * rhc = reinterpret_cast<struct graphrhc *>(rhc_cmn);
  rhc->reader = reader;
  static_cast<void>(topic);
  static_cast<void>(tkmap);
  return DDS_RETCODE_OK;
}

static void graphrhc_free(struct ddsi_rhc * rhc_cmn)
{
  // C++ doesn't grok the fake inheritance in C, so static_cast won't work
  struct graphrhc * rhc = reinterpret_cast<struct graphrhc *>(rhc_cmn);
  delete rhc;
}

static bool graphrhc_store(
  struct ddsi_rhc * __restrict rhc_cmn,
  const struct ddsi_writer_info * __restrict wrinfo,
  struct ddsi_serdata * __restrict sample,
  struct ddsi_tkmap_instance * __restrict tk)
{
  // C++ doesn't grok the fake inheritance in C, so static_cast won't work
  struct graphrhc * rhc = reinterpret_cast<struct graphrhc *>(rhc_cmn);
  dds_reader_data_available_cb(rhc->reader);
  static_cast<void>(wrinfo);
  static_cast<void>(sample);
  static_cast<void>(tk);
  return true;
}

static void graphrhc_unregister_wr(
  struct ddsi_rhc * __restrict rhc_cmn,
  const struct ddsi_writer_info * __restrict wrinfo)
{
  // C++ doesn't grok the fake inheritance in C, so static_cast won't work
  struct graphrhc * rhc = reinterpret_cast<struct graphrhc *>(rhc_cmn);
  dds_reader_data_available_cb(rhc->reader);
  static_cast<void>(wrinfo);
}

static void graphrhc_relinquish_ownership(
  struct ddsi_rhc * __restrict rhc_cmn,
  const uint64_t wr_iid)
{
  static_cast<void>(rhc_cmn);
  static_cast<void>(wr_iid);
}

static void graphrhc_set_qos(struct ddsi_rhc * rhc_cmn, const struct dds_qos * qos)
{
  static_cast<void>(rhc_cmn);
  static_cast<void>(qos);
}

static int graphrhc_read(
  struct dds_rhc * rhc_cmn, bool lock, void ** values,
  dds_sample_info_t * info_seq, uint32_t max_samples, uint32_t mask,
  dds_instance_handle_t handle, struct dds_readcond * cond)
{
  static_cast<void>(rhc_cmn);
  static_cast<void>(lock);
  static_cast<void>(values);
  static_cast<void>(info_seq);
  static_cast<void>(max_samples);
  static_cast<void>(mask);
  static_cast<void>(handle);
  static_cast<void>(cond);
  return 0;
}

static int graphrhc_take(
  struct dds_rhc * rhc_cmn, bool lock, void ** values,
  dds_sample_info_t * info_seq, uint32_t max_samples, uint32_t mask,
  dds_instance_handle_t handle, struct dds_readcond * cond)
{
  static_cast<void>(rhc_cmn);
  static_cast<void>(lock);
  static_cast<void>(values);
  static_cast<void>(info_seq);
  static_cast<void>(max_samples);
  static_cast<void>(mask);
  static_cast<void>(handle);
  static_cast<void>(cond);
  return 0;
}

static int graphrhc_takecdr(
  struct dds_rhc * rhc_cmn, bool lock, struct ddsi_serdata ** values,
  dds_sample_info_t * info_seq, uint32_t max_samples,
  uint32_t sample_states, uint32_t view_states, uint32_t instance_states,
  dds_instance_handle_t handle)
{
  static_cast<void>(rhc_cmn);
  static_cast<void>(lock);
  static_cast<void>(values);
  static_cast<void>(info_seq);
  static_cast<void>(max_samples);
  static_cast<void>(sample_states);
  static_cast<void>(view_states);
  static_cast<void>(instance_states);
  static_cast<void>(handle);
  return 0;
}

static bool graphrhc_add_readcondition(struct dds_rhc * rhc_cmn, struct dds_readcond * cond)
{
  static_cast<void>(rhc_cmn);
  static_cast<void>(cond);
  return true;
}

static void graphrhc_remove_readcondition(struct dds_rhc * rhc_cmn, struct dds_readcond * cond)
{
  static_cast<void>(rhc_cmn);
  static_cast<void>(cond);
}

static uint32_t graphrhc_lock_samples(struct dds_rhc * rhc_cmn)
{
  static_cast<void>(rhc_cmn);
  return 0;
}

static const struct dds_rhc_ops graphrhc_ops = {
  {
    graphrhc_store,
    graphrhc_unregister_wr,
    graphrhc_relinquish_ownership,
    graphrhc_set_qos,
    graphrhc_free
  },
  graphrhc_read,
  graphrhc_take,
  graphrhc_takecdr,
  graphrhc_add_readcondition,
  graphrhc_remove_readcondition,
  graphrhc_lock_samples,
  graphrhc_associate
};

struct dds_rhc * graphrhc_new()
{
  auto rhc = new graphrhc;
  rhc->common.ops = &graphrhc_ops;
  return static_cast<struct dds_rhc *>(rhc);
}

#endif  // DDS_CYCLONEDDS_HANDLE

#endif  // RMW_CYCLONEDDS_CPP__GRAPHRHC_HPP_
