/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "call/rampup_tests.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/strings/string_view.h"
#include "api/field_trials_view.h"
#include "api/make_ref_counted.h"
#include "api/rtc_event_log/rtc_event_log.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "api/rtc_event_log_output_file.h"
#include "api/rtp_parameters.h"
#include "api/sequence_checker.h"
#include "api/task_queue/task_queue_base.h"
#include "api/test/metrics/global_metrics_logger_and_exporter.h"
#include "api/test/metrics/metric.h"
#include "api/test/simulated_network.h"
#include "api/transport/bitrate_settings.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/video/video_codec_type.h"
#include "api/video_codecs/sdp_video_format.h"
#include "call/audio_receive_stream.h"
#include "call/audio_send_stream.h"
#include "call/call.h"
#include "call/fake_network_pipe.h"
#include "call/flexfec_receive_stream.h"
#include "call/video_receive_stream.h"
#include "call/video_send_stream.h"
#include "rtc_base/checks.h"
#include "rtc_base/string_encode.h"
#include "rtc_base/task_queue_for_test.h"
#include "rtc_base/task_utils/repeating_task.h"
#include "test/call_test.h"
#include "test/encoder_settings.h"
#include "test/gtest.h"
#include "test/rtp_rtcp_observer.h"
#include "test/video_test_constants.h"
#include "video/config/video_encoder_config.h"

ABSL_FLAG(std::string,
          ramp_dump_name,
          "",
          "Filename for dumped received RTP stream.");

