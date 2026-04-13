// Copyright 2019 Rover Robotics via Dan Rose
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

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <memory>

#include "rosidl_typesupport_interface/macros.h"

// std_msgs
#include "std_msgs/msg/detail/bool__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/int32__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/string__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/header__rosidl_typesupport_introspection_cpp.hpp"
#include "std_msgs/msg/detail/float64_multi_array__rosidl_typesupport_introspection_cpp.hpp"

// visualization_msgs
#include "visualization_msgs/msg/marker.hpp"
#include "visualization_msgs/msg/marker_array.hpp"
#include "visualization_msgs/msg/detail/marker__rosidl_typesupport_introspection_cpp.hpp"
#include "visualization_msgs/msg/detail/marker_array__rosidl_typesupport_introspection_cpp.hpp"

// sensor_msgs
#include "sensor_msgs/msg/point_cloud2.hpp"
#include "sensor_msgs/msg/laser_scan.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "sensor_msgs/msg/point_field.hpp"
#include "sensor_msgs/msg/detail/point_cloud2__rosidl_typesupport_introspection_cpp.hpp"
#include "sensor_msgs/msg/detail/laser_scan__rosidl_typesupport_introspection_cpp.hpp"
#include "sensor_msgs/msg/detail/image__rosidl_typesupport_introspection_cpp.hpp"

#include "SizeTypeSupport.hpp"
#include "SerTypeSupport.hpp"
#include "DeserTypeSupport.hpp"
#include "BaseCDRWriter.hpp"

#define GET_TS(pkg, iface, name) \
  ROSIDL_TYPESUPPORT_INTERFACE__MESSAGE_SYMBOL_NAME( \
    rosidl_typesupport_introspection_cpp, pkg, iface, name)()

using namespace rmw_cyclonedds_cpp;

// ---------------------------------------------------------------------------
// Pre-allocated array of heavy MarkerArray messages used by the data benchmarks.
// Each contains 20 Markers with 200 points, 200 colors, and non-empty strings.
// ---------------------------------------------------------------------------
static constexpr size_t kNumMessages = 32;
static constexpr size_t kMarkersPerArray = 20;
static constexpr size_t kPointsPerMarker = 200;

static std::array<visualization_msgs::msg::MarkerArray, kNumMessages> g_marker_arrays;

static void init_marker_arrays()
{
  static bool done = false;
  if (done) {return;}
  done = true;
  for (size_t i = 0; i < kNumMessages; ++i) {
    auto & ma = g_marker_arrays[i];
    ma.markers.resize(kMarkersPerArray);
    for (size_t j = 0; j < kMarkersPerArray; ++j) {
      auto & m = ma.markers[j];
      m.header.frame_id = "benchmark_frame";
      m.ns = "benchmark_ns";
      m.id = static_cast<int32_t>(i * kMarkersPerArray + j);
      m.type = visualization_msgs::msg::Marker::POINTS;
      m.action = visualization_msgs::msg::Marker::ADD;
      m.text = "some label text";
      m.mesh_resource = "package://some_pkg/meshes/model.dae";
      m.points.resize(kPointsPerMarker);
      m.colors.resize(kPointsPerMarker);
      for (size_t k = 0; k < kPointsPerMarker; ++k) {
        m.points[k].x = static_cast<double>(k);
        m.points[k].y = static_cast<double>(k) * 0.5;
        m.points[k].z = 0.0;
        m.colors[k].r = 1.0f;
        m.colors[k].g = 0.5f;
        m.colors[k].b = 0.0f;
        m.colors[k].a = 1.0f;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Size-bound benchmarks (construction cost only — no data pointer needed).
// Construction is outside the loop; we benchmark min/max queries.
// ---------------------------------------------------------------------------
static void BM_OldImpl_SizeBound(
  benchmark::State & state,
  const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  auto writer = make_cdr_writer_old(members, SampleOrRequest::Sample);
  for (auto _ : state) {
    benchmark::DoNotOptimize(writer->get_min_serialized_size(SampleOrKey::Sample));
    benchmark::DoNotOptimize(writer->get_max_serialized_size(SampleOrKey::Sample));
  }
}

static void BM_NewImpl_SizeBound(
  benchmark::State & state,
  const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, SampleOrRequest::Sample);
  for (auto _ : state) {
    benchmark::DoNotOptimize(sizer.get_min_serialized_size(SampleOrKey::Sample));
    benchmark::DoNotOptimize(sizer.get_max_serialized_size(SampleOrKey::Sample));
  }
}

// ---------------------------------------------------------------------------
// get_serialized_size benchmarks — cycles through the pre-allocated array.
// ---------------------------------------------------------------------------
static void BM_OldImpl_SerializedSize(benchmark::State & state)
{
  init_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  auto writer = make_cdr_writer_old(members, SampleOrRequest::Sample);
  size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
      writer->get_serialized_size(&g_marker_arrays[idx], SampleOrKey::Sample));
    idx = (idx + 1) % kNumMessages;
  }
}

static void BM_NewImpl_SerializedSize(benchmark::State & state)
{
  init_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
      sizer.get_serialized_size(&g_marker_arrays[idx], SampleOrKey::Sample));
    idx = (idx + 1) % kNumMessages;
  }
}

