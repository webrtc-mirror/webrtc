/*
 *  Copyright (c) 2022 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "test/pc/e2e/stats_based_network_quality_metrics_reporter.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/test/create_network_emulation_manager.h"
#include "api/test/metrics/metric.h"
#include "api/test/metrics/metrics_logger.h"
#include "api/test/network_emulation/network_emulation_interfaces.h"
#include "api/test/network_emulation_manager.h"
#include "api/test/pclf/media_configuration.h"
#include "api/test/pclf/media_quality_test_params.h"
#include "api/test/pclf/peer_configurer.h"
#include "api/test/peerconnection_quality_test_fixture.h"
#include "api/test/simulated_network.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "media/base/media_constants.h"
#include "test/gmock.h"
#include "test/gtest.h"
#include "test/pc/e2e/peer_connection_quality_test.h"

namespace webrtc {
namespace webrtc_pc_e2e {
namespace {

using ::testing::UnorderedElementsAre;

using test::DefaultMetricsLogger;
using test::ImprovementDirection;
using test::Metric;
using test::Unit;
using webrtc_pc_e2e::PeerConfigurer;

// Adds a peer with some audio and video (the client should not care about
// details about audio and video configs).
void AddDefaultAudioVideoPeer(absl::string_view peer_name,
                              absl::string_view audio_stream_label,
                              absl::string_view video_stream_label,
                              EmulatedNetworkManagerInterface& network,
                              PeerConnectionE2EQualityTestFixture& fixture) {
  AudioConfig audio{.stream_label = std::string(audio_stream_label),
                    .sync_group = std::string(peer_name)};
  VideoConfig video(std::string(video_stream_label), 320, 180, 15);
  video.sync_group = std::string(peer_name);
  auto peer = std::make_unique<PeerConfigurer>(network);
  peer->SetName(peer_name);
  peer->SetAudioConfig(std::move(audio));
  peer->AddVideoConfig(std::move(video));
  peer->SetVideoCodecs({VideoCodecConfig(kVp8CodecName)});
  fixture.AddPeer(std::move(peer));
}

std::optional<Metric> FindMeetricByName(absl::string_view name,
                                        ArrayView<const Metric> metrics) {
  for (const Metric& metric : metrics) {
    if (metric.name == name) {
      return metric;
    }
  }
  return std::nullopt;
}

TEST(StatsBasedNetworkQualityMetricsReporterTest, DebugStatsAreCollected) {
  std::unique_ptr<NetworkEmulationManager> network_emulation =
      CreateNetworkEmulationManager(
          {.time_mode = TimeMode::kSimulated,
           .stats_gathering_mode = EmulatedNetworkStatsGatheringMode::kDebug});
  DefaultMetricsLogger metrics_logger(
      network_emulation->time_controller()->GetClock());
  PeerConnectionE2EQualityTest fixture(
      "test_case", *network_emulation->time_controller(),
      /*audio_quality_analyzer=*/nullptr, /*video_quality_analyzer=*/nullptr,
      &metrics_logger);

  EmulatedEndpoint* alice_endpoint =
      network_emulation->CreateEndpoint(EmulatedEndpointConfig());
  EmulatedEndpoint* bob_endpoint =
      network_emulation->CreateEndpoint(EmulatedEndpointConfig());

  EmulatedNetworkNode* alice_link =
      network_emulation->CreateEmulatedNode(BuiltInNetworkBehaviorConfig{
          .link_capacity = DataRate::KilobitsPerSec(500)});
  network_emulation->CreateRoute(alice_endpoint, {alice_link}, bob_endpoint);
  EmulatedNetworkNode* bob_link =
      network_emulation->CreateEmulatedNode(BuiltInNetworkBehaviorConfig{
          .link_capacity = DataRate::KilobitsPerSec(500)});
  network_emulation->CreateRoute(bob_endpoint, {bob_link}, alice_endpoint);

  EmulatedNetworkManagerInterface* alice_network =
      network_emulation->CreateEmulatedNetworkManagerInterface(
          {alice_endpoint});
  EmulatedNetworkManagerInterface* bob_network =
      network_emulation->CreateEmulatedNetworkManagerInterface({bob_endpoint});

  AddDefaultAudioVideoPeer("alice", "alice_audio", "alice_video",
                           *alice_network, fixture);
  AddDefaultAudioVideoPeer("bob", "bob_audio", "bob_video", *bob_network,
                           fixture);

  auto network_stats_reporter =
      std::make_unique<StatsBasedNetworkQualityMetricsReporter>(
          /*peer_endpoints=*/std::map<std::string,
                                      std::vector<EmulatedEndpoint*>>{},
          network_emulation.get(), &metrics_logger);
  network_stats_reporter->AddPeer("alice", alice_network->endpoints(),
                                  /*uplink=*/{alice_link},
                                  /*downlink=*/{bob_link});
  network_stats_reporter->AddPeer("bob", bob_network->endpoints(),
                                  /*uplink=*/{bob_link},
                                  /*downlink=*/{alice_link});
  fixture.AddQualityMetricsReporter(std::move(network_stats_reporter));

  fixture.Run(RunParams(TimeDelta::Seconds(4)));

  std::vector<Metric> metrics = metrics_logger.GetCollectedMetrics();
  std::optional<Metric> uplink_packet_transport_time =
      FindMeetricByName("uplink_packet_transport_time", metrics);
  ASSERT_TRUE(uplink_packet_transport_time.has_value());
  ASSERT_FALSE(uplink_packet_transport_time->time_series.samples.empty());
  std::optional<Metric> uplink_size_to_packet_transport_time =
      FindMeetricByName("uplink_size_to_packet_transport_time", metrics);
  ASSERT_TRUE(uplink_size_to_packet_transport_time.has_value());
  ASSERT_FALSE(
      uplink_size_to_packet_transport_time->time_series.samples.empty());
  std::optional<Metric> downlink_packet_transport_time =
      FindMeetricByName("downlink_packet_transport_time", metrics);
  ASSERT_TRUE(downlink_packet_transport_time.has_value());
  ASSERT_FALSE(downlink_packet_transport_time->time_series.samples.empty());
  std::optional<Metric> downlink_size_to_packet_transport_time =
      FindMeetricByName("downlink_size_to_packet_transport_time", metrics);
  ASSERT_TRUE(downlink_size_to_packet_transport_time.has_value());
  ASSERT_FALSE(
      downlink_size_to_packet_transport_time->time_series.samples.empty());
}

}  // namespace
}  // namespace webrtc_pc_e2e
}  // namespace webrtc