namespace webrtc {
namespace {

using test::GetGlobalMetricsLogger;
using test::ImprovementDirection;
using test::Unit;

constexpr TimeDelta kPollInterval = TimeDelta::Millis(20);
const int kExpectedHighVideoBitrateBps = 80000;
const int kExpectedHighAudioBitrateBps = 30000;
const int kLowBandwidthLimitBps = 20000;
// Set target detected bitrate to slightly larger than the target bitrate to
// avoid flakiness.
const int kLowBitrateMarginBps = 2000;

std::vector<uint32_t> GenerateSsrcs(size_t num_streams, uint32_t ssrc_offset) {
  std::vector<uint32_t> ssrcs;
  for (size_t i = 0; i != num_streams; ++i)
    ssrcs.push_back(static_cast<uint32_t>(ssrc_offset + i));
  return ssrcs;
}

}  // namespace

RampUpTester::RampUpTester(size_t num_video_streams,
                           size_t num_audio_streams,
                           size_t num_flexfec_streams,
                           unsigned int start_bitrate_bps,
                           int64_t min_run_time_ms,
                           bool rtx,
                           bool red,
                           bool report_perf_stats,
                           TaskQueueBase* task_queue)
    : EndToEndTest(test::VideoTestConstants::kLongTimeout),
      clock_(Clock::GetRealTimeClock()),
      num_video_streams_(num_video_streams),
      num_audio_streams_(num_audio_streams),
      num_flexfec_streams_(num_flexfec_streams),
      rtx_(rtx),
      red_(red),
      report_perf_stats_(report_perf_stats),
      sender_call_(nullptr),
      send_stream_(nullptr),
      send_transport_(nullptr),
      send_simulated_network_(nullptr),
      start_bitrate_bps_(start_bitrate_bps),
      min_run_time_ms_(min_run_time_ms),
      expected_bitrate_bps_(0),
      test_start_ms_(-1),
      ramp_up_finished_ms_(-1),
      video_ssrcs_(GenerateSsrcs(num_video_streams_, 100)),
      video_rtx_ssrcs_(GenerateSsrcs(num_video_streams_, 200)),
      audio_ssrcs_(GenerateSsrcs(num_audio_streams_, 300)),
      task_queue_(task_queue) {
  if (red_)
    EXPECT_EQ(0u, num_flexfec_streams_);
  EXPECT_LE(num_audio_streams_, 1u);
}

RampUpTester::~RampUpTester() = default;

void RampUpTester::ModifySenderBitrateConfig(
    BitrateConstraints* bitrate_config) {
  if (start_bitrate_bps_ != 0) {
    bitrate_config->start_bitrate_bps = start_bitrate_bps_;
  }
  bitrate_config->min_bitrate_bps = 10000;
}

void RampUpTester::OnVideoStreamsCreated(
    VideoSendStream* send_stream,
    const std::vector<VideoReceiveStreamInterface*>& /* receive_streams */) {
  send_stream_ = send_stream;
}

BuiltInNetworkBehaviorConfig RampUpTester::GetSendTransportConfig() const {
  return forward_transport_config_;
}

size_t RampUpTester::GetNumVideoStreams() const {
  return num_video_streams_;
}

size_t RampUpTester::GetNumAudioStreams() const {
  return num_audio_streams_;
}

size_t RampUpTester::GetNumFlexfecStreams() const {
  return num_flexfec_streams_;
}

class RampUpTester::VideoStreamFactory
    : public VideoEncoderConfig::VideoStreamFactoryInterface {
 public:
  VideoStreamFactory() {}

 private:
  std::vector<VideoStream> CreateEncoderStreams(
      const FieldTrialsView& /*field_trials*/,
      int frame_width,
      int frame_height,
      const VideoEncoderConfig& encoder_config) override {
    std::vector<VideoStream> streams =
        test::CreateVideoStreams(frame_width, frame_height, encoder_config);
    if (encoder_config.number_of_streams == 1) {
      streams[0].target_bitrate_bps = streams[0].max_bitrate_bps = 2000000;
    }
    return streams;
  }
};

void RampUpTester::ModifyVideoConfigs(
    VideoSendStream::Config* send_config,
    std::vector<VideoReceiveStreamInterface::Config>* receive_configs,
    VideoEncoderConfig* encoder_config) {
  send_config->suspend_below_min_bitrate = true;
  encoder_config->number_of_streams = num_video_streams_;
  encoder_config->max_bitrate_bps = 2000000;
  encoder_config->video_stream_factory =
      make_ref_counted<RampUpTester::VideoStreamFactory>();
  if (num_video_streams_ == 1) {
    // For single stream rampup until 1mbps
    expected_bitrate_bps_ = kSingleStreamTargetBps;
  } else {
    // To ensure simulcast rate allocation.
    send_config->rtp.payload_name = "VP8";
    encoder_config->codec_type = kVideoCodecVP8;
    std::vector<VideoStream> streams = test::CreateVideoStreams(
        test::VideoTestConstants::kDefaultWidth,
        test::VideoTestConstants::kDefaultHeight, *encoder_config);
    // For multi stream rampup until all streams are being sent. That means
    // enough bitrate to send all the target streams plus the min bitrate of
    // the last one.
    expected_bitrate_bps_ = streams.back().min_bitrate_bps;
    for (size_t i = 0; i < streams.size() - 1; ++i) {
      expected_bitrate_bps_ += streams[i].target_bitrate_bps;
    }
  }

  send_config->rtp.nack.rtp_history_ms =
      test::VideoTestConstants::kNackRtpHistoryMs;
  send_config->rtp.ssrcs = video_ssrcs_;
  if (rtx_) {
    send_config->rtp.rtx.payload_type =
        test::VideoTestConstants::kSendRtxPayloadType;
    send_config->rtp.rtx.ssrcs = video_rtx_ssrcs_;
  }
  if (red_) {
    send_config->rtp.ulpfec.ulpfec_payload_type =
        test::VideoTestConstants::kUlpfecPayloadType;
    send_config->rtp.ulpfec.red_payload_type =
        test::VideoTestConstants::kRedPayloadType;
    if (rtx_) {
      send_config->rtp.ulpfec.red_rtx_payload_type =
          test::VideoTestConstants::kRtxRedPayloadType;
    }
  }

  size_t i = 0;
  for (VideoReceiveStreamInterface::Config& recv_config : *receive_configs) {
    recv_config.decoders.reserve(1);
    recv_config.decoders[0].payload_type = send_config->rtp.payload_type;
    recv_config.decoders[0].video_format =
        SdpVideoFormat(send_config->rtp.payload_name);

    recv_config.rtp.remote_ssrc = video_ssrcs_[i];
    recv_config.rtp.nack.rtp_history_ms = send_config->rtp.nack.rtp_history_ms;

    if (red_) {
      recv_config.rtp.red_payload_type =
          send_config->rtp.ulpfec.red_payload_type;
      recv_config.rtp.ulpfec_payload_type =
          send_config->rtp.ulpfec.ulpfec_payload_type;
      if (rtx_) {
        recv_config.rtp.rtx_associated_payload_types
            [send_config->rtp.ulpfec.red_rtx_payload_type] =
            send_config->rtp.ulpfec.red_payload_type;
      }
    }

    if (rtx_) {
      recv_config.rtp.rtx_ssrc = video_rtx_ssrcs_[i];
      recv_config.rtp
          .rtx_associated_payload_types[send_config->rtp.rtx.payload_type] =
          send_config->rtp.payload_type;
    }
    ++i;
  }

  RTC_DCHECK_LE(num_flexfec_streams_, 1);
  if (num_flexfec_streams_ == 1) {
    send_config->rtp.flexfec.payload_type =
        test::VideoTestConstants::kFlexfecPayloadType;
    send_config->rtp.flexfec.ssrc = test::VideoTestConstants::kFlexfecSendSsrc;
    send_config->rtp.flexfec.protected_media_ssrcs = {video_ssrcs_[0]};
  }
}

void RampUpTester::ModifyAudioConfigs(
    AudioSendStream::Config* send_config,
    std::vector<AudioReceiveStreamInterface::Config>* receive_configs) {
  if (num_audio_streams_ == 0)
    return;

  send_config->rtp.ssrc = audio_ssrcs_[0];
  send_config->min_bitrate_bps = 6000;
  send_config->max_bitrate_bps = 60000;

  for (AudioReceiveStreamInterface::Config& recv_config : *receive_configs) {
    recv_config.rtp.remote_ssrc = send_config->rtp.ssrc;
  }
}

void RampUpTester::ModifyFlexfecConfigs(
    std::vector<FlexfecReceiveStream::Config>* receive_configs) {
  if (num_flexfec_streams_ == 0)
    return;
  RTC_DCHECK_EQ(1, num_flexfec_streams_);
  (*receive_configs)[0].payload_type =
      test::VideoTestConstants::kFlexfecPayloadType;
  (*receive_configs)[0].rtp.remote_ssrc =
      test::VideoTestConstants::kFlexfecSendSsrc;
  (*receive_configs)[0].protected_media_ssrcs = {video_ssrcs_[0]};
  (*receive_configs)[0].rtp.local_ssrc = video_ssrcs_[0];
}

void RampUpTester::OnCallsCreated(Call* sender_call,
                                  Call* /* receiver_call */) {
  RTC_DCHECK(sender_call);
  sender_call_ = sender_call;
  pending_task_ = RepeatingTaskHandle::Start(task_queue_, [this] {
    PollStats();
    return kPollInterval;
  });
}

void RampUpTester::OnTransportCreated(
    test::PacketTransport* to_receiver,
    SimulatedNetworkInterface* sender_network,
    test::PacketTransport* /* to_sender */,
    SimulatedNetworkInterface* /* receiver_network */) {
  RTC_DCHECK_RUN_ON(task_queue_);

  send_transport_ = to_receiver;
  send_simulated_network_ = sender_network;
}

void RampUpTester::PollStats() {
  RTC_DCHECK_RUN_ON(task_queue_);

  Call::Stats stats = sender_call_->GetStats();
  EXPECT_GE(expected_bitrate_bps_, 0);

  if (stats.send_bandwidth_bps >= expected_bitrate_bps_ &&
      (min_run_time_ms_ == -1 ||
       clock_->TimeInMilliseconds() - test_start_ms_ >= min_run_time_ms_)) {
    ramp_up_finished_ms_ = clock_->TimeInMilliseconds();
    observation_complete_.Set();
    pending_task_.Stop();
  }
}

void RampUpTester::ReportResult(
    absl::string_view measurement,
    size_t value,
    Unit unit,
    ImprovementDirection improvement_direction) const {
  GetGlobalMetricsLogger()->LogSingleValueMetric(
      measurement,
      ::testing::UnitTest::GetInstance()->current_test_info()->name(), value,
      unit, improvement_direction);
}

void RampUpTester::AccumulateStats(const VideoSendStream::StreamStats& stream,
                                   size_t* total_packets_sent,
                                   size_t* total_sent,
                                   size_t* padding_sent,
                                   size_t* media_sent) const {
  *total_packets_sent += stream.rtp_stats.transmitted.packets +
                         stream.rtp_stats.retransmitted.packets +
                         stream.rtp_stats.fec.packets;
  *total_sent += stream.rtp_stats.transmitted.TotalBytes() +
                 stream.rtp_stats.retransmitted.TotalBytes() +
                 stream.rtp_stats.fec.TotalBytes();
  *padding_sent += stream.rtp_stats.transmitted.padding_bytes +
                   stream.rtp_stats.retransmitted.padding_bytes +
                   stream.rtp_stats.fec.padding_bytes;
  *media_sent += stream.rtp_stats.MediaPayloadBytes();
}

void RampUpTester::TriggerTestDone() {
  RTC_DCHECK_GE(test_start_ms_, 0);

  // Stop polling stats.
  // Corner case for webrtc_quick_perf_test
  SendTask(task_queue_, [this] { pending_task_.Stop(); });

  // TODO(holmer): Add audio send stats here too when those APIs are available.
  if (!send_stream_)
    return;

  VideoSendStream::Stats send_stats;
  SendTask(task_queue_, [&] { send_stats = send_stream_->GetStats(); });

  send_stream_ = nullptr;  // To avoid dereferencing a bad pointer.

  size_t total_packets_sent = 0;
  size_t total_sent = 0;
  size_t padding_sent = 0;
  size_t media_sent = 0;
  for (uint32_t ssrc : video_ssrcs_) {
    AccumulateStats(send_stats.substreams[ssrc], &total_packets_sent,
                    &total_sent, &padding_sent, &media_sent);
  }

  size_t rtx_total_packets_sent = 0;
  size_t rtx_total_sent = 0;
  size_t rtx_padding_sent = 0;
  size_t rtx_media_sent = 0;
  for (uint32_t rtx_ssrc : video_rtx_ssrcs_) {
    AccumulateStats(send_stats.substreams[rtx_ssrc], &rtx_total_packets_sent,
                    &rtx_total_sent, &rtx_padding_sent, &rtx_media_sent);
  }

  if (report_perf_stats_) {
    ReportResult("ramp-up-media-sent", media_sent, Unit::kBytes,
                 ImprovementDirection::kBiggerIsBetter);
    ReportResult("ramp-up-padding-sent", padding_sent, Unit::kBytes,
                 ImprovementDirection::kSmallerIsBetter);
    ReportResult("ramp-up-rtx-media-sent", rtx_media_sent, Unit::kBytes,
                 ImprovementDirection::kBiggerIsBetter);
    ReportResult("ramp-up-rtx-padding-sent", rtx_padding_sent, Unit::kBytes,
                 ImprovementDirection::kSmallerIsBetter);
    if (ramp_up_finished_ms_ >= 0) {
      ReportResult("ramp-up-time", ramp_up_finished_ms_ - test_start_ms_,
                   Unit::kMilliseconds, ImprovementDirection::kSmallerIsBetter);
    }
    ReportResult("ramp-up-average-network-latency",
                 send_transport_->GetAverageDelayMs(), Unit::kMilliseconds,
                 ImprovementDirection::kSmallerIsBetter);
  }
}

void RampUpTester::PerformTest() {
  test_start_ms_ = clock_->TimeInMilliseconds();
  EXPECT_TRUE(Wait()) << "Timed out while waiting for ramp-up to complete.";
  TriggerTestDone();
}

RampUpDownUpTester::RampUpDownUpTester(size_t num_video_streams,
                                       size_t num_audio_streams,
                                       size_t num_flexfec_streams,
                                       unsigned int start_bitrate_bps,
                                       bool rtx,
                                       bool red,
                                       const std::vector<int>& loss_rates,
                                       bool report_perf_stats,
                                       TaskQueueBase* task_queue)
    : RampUpTester(num_video_streams,
                   num_audio_streams,
                   num_flexfec_streams,
                   start_bitrate_bps,
                   0,
                   rtx,
                   red,
                   report_perf_stats,
                   task_queue),
      link_rates_({4 * GetExpectedHighBitrate() / (3 * 1000),
                   kLowBandwidthLimitBps / 1000,
                   4 * GetExpectedHighBitrate() / (3 * 1000), 0}),
      test_state_(kFirstRampup),
      next_state_(kTransitionToNextState),
      state_start_ms_(clock_->TimeInMilliseconds()),
      interval_start_ms_(clock_->TimeInMilliseconds()),
      sent_bytes_(0),
      loss_rates_(loss_rates) {
  forward_transport_config_.link_capacity =
      DataRate::KilobitsPerSec(link_rates_[test_state_]);
  forward_transport_config_.queue_delay_ms = 100;
  forward_transport_config_.loss_percent = loss_rates_[test_state_];
}

RampUpDownUpTester::~RampUpDownUpTester() {}

void RampUpDownUpTester::PollStats() {
  if (test_state_ == kTestEnd) {
    pending_task_.Stop();
  }

  int transmit_bitrate_bps = 0;
  bool suspended = false;
  if (num_video_streams_ > 0 && send_stream_) {
    VideoSendStream::Stats stats = send_stream_->GetStats();
    for (const auto& it : stats.substreams) {
      transmit_bitrate_bps += it.second.total_bitrate_bps;
    }
    suspended = stats.suspended;
  }
  if (num_audio_streams_ > 0 && sender_call_) {
    // An audio send stream doesn't have bitrate stats, so the call send BW is
    // currently used instead.
    transmit_bitrate_bps = sender_call_->GetStats().send_bandwidth_bps;
  }

  EvolveTestState(transmit_bitrate_bps, suspended);
}

void RampUpDownUpTester::ModifyReceiverBitrateConfig(
    BitrateConstraints* bitrate_config) {
  bitrate_config->min_bitrate_bps = 10000;
}

std::string RampUpDownUpTester::GetModifierString() const {
  std::string str("_");
  if (num_video_streams_ > 0) {
    str += absl::StrCat(num_video_streams_);
    str += "stream";
    str += (num_video_streams_ > 1 ? "s" : "");
    str += "_";
  }
  if (num_audio_streams_ > 0) {
    str += absl::StrCat(num_audio_streams_);
    str += "stream";
    str += (num_audio_streams_ > 1 ? "s" : "");
    str += "_";
  }
  str += (rtx_ ? "" : "no");
  str += "rtx_";
  str += (red_ ? "" : "no");
  str += "red";
  return str;
}

int RampUpDownUpTester::GetExpectedHighBitrate() const {
  int expected_bitrate_bps = 0;
  if (num_audio_streams_ > 0)
    expected_bitrate_bps += kExpectedHighAudioBitrateBps;
  if (num_video_streams_ > 0)
    expected_bitrate_bps += kExpectedHighVideoBitrateBps;
  return expected_bitrate_bps;
}

size_t RampUpDownUpTester::GetFecBytes() const {
  size_t flex_fec_bytes = 0;
  if (num_flexfec_streams_ > 0) {
    VideoSendStream::Stats stats = send_stream_->GetStats();
    for (const auto& kv : stats.substreams)
      flex_fec_bytes += kv.second.rtp_stats.fec.TotalBytes();
  }
  return flex_fec_bytes;
}

bool RampUpDownUpTester::ExpectingFec() const {
  return num_flexfec_streams_ > 0 && forward_transport_config_.loss_percent > 0;
}

void RampUpDownUpTester::EvolveTestState(int bitrate_bps, bool suspended) {
  int64_t now = clock_->TimeInMilliseconds();
  switch (test_state_) {
    case kFirstRampup:
      EXPECT_FALSE(suspended);
      if (bitrate_bps >= GetExpectedHighBitrate()) {
        if (report_perf_stats_) {
          GetGlobalMetricsLogger()->LogSingleValueMetric(
              "ramp_up_down_up" + GetModifierString(), "first_rampup",
              now - state_start_ms_, Unit::kMilliseconds,
              ImprovementDirection::kSmallerIsBetter);
        }
        // Apply loss during the transition between states if FEC is enabled.
        forward_transport_config_.loss_percent = loss_rates_[test_state_];
        test_state_ = kTransitionToNextState;
        next_state_ = kLowRate;
      }
      break;
    case kLowRate: {
      // Audio streams are never suspended.
      bool check_suspend_state = num_video_streams_ > 0;
      if (bitrate_bps < kLowBandwidthLimitBps + kLowBitrateMarginBps &&
          suspended == check_suspend_state) {
        if (report_perf_stats_) {
          GetGlobalMetricsLogger()->LogSingleValueMetric(
              "ramp_up_down_up" + GetModifierString(), "rampdown",
              now - state_start_ms_, Unit::kMilliseconds,
              ImprovementDirection::kSmallerIsBetter);
        }
        // Apply loss during the transition between states if FEC is enabled.
        forward_transport_config_.loss_percent = loss_rates_[test_state_];
        test_state_ = kTransitionToNextState;
        next_state_ = kSecondRampup;
      }
      break;
    }
    case kSecondRampup:
      if (bitrate_bps >= GetExpectedHighBitrate() && !suspended) {
        if (report_perf_stats_) {
          GetGlobalMetricsLogger()->LogSingleValueMetric(
              "ramp_up_down_up" + GetModifierString(), "second_rampup",
              now - state_start_ms_, Unit::kMilliseconds,
              ImprovementDirection::kSmallerIsBetter);
          ReportResult("ramp-up-down-up-average-network-latency",
                       send_transport_->GetAverageDelayMs(),
                       Unit::kMilliseconds,
                       ImprovementDirection::kSmallerIsBetter);
        }
        // Apply loss during the transition between states if FEC is enabled.
        forward_transport_config_.loss_percent = loss_rates_[test_state_];
        test_state_ = kTransitionToNextState;
        next_state_ = kTestEnd;
      }
      break;
    case kTestEnd:
      observation_complete_.Set();
      break;
    case kTransitionToNextState:
      if (!ExpectingFec() || GetFecBytes() > 0) {
        test_state_ = next_state_;
        forward_transport_config_.link_capacity =
            DataRate::KilobitsPerSec(link_rates_[test_state_]);
        // No loss while ramping up and down as it may affect the BWE
        // negatively, making the test flaky.
        forward_transport_config_.loss_percent = 0;
        state_start_ms_ = now;
        interval_start_ms_ = now;
        sent_bytes_ = 0;
        send_simulated_network_->SetConfig(forward_transport_config_);
      }
      break;
  }
}

class RampUpTest : public test::CallTest {
 public:
  RampUpTest() {
    std::string dump_name(absl::GetFlag(FLAGS_ramp_dump_name));
    if (!dump_name.empty()) {
      std::unique_ptr<RtcEventLog> send_event_log =
          rtc_event_log_factory_.Create(env());
      std::unique_ptr<RtcEventLog> recv_event_log =
          rtc_event_log_factory_.Create(env());
      bool event_log_started =
          send_event_log->StartLogging(
              std::make_unique<RtcEventLogOutputFile>(
                  dump_name + ".send.rtc.dat", RtcEventLog::kUnlimitedOutput),
              RtcEventLog::kImmediateOutput) &&
          recv_event_log->StartLogging(
              std::make_unique<RtcEventLogOutputFile>(
                  dump_name + ".recv.rtc.dat", RtcEventLog::kUnlimitedOutput),
              RtcEventLog::kImmediateOutput);
      RTC_DCHECK(event_log_started);
      SetSendEventLog(std::move(send_event_log));
      SetRecvEventLog(std::move(recv_event_log));
    }
  }