// ---------------------------------------------------------------------------
// get_serialized_size_estimate benchmarks
// ---------------------------------------------------------------------------
static void BM_OldImpl_SerializedSizeEstimate(benchmark::State & state)
{
  init_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  auto writer = make_cdr_writer_old(members, SampleOrRequest::Sample);
  size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
      writer->get_serialized_size_estimate(&g_marker_arrays[idx], SampleOrKey::Sample));
    idx = (idx + 1) % kNumMessages;
  }
}

static void BM_NewImpl_SerializedSizeEstimate(benchmark::State & state)
{
  init_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  CDRSizer sizer(members, SampleOrRequest::Sample);
  size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
      sizer.get_serialized_size_estimate(&g_marker_arrays[idx], SampleOrKey::Sample));
    idx = (idx + 1) % kNumMessages;
  }
}

// ---------------------------------------------------------------------------
// Register benchmarks
// ---------------------------------------------------------------------------

#define REGISTER_BOUND(label, pkg, iface, name) \
  BENCHMARK_CAPTURE(BM_OldImpl_SizeBound, label, GET_TS(pkg, iface, name)); \
  BENCHMARK_CAPTURE(BM_NewImpl_SizeBound, label, GET_TS(pkg, iface, name));

REGISTER_BOUND(Bool,              std_msgs,           msg, Bool)
REGISTER_BOUND(Int32,             std_msgs,           msg, Int32)
REGISTER_BOUND(String,            std_msgs,           msg, String)
REGISTER_BOUND(Header,            std_msgs,           msg, Header)
REGISTER_BOUND(Float64MultiArray, std_msgs,           msg, Float64MultiArray)
REGISTER_BOUND(Marker,            visualization_msgs, msg, Marker)
REGISTER_BOUND(MarkerArray,       visualization_msgs, msg, MarkerArray)
REGISTER_BOUND(PointCloud2,       sensor_msgs,        msg, PointCloud2)
REGISTER_BOUND(LaserScan,         sensor_msgs,        msg, LaserScan)
REGISTER_BOUND(Image,             sensor_msgs,        msg, Image)

BENCHMARK(BM_OldImpl_SerializedSize);
BENCHMARK(BM_NewImpl_SerializedSize);
BENCHMARK(BM_OldImpl_SerializedSizeEstimate);
BENCHMARK(BM_NewImpl_SerializedSizeEstimate);

// ---------------------------------------------------------------------------
// serialize() benchmarks — pre-allocate output buffer outside loop
// ---------------------------------------------------------------------------
static void BM_OldImpl_Serialize(benchmark::State & state)
{
  init_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  auto writer = make_cdr_writer_old(members, SampleOrRequest::Sample);

  // Allocate worst-case buffer
  size_t max_size = writer->get_max_serialized_size(SampleOrKey::Sample);
  if (max_size == SIZE_MAX) {
    max_size = 16 * 1024 * 1024;  // 16 MB fallback for unbounded messages
  }
  std::vector<unsigned char> buf(max_size);

  size_t idx = 0;
  for (auto _ : state) {
    writer->serialize(buf.data(), &g_marker_arrays[idx], SampleOrKey::Sample);
    benchmark::DoNotOptimize(buf.data());
    idx = (idx + 1) % kNumMessages;
  }
}

static void BM_NewImpl_Serialize(benchmark::State & state)
{
  init_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  CDRSerializer serializer(members, SampleOrRequest::Sample);

  size_t max_size = serializer.get_max_serialized_size(SampleOrKey::Sample);
  if (max_size == SIZE_MAX) {
    max_size = 16 * 1024 * 1024;
  }
  std::vector<unsigned char> buf(max_size);

  size_t idx = 0;
  for (auto _ : state) {
    serializer.serialize(buf.data(), &g_marker_arrays[idx], SampleOrKey::Sample);
    benchmark::DoNotOptimize(buf.data());
    idx = (idx + 1) % kNumMessages;
  }
}

BENCHMARK(BM_OldImpl_Serialize);
BENCHMARK(BM_NewImpl_Serialize);

// ---------------------------------------------------------------------------
// deserialize() benchmarks — pre-serialize all messages, then time reading
// ---------------------------------------------------------------------------
static std::vector<std::vector<unsigned char>> g_serialized_marker_arrays;

static void init_serialized_marker_arrays()
{
  if (!g_serialized_marker_arrays.empty()) {
    return;
  }
  init_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  CDRSerializer serializer(members, SampleOrRequest::Sample);
  g_serialized_marker_arrays.resize(kNumMessages);
  for (size_t i = 0; i < kNumMessages; ++i) {
    size_t sz = serializer.get_serialized_size(&g_marker_arrays[i], SampleOrKey::Sample);
    g_serialized_marker_arrays[i].resize(sz);
    serializer.serialize(
      g_serialized_marker_arrays[i].data(), &g_marker_arrays[i], SampleOrKey::Sample);
  }
}

static void BM_NewImpl_Deserialize(benchmark::State & state)
{
  init_serialized_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  CDRDeserializer deserializer(members, SampleOrRequest::Sample);

  visualization_msgs::msg::MarkerArray msg;
  size_t idx = 0;
  for (auto _ : state) {
    const auto & buf = g_serialized_marker_arrays[idx];
    deserializer.deserialize(&msg, buf.data(), buf.size(), SampleOrKey::Sample);
    benchmark::DoNotOptimize(msg.markers.data());
    idx = (idx + 1) % kNumMessages;
  }
}

static void BM_OldImpl_Deserialize(benchmark::State & state)
{
  init_serialized_marker_arrays();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, MarkerArray));
  auto reader = make_cdr_reader_old(members, SampleOrRequest::Sample);

  visualization_msgs::msg::MarkerArray msg;
  size_t idx = 0;
  for (auto _ : state) {
    const auto & buf = g_serialized_marker_arrays[idx];
    reader->deserialize(&msg, buf.data(), buf.size(), SampleOrKey::Sample);
    benchmark::DoNotOptimize(msg.markers.data());
    idx = (idx + 1) % kNumMessages;
  }
}

BENCHMARK(BM_NewImpl_Deserialize);
BENCHMARK(BM_OldImpl_Deserialize);

// ---------------------------------------------------------------------------
// Single Marker serialize/deserialize benchmarks
// Each Marker has kPointsPerMarker points and colors.
// ---------------------------------------------------------------------------
static std::array<visualization_msgs::msg::Marker, kNumMessages> g_markers;