 private:
  RtcEventLogFactory rtc_event_log_factory_;
};

static const uint32_t kStartBitrateBps = 60000;

TEST_F(RampUpTest, UpDownUpAbsSendTimeSimulcastRedRtx) {
  std::vector<int> loss_rates = {0, 0, 0, 0};
  RegisterRtpExtension(
      RtpExtension(RtpExtension::kAbsSendTimeUri, kAbsSendTimeExtensionId));
  RampUpDownUpTester test(3, 0, 0, kStartBitrateBps, true, true, loss_rates,
                          true, task_queue());
  RunBaseTest(&test);
}

// TODO(bugs.webrtc.org/8878)
#if defined(WEBRTC_MAC)
#define MAYBE_UpDownUpTransportSequenceNumberRtx \
  DISABLED_UpDownUpTransportSequenceNumberRtx
#else
#define MAYBE_UpDownUpTransportSequenceNumberRtx \
  UpDownUpTransportSequenceNumberRtx
#endif
TEST_F(RampUpTest, MAYBE_UpDownUpTransportSequenceNumberRtx) {
  std::vector<int> loss_rates = {0, 0, 0, 0};
  RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                    kTransportSequenceNumberExtensionId));
  RampUpDownUpTester test(3, 0, 0, kStartBitrateBps, true, false, loss_rates,
                          true, task_queue());
  RunBaseTest(&test);
}