static void init_markers()
{
  static bool done = false;
  if (done) {return;}
  done = true;
  for (size_t i = 0; i < kNumMessages; ++i) {
    auto & m = g_markers[i];
    m.header.frame_id = "benchmark_frame";
    m.ns = "benchmark_ns";
    m.id = static_cast<int32_t>(i);
    m.type = visualization_msgs::msg::Marker::POINTS;
    m.action = visualization_msgs::msg::Marker::ADD;
    m.text = "some label text";
    m.mesh_resource = "package://some_pkg/meshes/model.dae";
    m.points.resize(kPointsPerMarker);
    m.colors.resize(kPointsPerMarker);
    for (size_t k = 0; k < kPointsPerMarker; ++k) {
      m.points[k].x = static_cast<double>(k);
      m.points[k].y = static_cast<double>(k) * 0.5;
      m.points[k].z = 0.0;
      m.colors[k].r = 1.0f;
      m.colors[k].g = 0.5f;
      m.colors[k].b = 0.0f;
      m.colors[k].a = 1.0f;
    }
  }
}

static std::vector<std::vector<unsigned char>> g_serialized_markers;

static void init_serialized_markers()
{
  if (!g_serialized_markers.empty()) {
    return;
  }
  init_markers();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, Marker));
  CDRSerializer serializer(members, SampleOrRequest::Sample);
  g_serialized_markers.resize(kNumMessages);
  for (size_t i = 0; i < kNumMessages; ++i) {
    size_t sz = serializer.get_serialized_size(&g_markers[i], SampleOrKey::Sample);
    g_serialized_markers[i].resize(sz);
    serializer.serialize(g_serialized_markers[i].data(), &g_markers[i], SampleOrKey::Sample);
  }
}

static void BM_Marker_Serialize_Old(benchmark::State & state)
{
  init_markers();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, Marker));
  auto writer = make_cdr_writer_old(members, SampleOrRequest::Sample);

  size_t max_size = writer->get_max_serialized_size(SampleOrKey::Sample);
  if (max_size == SIZE_MAX) {
    max_size = 1024 * 1024;
  }
  std::vector<unsigned char> buf(max_size);

  size_t idx = 0;
  for (auto _ : state) {
    writer->serialize(buf.data(), &g_markers[idx], SampleOrKey::Sample);
    benchmark::DoNotOptimize(buf.data());
    idx = (idx + 1) % kNumMessages;
  }
}

static void BM_Marker_Serialize(benchmark::State & state)
{
  init_markers();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, Marker));
  CDRSerializer serializer(members, SampleOrRequest::Sample);

  size_t max_size = serializer.get_max_serialized_size(SampleOrKey::Sample);
  if (max_size == SIZE_MAX) {
    max_size = 1024 * 1024;
  }
  std::vector<unsigned char> buf(max_size);

  size_t idx = 0;
  for (auto _ : state) {
    serializer.serialize(buf.data(), &g_markers[idx], SampleOrKey::Sample);
    benchmark::DoNotOptimize(buf.data());
    idx = (idx + 1) % kNumMessages;
  }
}

static void BM_Marker_Deserialize_Old(benchmark::State & state)
{
  init_serialized_markers();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, Marker));
  auto reader = make_cdr_reader_old(members, SampleOrRequest::Sample);

  visualization_msgs::msg::Marker msg;
  size_t idx = 0;
  for (auto _ : state) {
    const auto & buf = g_serialized_markers[idx];
    reader->deserialize(&msg, buf.data(), buf.size(), SampleOrKey::Sample);
    benchmark::DoNotOptimize(msg.points.data());
    idx = (idx + 1) % kNumMessages;
  }
}

static void BM_Marker_Deserialize(benchmark::State & state)
{
  init_serialized_markers();
  MessageMembersVariant members =
    make_message_members_variant(GET_TS(visualization_msgs, msg, Marker));
  CDRDeserializer deserializer(members, SampleOrRequest::Sample);

  visualization_msgs::msg::Marker msg;
  size_t idx = 0;
  for (auto _ : state) {
    const auto & buf = g_serialized_markers[idx];
    deserializer.deserialize(&msg, buf.data(), buf.size(), SampleOrKey::Sample);
    benchmark::DoNotOptimize(msg.points.data());
    idx = (idx + 1) % kNumMessages;
  }
}

BENCHMARK(BM_Marker_Serialize_Old);
BENCHMARK(BM_Marker_Serialize);
BENCHMARK(BM_Marker_Deserialize_Old);
BENCHMARK(BM_Marker_Deserialize);

// ===========================================================================
// sensor_msgs benchmarks
// ===========================================================================

// ---------------------------------------------------------------------------
// Message pools: 32 pre-populated instances of each type
// ---------------------------------------------------------------------------
static constexpr uint32_t kPC2Width = 640;   // points per row
static constexpr uint32_t kPC2Height = 480;  // rows (VGA-sized organized cloud)
static constexpr uint32_t kPC2PointStep = 32; // XYZ + intensity + ring (typical Velodyne)
static constexpr uint32_t kLaserBeams = 1080; // typical 270° / 0.25° scan
static constexpr uint32_t kImageWidth = 1280;
static constexpr uint32_t kImageHeight = 720;
static constexpr uint32_t kImageChannels = 3; // BGR8

static std::array<sensor_msgs::msg::PointCloud2, kNumMessages> g_pointclouds;
static std::array<sensor_msgs::msg::LaserScan,   kNumMessages> g_laser_scans;
static std::array<sensor_msgs::msg::Image,       kNumMessages> g_images;

static void init_sensor_msgs()
{
  static bool done = false;
  if (done) {return;}
  done = true;

  // PointCloud2: kPC2Width * kPC2Height points, each kPC2PointStep bytes
  for (size_t i = 0; i < kNumMessages; ++i) {
    auto & pc = g_pointclouds[i];
    pc.header.frame_id = "velodyne";
    pc.height = kPC2Height;
    pc.width  = kPC2Width;
    pc.point_step = kPC2PointStep;
    pc.row_step   = kPC2Width * kPC2PointStep;
    pc.is_bigendian = false;
    pc.is_dense = true;
    // 4 named fields: x, y, z, intensity
    const char * names[] = {"x", "y", "z", "intensity"};
    const uint8_t datatypes[] = {7, 7, 7, 7};  // FLOAT32 = 7
    const uint32_t offsets[] = {0, 4, 8, 12};
    pc.fields.resize(4);
    for (int f = 0; f < 4; ++f) {
      pc.fields[f].name     = names[f];
      pc.fields[f].offset   = offsets[f];
      pc.fields[f].datatype = datatypes[f];
      pc.fields[f].count    = 1;
    }
    pc.data.resize(static_cast<size_t>(kPC2Height) * kPC2Width * kPC2PointStep,
      static_cast<uint8_t>(i & 0xFF));
  }

  // LaserScan: kLaserBeams range + intensity readings
  for (size_t i = 0; i < kNumMessages; ++i) {
    auto & ls = g_laser_scans[i];
    ls.header.frame_id = "laser";
    ls.angle_min      = -2.356194f;  // -135°
    ls.angle_max      =  2.356194f;  // +135°
    ls.angle_increment = static_cast<float>(4.712389 / (kLaserBeams - 1));
    ls.time_increment  = 0.0f;
    ls.scan_time       = 0.1f;
    ls.range_min       = 0.05f;
    ls.range_max       = 30.0f;
    ls.ranges.resize(kLaserBeams, 5.0f);
    ls.intensities.resize(kLaserBeams, 100.0f);
  }

  // Image: kImageWidth × kImageHeight BGR8
  for (size_t i = 0; i < kNumMessages; ++i) {
    auto & img = g_images[i];
    img.header.frame_id = "camera";
    img.height   = kImageHeight;
    img.width    = kImageWidth;
    img.encoding = "bgr8";
    img.is_bigendian = 0;
    img.step     = kImageWidth * kImageChannels;
    img.data.resize(
      static_cast<size_t>(kImageHeight) * kImageWidth * kImageChannels,
      static_cast<uint8_t>(i & 0xFF));
  }
}