// TODO(holmer): Tests which don't report perf stats should be moved to a
// different executable since they per definition are not perf tests.
// This test is disabled because it crashes on Linux, and is flaky on other
// platforms. See: crbug.com/webrtc/7919
TEST_F(RampUpTest, DISABLED_UpDownUpTransportSequenceNumberPacketLoss) {
  std::vector<int> loss_rates = {20, 0, 0, 0};
  RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                    kTransportSequenceNumberExtensionId));
  RampUpDownUpTester test(1, 0, 1, kStartBitrateBps, true, false, loss_rates,
                          false, task_queue());
  RunBaseTest(&test);
}

// TODO(bugs.webrtc.org/8878)
#if defined(WEBRTC_MAC)
#define MAYBE_UpDownUpAudioVideoTransportSequenceNumberRtx \
  DISABLED_UpDownUpAudioVideoTransportSequenceNumberRtx
#else
#define MAYBE_UpDownUpAudioVideoTransportSequenceNumberRtx \
  UpDownUpAudioVideoTransportSequenceNumberRtx
#endif
TEST_F(RampUpTest, MAYBE_UpDownUpAudioVideoTransportSequenceNumberRtx) {
  std::vector<int> loss_rates = {0, 0, 0, 0};
  RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                    kTransportSequenceNumberExtensionId));
  RampUpDownUpTester test(3, 1, 0, kStartBitrateBps, true, false, loss_rates,
                          false, task_queue());
  RunBaseTest(&test);
}

TEST_F(RampUpTest, UpDownUpAudioTransportSequenceNumberRtx) {
  std::vector<int> loss_rates = {0, 0, 0, 0};
  RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                    kTransportSequenceNumberExtensionId));
  RampUpDownUpTester test(0, 1, 0, kStartBitrateBps, true, false, loss_rates,
                          false, task_queue());
  RunBaseTest(&test);
}

TEST_F(RampUpTest, TOffsetSimulcastRedRtx) {
  RegisterRtpExtension(RtpExtension(RtpExtension::kTimestampOffsetUri,
                                    kTransmissionTimeOffsetExtensionId));
  RampUpTester test(3, 0, 0, 0, 0, true, true, true, task_queue());
  RunBaseTest(&test);
}

TEST_F(RampUpTest, AbsSendTime) {
  RegisterRtpExtension(
      RtpExtension(RtpExtension::kAbsSendTimeUri, kAbsSendTimeExtensionId));
  RampUpTester test(1, 0, 0, 0, 0, false, false, false, task_queue());
  RunBaseTest(&test);
}

TEST_F(RampUpTest, AbsSendTimeSimulcastRedRtx) {
  RegisterRtpExtension(
      RtpExtension(RtpExtension::kAbsSendTimeUri, kAbsSendTimeExtensionId));
  RampUpTester test(3, 0, 0, 0, 0, true, true, true, task_queue());
  RunBaseTest(&test);
}

TEST_F(RampUpTest, TransportSequenceNumber) {
  RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                    kTransportSequenceNumberExtensionId));
  RampUpTester test(1, 0, 0, 0, 0, false, false, false, task_queue());
  RunBaseTest(&test);
}

TEST_F(RampUpTest, TransportSequenceNumberSimulcast) {
  RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                    kTransportSequenceNumberExtensionId));
  RampUpTester test(3, 0, 0, 0, 0, false, false, false, task_queue());
  RunBaseTest(&test);
}

TEST_F(RampUpTest, TransportSequenceNumberSimulcastRedRtx) {
  RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                    kTransportSequenceNumberExtensionId));
  RampUpTester test(3, 0, 0, 0, 0, true, true, true, task_queue());
  RunBaseTest(&test);
}

TEST_F(RampUpTest, AudioTransportSequenceNumber) {
  RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                    kTransportSequenceNumberExtensionId));
  RampUpTester test(0, 1, 0, 300000, 10000, false, false, false, task_queue());
  RunBaseTest(&test);
}

}  // namespace webrtc