// ---------------------------------------------------------------------------
// Generic size / serialize / deserialize helpers templated on message type
// ---------------------------------------------------------------------------
template<typename MsgT>
static void BM_SensorMsg_SerializedSize_Old(
  benchmark::State & state,
  std::array<MsgT, kNumMessages> & pool,
  const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  auto writer = make_cdr_writer_old(members, SampleOrRequest::Sample);
  size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
      writer->get_serialized_size(&pool[idx], SampleOrKey::Sample));
    idx = (idx + 1) % kNumMessages;
  }
}

template<typename MsgT>
static void BM_SensorMsg_Serialize_Old(
  benchmark::State & state,
  std::array<MsgT, kNumMessages> & pool,
  const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  auto writer = make_cdr_writer_old(members, SampleOrRequest::Sample);
  size_t max_size = writer->get_max_serialized_size(SampleOrKey::Sample);
  if (max_size == SIZE_MAX) {
    max_size = 16 * 1024 * 1024;
  }
  std::vector<unsigned char> buf(max_size);
  size_t idx = 0;
  for (auto _ : state) {
    writer->serialize(buf.data(), &pool[idx], SampleOrKey::Sample);
    benchmark::DoNotOptimize(buf.data());
    idx = (idx + 1) % kNumMessages;
  }
  state.SetBytesProcessed(
    static_cast<int64_t>(state.iterations()) *
    static_cast<int64_t>(writer->get_serialized_size(&pool[0], SampleOrKey::Sample)));
}

template<typename MsgT>
static void BM_SensorMsg_Deserialize_Old(
  benchmark::State & state,
  std::array<MsgT, kNumMessages> & pool,
  const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  auto writer = make_cdr_writer_old(members, SampleOrRequest::Sample);
  auto reader = make_cdr_reader_old(members, SampleOrRequest::Sample);

  std::vector<std::vector<unsigned char>> bufs(kNumMessages);
  for (size_t i = 0; i < kNumMessages; ++i) {
    size_t sz = writer->get_serialized_size(&pool[i], SampleOrKey::Sample);
    bufs[i].resize(sz);
    writer->serialize(bufs[i].data(), &pool[i], SampleOrKey::Sample);
  }

  MsgT msg;
  size_t idx = 0;
  for (auto _ : state) {
    const auto & buf = bufs[idx];
    reader->deserialize(&msg, buf.data(), buf.size(), SampleOrKey::Sample);
    benchmark::DoNotOptimize(&msg);
    idx = (idx + 1) % kNumMessages;
  }
  state.SetBytesProcessed(
    static_cast<int64_t>(state.iterations()) *
    static_cast<int64_t>(bufs[0].size()));
}

template<typename MsgT>
static void BM_SensorMsg_SerializedSize(
  benchmark::State & state,
  std::array<MsgT, kNumMessages> & pool,
  const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSizer sizer(members, SampleOrRequest::Sample);
  size_t idx = 0;
  for (auto _ : state) {
    benchmark::DoNotOptimize(
      sizer.get_serialized_size(&pool[idx], SampleOrKey::Sample));
    idx = (idx + 1) % kNumMessages;
  }
}

template<typename MsgT>
static void BM_SensorMsg_Serialize(
  benchmark::State & state,
  std::array<MsgT, kNumMessages> & pool,
  const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSerializer serializer(members, SampleOrRequest::Sample);
  // size off first element — all are the same size
  size_t sz = serializer.get_serialized_size(&pool[0], SampleOrKey::Sample);
  std::vector<unsigned char> buf(sz);
  size_t idx = 0;
  for (auto _ : state) {
    serializer.serialize(buf.data(), &pool[idx], SampleOrKey::Sample);
    benchmark::DoNotOptimize(buf.data());
    idx = (idx + 1) % kNumMessages;
  }
  state.SetBytesProcessed(
    static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(sz));
}

template<typename MsgT>
static void BM_SensorMsg_Deserialize(
  benchmark::State & state,
  std::array<MsgT, kNumMessages> & pool,
  const rosidl_message_type_support_t * ts)
{
  MessageMembersVariant members = make_message_members_variant(ts);
  CDRSerializer serializer(members, SampleOrRequest::Sample);
  CDRDeserializer deserializer(members, SampleOrRequest::Sample);

  std::vector<std::vector<unsigned char>> bufs(kNumMessages);
  for (size_t i = 0; i < kNumMessages; ++i) {
    size_t sz = serializer.get_serialized_size(&pool[i], SampleOrKey::Sample);
    bufs[i].resize(sz);
    serializer.serialize(bufs[i].data(), &pool[i], SampleOrKey::Sample);
  }

  MsgT msg;
  size_t idx = 0;
  for (auto _ : state) {
    const auto & buf = bufs[idx];
    deserializer.deserialize(&msg, buf.data(), buf.size(), SampleOrKey::Sample);
    benchmark::DoNotOptimize(&msg);
    idx = (idx + 1) % kNumMessages;
  }
  state.SetBytesProcessed(
    static_cast<int64_t>(state.iterations()) *
    static_cast<int64_t>(bufs[0].size()));
}

// Helper macro: register old+new sensor msg benchmarks for one message type.
#define REGISTER_SENSOR(label, pool, pkg, iface, name) \
  static void BM_ ## label ## _SerializedSize_Old(benchmark::State & state) \
  { \
    init_sensor_msgs(); \
    BM_SensorMsg_SerializedSize_Old(state, pool, GET_TS(pkg, iface, name)); \
  } \
  static void BM_ ## label ## _SerializedSize(benchmark::State & state) \
  { \
    init_sensor_msgs(); \
    BM_SensorMsg_SerializedSize(state, pool, GET_TS(pkg, iface, name)); \
  } \
  static void BM_ ## label ## _Serialize_Old(benchmark::State & state) \
  { \
    init_sensor_msgs(); \
    BM_SensorMsg_Serialize_Old(state, pool, GET_TS(pkg, iface, name)); \
  } \
  static void BM_ ## label ## _Serialize(benchmark::State & state) \
  { \
    init_sensor_msgs(); \
    BM_SensorMsg_Serialize(state, pool, GET_TS(pkg, iface, name)); \
  } \
  static void BM_ ## label ## _Deserialize_Old(benchmark::State & state) \
  { \
    init_sensor_msgs(); \
    BM_SensorMsg_Deserialize_Old(state, pool, GET_TS(pkg, iface, name)); \
  } \
  static void BM_ ## label ## _Deserialize(benchmark::State & state) \
  { \
    init_sensor_msgs(); \
    BM_SensorMsg_Deserialize(state, pool, GET_TS(pkg, iface, name)); \
  } \
  BENCHMARK(BM_ ## label ## _SerializedSize_Old); \
  BENCHMARK(BM_ ## label ## _SerializedSize); \
  BENCHMARK(BM_ ## label ## _Serialize_Old); \
  BENCHMARK(BM_ ## label ## _Serialize); \
  BENCHMARK(BM_ ## label ## _Deserialize_Old); \
  BENCHMARK(BM_ ## label ## _Deserialize);

REGISTER_SENSOR(PointCloud2, g_pointclouds, sensor_msgs, msg, PointCloud2)
REGISTER_SENSOR(LaserScan,   g_laser_scans, sensor_msgs, msg, LaserScan)
REGISTER_SENSOR(Image,       g_images,      sensor_msgs, msg, Image)

BENCHMARK_MAIN();
