/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/strings/string_view.h"
#include "api/array_view.h"
#include "api/audio/audio_device.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/environment/environment.h"
#include "api/field_trials_view.h"
#include "api/make_ref_counted.h"
#include "api/numerics/samples_stats_counter.h"
#include "api/rtp_parameters.h"
#include "api/scoped_refptr.h"
#include "api/task_queue/pending_task_safety_flag.h"
#include "api/task_queue/task_queue_base.h"
#include "api/test/metrics/global_metrics_logger_and_exporter.h"
#include "api/test/metrics/metric.h"
#include "api/test/simulated_network.h"
#include "api/test/video/function_video_encoder_factory.h"
#include "api/transport/bitrate_settings.h"
#include "api/units/data_rate.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "api/video/builtin_video_bitrate_allocator_factory.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video/video_bitrate_allocator_factory.h"
#include "api/video/video_frame.h"
#include "api/video/video_sink_interface.h"
#include "api/video/video_source_interface.h"
#include "api/video_codecs/sdp_video_format.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "api/video_codecs/video_encoder_factory.h"
#include "call/audio_receive_stream.h"
#include "call/audio_send_stream.h"
#include "call/audio_state.h"
#include "call/call.h"
#include "call/call_config.h"
#include "call/fake_network_pipe.h"
#include "call/video_receive_stream.h"
#include "call/video_send_stream.h"
#include "media/engine/internal_encoder_factory.h"
#include "media/engine/simulcast_encoder_adapter.h"
#include "modules/audio_device/include/test_audio_device.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "rtc_base/checks.h"
#include "rtc_base/event.h"
#include "rtc_base/logging.h"
#include "rtc_base/task_queue_for_test.h"
#include "rtc_base/thread.h"
#include "system_wrappers/include/metrics.h"
#include "test/call_test.h"
#include "test/drifting_clock.h"
#include "test/encoder_settings.h"
#include "test/fake_encoder.h"
#include "test/frame_generator_capturer.h"
#include "test/gtest.h"
#include "test/network/simulated_network.h"
#include "test/rtp_rtcp_observer.h"
#include "test/test_flags.h"
#include "test/video_encoder_proxy_factory.h"
#include "test/video_test_constants.h"
#include "video/config/video_encoder_config.h"

using webrtc::test::DriftingClock;

namespace webrtc {
namespace {

using test::GetGlobalMetricsLogger;
using test::ImprovementDirection;
using test::Unit;

enum : int {  // The first valid value is 1.
  kTransportSequenceNumberExtensionId = 1,
};

}  // namespace

class CallPerfTest : public test::CallTest {
 public:
  CallPerfTest() {
    RegisterRtpExtension(RtpExtension(RtpExtension::kTransportSequenceNumberUri,
                                      kTransportSequenceNumberExtensionId));
  }

 protected:
  enum class FecMode { kOn, kOff };
  enum class CreateOrder { kAudioFirst, kVideoFirst };
  void TestAudioVideoSync(FecMode fec,
                          CreateOrder create_first,
                          float video_ntp_speed,
                          float video_rtp_speed,
                          float audio_rtp_speed,
                          absl::string_view test_label);

  void TestMinTransmitBitrate(bool pad_to_min_bitrate);

  void TestCaptureNtpTime(const BuiltInNetworkBehaviorConfig& net_config,
                          int threshold_ms,
                          int start_time_ms,
                          int run_time_ms);
  void TestMinAudioVideoBitrate(int test_bitrate_from,
                                int test_bitrate_to,
                                int test_bitrate_step,
                                int min_bwe,
                                int start_bwe,
                                int max_bwe);
  void TestEncodeFramerate(VideoEncoderFactory* encoder_factory,
                           absl::string_view payload_name,
                           const std::vector<int>& max_framerates);
};

class VideoRtcpAndSyncObserver : public test::RtpRtcpObserver,
                                 public VideoSinkInterface<VideoFrame> {
  static const int kInSyncThresholdMs = 50;
  static const int kStartupTimeMs = 2000;
  static const int kMinRunTimeMs = 30000;

 public:
  explicit VideoRtcpAndSyncObserver(TaskQueueBase* task_queue,
                                    Clock* clock,
                                    absl::string_view test_label)
      : test::RtpRtcpObserver(test::VideoTestConstants::kLongTimeout),
        clock_(clock),
        test_label_(test_label),
        creation_time_ms_(clock_->TimeInMilliseconds()),
        task_queue_(task_queue) {}

  void OnFrame(const VideoFrame& /* video_frame */) override {
    task_queue_->PostTask([this]() { CheckStats(); });
  }

  void CheckStats() {
    if (!receive_stream_)
      return;

    VideoReceiveStreamInterface::Stats stats = receive_stream_->GetStats();
    if (stats.sync_offset_ms == std::numeric_limits<int>::max())
      return;

    int64_t now_ms = clock_->TimeInMilliseconds();
    int64_t time_since_creation = now_ms - creation_time_ms_;
    // During the first couple of seconds audio and video can falsely be
    // estimated as being synchronized. We don't want to trigger on those.
    if (time_since_creation < kStartupTimeMs)
      return;
    if (std::abs(stats.sync_offset_ms) < kInSyncThresholdMs) {
      if (first_time_in_sync_ == -1) {
        first_time_in_sync_ = now_ms;
        GetGlobalMetricsLogger()->LogSingleValueMetric(
            "sync_convergence_time" + test_label_, "synchronization",
            time_since_creation, Unit::kMilliseconds,
            ImprovementDirection::kSmallerIsBetter);
      }
      if (time_since_creation > kMinRunTimeMs)
        observation_complete_.Set();
    }
    if (first_time_in_sync_ != -1)
      sync_offset_ms_list_.AddSample(stats.sync_offset_ms);
  }

  void set_receive_stream(VideoReceiveStreamInterface* receive_stream) {
    RTC_DCHECK_EQ(task_queue_, TaskQueueBase::Current());
    // Note that receive_stream may be nullptr.
    receive_stream_ = receive_stream;
  }

  void PrintResults() {
    GetGlobalMetricsLogger()->LogMetric(
        "stream_offset" + test_label_, "synchronization", sync_offset_ms_list_,
        Unit::kMilliseconds, ImprovementDirection::kNeitherIsBetter);
  }

 private:
  Clock* const clock_;
  const std::string test_label_;
  const int64_t creation_time_ms_;
  int64_t first_time_in_sync_ = -1;
  VideoReceiveStreamInterface* receive_stream_ = nullptr;
  SamplesStatsCounter sync_offset_ms_list_;
  TaskQueueBase* const task_queue_;
};

void CallPerfTest::TestAudioVideoSync(FecMode fec,
                                      CreateOrder create_first,
                                      float video_ntp_speed,
                                      float video_rtp_speed,
                                      float audio_rtp_speed,
                                      absl::string_view test_label) {
  const char* kSyncGroup = "av_sync";
  const uint32_t kAudioSendSsrc = 1234;
  const uint32_t kAudioRecvSsrc = 5678;

  BuiltInNetworkBehaviorConfig audio_net_config;
  audio_net_config.queue_delay_ms = 500;
  audio_net_config.loss_percent = 5;

  auto observer = std::make_unique<VideoRtcpAndSyncObserver>(
      task_queue(), Clock::GetRealTimeClock(), test_label);

  std::map<uint8_t, MediaType> audio_pt_map;
  std::map<uint8_t, MediaType> video_pt_map;

  std::unique_ptr<test::PacketTransport> audio_send_transport;
  std::unique_ptr<test::PacketTransport> video_send_transport;
  std::unique_ptr<test::PacketTransport> receive_transport;

  AudioSendStream* audio_send_stream;
  AudioReceiveStreamInterface* audio_receive_stream;
  std::unique_ptr<DriftingClock> drifting_clock;

  SendTask(task_queue(), [&]() {
    metrics::Reset();
    scoped_refptr<AudioDeviceModule> fake_audio_device =
        TestAudioDeviceModule::Create(
            env(), TestAudioDeviceModule::CreatePulsedNoiseCapturer(256, 48000),
            TestAudioDeviceModule::CreateDiscardRenderer(48000),
            audio_rtp_speed);
    EXPECT_EQ(0, fake_audio_device->Init());

    AudioState::Config send_audio_state_config;
    send_audio_state_config.audio_mixer = AudioMixerImpl::Create();
    send_audio_state_config.audio_processing =
        BuiltinAudioProcessingBuilder().Build(env());
    send_audio_state_config.audio_device_module = fake_audio_device;
    CallConfig sender_config = SendCallConfig();

    auto audio_state = AudioState::Create(send_audio_state_config);
    fake_audio_device->RegisterAudioCallback(audio_state->audio_transport());
    sender_config.audio_state = audio_state;
    CallConfig receiver_config = RecvCallConfig();
    receiver_config.audio_state = audio_state;
    CreateCalls(std::move(sender_config), std::move(receiver_config));

    std::copy_if(std::begin(payload_type_map_), std::end(payload_type_map_),
                 std::inserter(audio_pt_map, audio_pt_map.end()),
                 [](const std::pair<const uint8_t, MediaType>& pair) {
                   return pair.second == MediaType::AUDIO;
                 });
    std::copy_if(std::begin(payload_type_map_), std::end(payload_type_map_),
                 std::inserter(video_pt_map, video_pt_map.end()),
                 [](const std::pair<const uint8_t, MediaType>& pair) {
                   return pair.second == MediaType::VIDEO;
                 });

    audio_send_transport = std::make_unique<test::PacketTransport>(
        task_queue(), sender_call_.get(), observer.get(),
        test::PacketTransport::kSender, audio_pt_map,
        std::make_unique<FakeNetworkPipe>(
            Clock::GetRealTimeClock(),
            std::make_unique<SimulatedNetwork>(audio_net_config)),
        GetRegisteredExtensions(), GetRegisteredExtensions());
    audio_send_transport->SetReceiver(receiver_call_->Receiver());

    video_send_transport = std::make_unique<test::PacketTransport>(
        task_queue(), sender_call_.get(), observer.get(),
        test::PacketTransport::kSender, video_pt_map,
        std::make_unique<FakeNetworkPipe>(
            Clock::GetRealTimeClock(),
            std::make_unique<SimulatedNetwork>(BuiltInNetworkBehaviorConfig())),
        GetRegisteredExtensions(), GetRegisteredExtensions());
    video_send_transport->SetReceiver(receiver_call_->Receiver());

    receive_transport = std::make_unique<test::PacketTransport>(
        task_queue(), receiver_call_.get(), observer.get(),
        test::PacketTransport::kReceiver, payload_type_map_,
        std::make_unique<FakeNetworkPipe>(
            Clock::GetRealTimeClock(),
            std::make_unique<SimulatedNetwork>(BuiltInNetworkBehaviorConfig())),
        GetRegisteredExtensions(), GetRegisteredExtensions());
    receive_transport->SetReceiver(sender_call_->Receiver());

    CreateSendConfig(1, 0, 0, video_send_transport.get());
    CreateMatchingReceiveConfigs(receive_transport.get());

    AudioSendStream::Config audio_send_config(audio_send_transport.get());
    audio_send_config.rtp.ssrc = kAudioSendSsrc;
    // TODO(bugs.webrtc.org/14683): Let the tests fail with invalid config.
    audio_send_config.send_codec_spec = AudioSendStream::Config::SendCodecSpec(
        test::VideoTestConstants::kAudioSendPayloadType, {"OPUS", 48000, 2});
    audio_send_config.min_bitrate_bps = 6000;
    audio_send_config.max_bitrate_bps = 510000;
    audio_send_config.encoder_factory = CreateBuiltinAudioEncoderFactory();
    audio_send_stream = sender_call_->CreateAudioSendStream(audio_send_config);

    GetVideoSendConfig()->rtp.nack.rtp_history_ms =
        test::VideoTestConstants::kNackRtpHistoryMs;
    if (fec == FecMode::kOn) {
      GetVideoSendConfig()->rtp.ulpfec.red_payload_type =
          test::VideoTestConstants::kRedPayloadType;
      GetVideoSendConfig()->rtp.ulpfec.ulpfec_payload_type =
          test::VideoTestConstants::kUlpfecPayloadType;
      video_receive_configs_[0].rtp.red_payload_type =
          test::VideoTestConstants::kRedPayloadType;
      video_receive_configs_[0].rtp.ulpfec_payload_type =
          test::VideoTestConstants::kUlpfecPayloadType;
    }
    video_receive_configs_[0].rtp.nack.rtp_history_ms = 1000;
    video_receive_configs_[0].renderer = observer.get();
    video_receive_configs_[0].sync_group = kSyncGroup;

    AudioReceiveStreamInterface::Config audio_recv_config;
    audio_recv_config.rtp.remote_ssrc = kAudioSendSsrc;
    audio_recv_config.rtp.local_ssrc = kAudioRecvSsrc;
    audio_recv_config.rtcp_send_transport = receive_transport.get();
    audio_recv_config.sync_group = kSyncGroup;
    audio_recv_config.decoder_factory = audio_decoder_factory_;
    audio_recv_config.decoder_map = {
        {test::VideoTestConstants::kAudioSendPayloadType, {"OPUS", 48000, 2}}};

    if (create_first == CreateOrder::kAudioFirst) {
      audio_receive_stream =
          receiver_call_->CreateAudioReceiveStream(audio_recv_config);
      CreateVideoStreams();
    } else {
      CreateVideoStreams();
      audio_receive_stream =
          receiver_call_->CreateAudioReceiveStream(audio_recv_config);
    }
    EXPECT_EQ(1u, video_receive_streams_.size());
    observer->set_receive_stream(video_receive_streams_[0]);
    drifting_clock =
        std::make_unique<DriftingClock>(&env().clock(), video_ntp_speed);
    CreateFrameGeneratorCapturerWithDrift(
        drifting_clock.get(), video_rtp_speed,
        test::VideoTestConstants::kDefaultFramerate,
        test::VideoTestConstants::kDefaultWidth,
        test::VideoTestConstants::kDefaultHeight);

    Start();

    audio_send_stream->Start();
    audio_receive_stream->Start();
  });

  EXPECT_TRUE(observer->Wait())
      << "Timed out while waiting for audio and video to be synchronized.";

  SendTask(task_queue(), [&]() {
    // Clear the pointer to the receive stream since it will now be deleted.
    observer->set_receive_stream(nullptr);

    audio_send_stream->Stop();
    audio_receive_stream->Stop();

    Stop();

    DestroyStreams();

    sender_call_->DestroyAudioSendStream(audio_send_stream);
    receiver_call_->DestroyAudioReceiveStream(audio_receive_stream);

    DestroyCalls();
    // Call may post periodic rtcp packet to the transport on the process
    // thread, thus transport should be destroyed after the call objects.
    // Though transports keep pointers to the call objects, transports handle
    // packets on the task_queue() and thus wouldn't create a race while current
    // destruction happens in the same task as destruction of the call objects.
    video_send_transport.reset();
    audio_send_transport.reset();
    receive_transport.reset();
  });

  observer->PrintResults();

  // In quick test synchronization may not be achieved in time.
  if (!absl::GetFlag(FLAGS_webrtc_quick_perf_test)) {
// TODO(bugs.webrtc.org/10417): Reenable this for iOS
#if !defined(WEBRTC_IOS)
    EXPECT_METRIC_EQ(1, metrics::NumSamples("WebRTC.Video.AVSyncOffsetInMs"));
#endif
  }

  task_queue()->PostTask(
      [to_delete = observer.release()]() { delete to_delete; });
}

TEST_F(CallPerfTest, Synchronization_PlaysOutAudioAndVideoWithoutClockDrift) {
  TestAudioVideoSync(FecMode::kOff, CreateOrder::kAudioFirst,
                     DriftingClock::kNoDrift, DriftingClock::kNoDrift,
                     DriftingClock::kNoDrift, "_video_no_drift");
}

TEST_F(CallPerfTest, Synchronization_PlaysOutAudioAndVideoWithVideoNtpDrift) {
  TestAudioVideoSync(FecMode::kOff, CreateOrder::kAudioFirst,
                     DriftingClock::PercentsFaster(10.0f),
                     DriftingClock::kNoDrift, DriftingClock::kNoDrift,
                     "_video_ntp_drift");
}

TEST_F(CallPerfTest,
       Synchronization_PlaysOutAudioAndVideoWithAudioFasterThanVideoDrift) {
  TestAudioVideoSync(FecMode::kOff, CreateOrder::kAudioFirst,
                     DriftingClock::kNoDrift,
                     DriftingClock::PercentsSlower(30.0f),
                     DriftingClock::PercentsFaster(30.0f), "_audio_faster");
}

TEST_F(CallPerfTest,
       Synchronization_PlaysOutAudioAndVideoWithVideoFasterThanAudioDrift) {
  TestAudioVideoSync(FecMode::kOn, CreateOrder::kVideoFirst,
                     DriftingClock::kNoDrift,
                     DriftingClock::PercentsFaster(30.0f),
                     DriftingClock::PercentsSlower(30.0f), "_video_faster");
}

TEST_F(CallPerfTest, ReceivesCpuOveruseAndUnderuse) {
  // Minimal normal usage at the start, then 30s overuse to allow filter to
  // settle, and then 80s underuse to allow plenty of time for rampup again.
  field_trials().Set("WebRTC-ForceSimulatedOveruseIntervalMs", "1-30000-80000");

  class LoadObserver : public test::SendTest,
                       public test::FrameGeneratorCapturer::SinkWantsObserver {
   public:
    LoadObserver()
        : SendTest(test::VideoTestConstants::kLongTimeout),
          test_phase_(TestPhase::kInit) {}

    void OnFrameGeneratorCapturerCreated(
        test::FrameGeneratorCapturer* frame_generator_capturer) override {
      frame_generator_capturer->SetSinkWantsObserver(this);
      // Set a high initial resolution to be sure that we can scale down.
      frame_generator_capturer->ChangeResolution(1920, 1080);
    }

    // OnSinkWantsChanged is called when FrameGeneratorCapturer::AddOrUpdateSink
    // is called.
    // TODO(sprang): Add integration test for maintain-framerate mode?
    void OnSinkWantsChanged(VideoSinkInterface<VideoFrame>* /* sink */,
                            const VideoSinkWants& wants) override {
      RTC_LOG(LS_INFO) << "OnSinkWantsChanged fps:" << wants.max_framerate_fps
                       << " max_pixel_count " << wants.max_pixel_count
                       << " target_pixel_count"
                       << wants.target_pixel_count.value_or(-1);
      // The sink wants can change either because an adaptation happened
      // (i.e. the pixels or frame rate changed) or for other reasons, such
      // as encoded resolutions being communicated (happens whenever we
      // capture a new frame size). In this test, we only care about
      // adaptations.
      bool did_adapt =
          last_wants_.max_pixel_count != wants.max_pixel_count ||
          last_wants_.target_pixel_count != wants.target_pixel_count ||
          last_wants_.max_framerate_fps != wants.max_framerate_fps;
      last_wants_ = wants;
      if (!did_adapt) {
        if (test_phase_ == TestPhase::kInit) {
          test_phase_ = TestPhase::kStart;
        }
        return;
      }
      // At kStart expect CPU overuse. Then expect CPU underuse when the encoder
      // delay has been decreased.
      switch (test_phase_) {
        case TestPhase::kInit:
          ADD_FAILURE() << "Got unexpected adaptation request, max res = "
                        << wants.max_pixel_count << ", target res = "
                        << wants.target_pixel_count.value_or(-1)
                        << ", max fps = " << wants.max_framerate_fps;
          break;
        case TestPhase::kStart:
          if (wants.max_pixel_count < std::numeric_limits<int>::max()) {
            // On adapting down, VideoStreamEncoder::VideoSourceProxy will set
            // only the max pixel count, leaving the target unset.
            test_phase_ = TestPhase::kAdaptedDown;
          } else {
            ADD_FAILURE() << "Got unexpected adaptation request, max res = "
                          << wants.max_pixel_count << ", target res = "
                          << wants.target_pixel_count.value_or(-1)
                          << ", max fps = " << wants.max_framerate_fps;
          }
          break;
        case TestPhase::kAdaptedDown:
          // On adapting up, the adaptation counter will again be at zero, and
          // so all constraints will be reset.
          if (wants.max_pixel_count == std::numeric_limits<int>::max() &&
              !wants.target_pixel_count) {
            test_phase_ = TestPhase::kAdaptedUp;
            observation_complete_.Set();
          } else {
            ADD_FAILURE() << "Got unexpected adaptation request, max res = "
                          << wants.max_pixel_count << ", target res = "
                          << wants.target_pixel_count.value_or(-1)
                          << ", max fps = " << wants.max_framerate_fps;
          }
          break;
        case TestPhase::kAdaptedUp:
          ADD_FAILURE() << "Got unexpected adaptation request, max res = "
                        << wants.max_pixel_count << ", target res = "
                        << wants.target_pixel_count.value_or(-1)
                        << ", max fps = " << wants.max_framerate_fps;
      }
    }

    void ModifyVideoConfigs(
        VideoSendStream::Config* /* send_config */,
        std::vector<VideoReceiveStreamInterface::Config>* /* receive_configs */,
        VideoEncoderConfig* /* encoder_config */) override {}

    void PerformTest() override {
      EXPECT_TRUE(Wait()) << "Timed out before receiving an overuse callback.";
    }

    enum class TestPhase {
      kInit,
      kStart,
      kAdaptedDown,
      kAdaptedUp
    } test_phase_;

   private:
    VideoSinkWants last_wants_;
  } test;

  RunBaseTest(&test);
}

void CallPerfTest::TestMinTransmitBitrate(bool pad_to_min_bitrate) {
  static const int kMaxEncodeBitrateKbps = 30;
  static const int kMinTransmitBitrateBps = 150000;
  static const int kMinAcceptableTransmitBitrate = 130;
  static const int kMaxAcceptableTransmitBitrate = 170;
  static const int kNumBitrateObservationsInRange = 100;
  static const int kAcceptableBitrateErrorMargin = 15;  // +- 7
  class BitrateObserver : public test::EndToEndTest {
   public:
    explicit BitrateObserver(bool using_min_transmit_bitrate,
                             TaskQueueBase* task_queue)
        : EndToEndTest(test::VideoTestConstants::kLongTimeout),
          send_stream_(nullptr),
          converged_(false),
          pad_to_min_bitrate_(using_min_transmit_bitrate),
          min_acceptable_bitrate_(using_min_transmit_bitrate
                                      ? kMinAcceptableTransmitBitrate
                                      : (kMaxEncodeBitrateKbps -
                                         kAcceptableBitrateErrorMargin / 2)),
          max_acceptable_bitrate_(using_min_transmit_bitrate
                                      ? kMaxAcceptableTransmitBitrate
                                      : (kMaxEncodeBitrateKbps +
                                         kAcceptableBitrateErrorMargin / 2)),
          num_bitrate_observations_in_range_(0),
          task_queue_(task_queue),
          task_safety_flag_(PendingTaskSafetyFlag::CreateDetached()) {}

   private:
    // TODO(holmer): Run this with a timer instead of once per packet.
    Action OnSendRtp(ArrayView<const uint8_t> /* packet */) override {
      task_queue_->PostTask(SafeTask(task_safety_flag_, [this]() {
        VideoSendStream::Stats stats = send_stream_->GetStats();

        if (!stats.substreams.empty()) {
          RTC_DCHECK_EQ(1, stats.substreams.size());
          int bitrate_kbps =
              stats.substreams.begin()->second.total_bitrate_bps / 1000;
          if (bitrate_kbps > min_acceptable_bitrate_ &&
              bitrate_kbps < max_acceptable_bitrate_) {
            converged_ = true;
            ++num_bitrate_observations_in_range_;
            if (num_bitrate_observations_in_range_ ==
                kNumBitrateObservationsInRange)
              observation_complete_.Set();
          }
          if (converged_)
            bitrate_kbps_list_.AddSample(bitrate_kbps);
        }
      }));
      return SEND_PACKET;
    }

    void OnVideoStreamsCreated(VideoSendStream* send_stream,
                               const std::vector<VideoReceiveStreamInterface*>&
                               /* receive_streams */) override {
      send_stream_ = send_stream;
    }

    void OnStreamsStopped() override { task_safety_flag_->SetNotAlive(); }

    void ModifyVideoConfigs(
        VideoSendStream::Config* /* send_config */,
        std::vector<VideoReceiveStreamInterface::Config>* /* receive_configs */,
        VideoEncoderConfig* encoder_config) override {
      if (pad_to_min_bitrate_) {
        encoder_config->min_transmit_bitrate_bps = kMinTransmitBitrateBps;
      } else {
        RTC_DCHECK_EQ(0, encoder_config->min_transmit_bitrate_bps);
      }
    }

    void PerformTest() override {
      EXPECT_TRUE(Wait()) << "Timeout while waiting for send-bitrate stats.";
      GetGlobalMetricsLogger()->LogMetric(
          std::string("bitrate_stats_") +
              (pad_to_min_bitrate_ ? "min_transmit_bitrate"
                                   : "without_min_transmit_bitrate"),
          "bitrate_kbps", bitrate_kbps_list_, Unit::kUnitless,
          ImprovementDirection::kNeitherIsBetter);
    }

    VideoSendStream* send_stream_;
    bool converged_;
    const bool pad_to_min_bitrate_;
    const int min_acceptable_bitrate_;
    const int max_acceptable_bitrate_;
    int num_bitrate_observations_in_range_;
    SamplesStatsCounter bitrate_kbps_list_;
    TaskQueueBase* task_queue_;
    scoped_refptr<PendingTaskSafetyFlag> task_safety_flag_;
  } test(pad_to_min_bitrate, task_queue());

  fake_encoder_max_bitrate_ = kMaxEncodeBitrateKbps;
  RunBaseTest(&test);
}

TEST_F(CallPerfTest, Bitrate_Kbps_PadsToMinTransmitBitrate) {
  TestMinTransmitBitrate(true);
}

TEST_F(CallPerfTest, Bitrate_Kbps_NoPadWithoutMinTransmitBitrate) {
  TestMinTransmitBitrate(false);
}

// TODO(bugs.webrtc.org/8878)
#if defined(WEBRTC_MAC)
#define MAYBE_KeepsHighBitrateWhenReconfiguringSender \
  DISABLED_KeepsHighBitrateWhenReconfiguringSender
#else
#define MAYBE_KeepsHighBitrateWhenReconfiguringSender \
  KeepsHighBitrateWhenReconfiguringSender
#endif
TEST_F(CallPerfTest, MAYBE_KeepsHighBitrateWhenReconfiguringSender) {
  static const uint32_t kInitialBitrateKbps = 400;
  static const uint32_t kInitialBitrateOverheadKpbs = 6;
  static const uint32_t kReconfigureThresholdKbps = 600;

  class VideoStreamFactory
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
      streams[0].min_bitrate_bps = 50000;
      streams[0].target_bitrate_bps = streams[0].max_bitrate_bps = 2000000;
      return streams;
    }
  };

  class BitrateObserver : public test::EndToEndTest, public test::FakeEncoder {
   public:
    explicit BitrateObserver(const Environment& env, TaskQueueBase* task_queue)
        : EndToEndTest(test::VideoTestConstants::kDefaultTimeout),
          FakeEncoder(env),
          encoder_inits_(0),
          last_set_bitrate_kbps_(0),
          send_stream_(nullptr),
          frame_generator_(nullptr),
          encoder_factory_(this),
          bitrate_allocator_factory_(
              CreateBuiltinVideoBitrateAllocatorFactory()),
          task_queue_(task_queue) {}

    int32_t InitEncode(const VideoCodec* config,
                       const VideoEncoder::Settings& settings) override {
      ++encoder_inits_;
      if (encoder_inits_ == 1) {
        // First time initialization. Frame size is known.
        // `expected_bitrate` is affected by bandwidth estimation before the
        // first frame arrives to the encoder.
        uint32_t expected_bitrate =
            last_set_bitrate_kbps_ > 0
                ? last_set_bitrate_kbps_
                : kInitialBitrateKbps - kInitialBitrateOverheadKpbs;
        EXPECT_EQ(expected_bitrate, config->startBitrate)
            << "Encoder not initialized at expected bitrate.";
        EXPECT_EQ(test::VideoTestConstants::kDefaultWidth, config->width);
        EXPECT_EQ(test::VideoTestConstants::kDefaultHeight, config->height);
      } else if (encoder_inits_ == 2) {
        EXPECT_EQ(2 * test::VideoTestConstants::kDefaultWidth, config->width);
        EXPECT_EQ(2 * test::VideoTestConstants::kDefaultHeight, config->height);
        EXPECT_GE(last_set_bitrate_kbps_, kReconfigureThresholdKbps);
        EXPECT_GT(config->startBitrate, kReconfigureThresholdKbps)
            << "Encoder reconfigured with bitrate too far away from last set.";
        observation_complete_.Set();
      }
      return FakeEncoder::InitEncode(config, settings);
    }

    void SetRates(const RateControlParameters& parameters) override {
      last_set_bitrate_kbps_ = parameters.bitrate.get_sum_kbps();
      if (encoder_inits_ == 1 &&
          parameters.bitrate.get_sum_kbps() > kReconfigureThresholdKbps) {
        time_to_reconfigure_.Set();
      }
      FakeEncoder::SetRates(parameters);
    }

    void ModifySenderBitrateConfig(
        BitrateConstraints* bitrate_config) override {
      bitrate_config->start_bitrate_bps = kInitialBitrateKbps * 1000;
    }

    void ModifyVideoConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStreamInterface::Config>* /* receive_configs */,
        VideoEncoderConfig* encoder_config) override {
      send_config->encoder_settings.encoder_factory = &encoder_factory_;
      send_config->encoder_settings.bitrate_allocator_factory =
          bitrate_allocator_factory_.get();
      encoder_config->max_bitrate_bps = 2 * kReconfigureThresholdKbps * 1000;
      encoder_config->video_stream_factory =
          make_ref_counted<VideoStreamFactory>();

      encoder_config_ = encoder_config->Copy();
    }

    void OnVideoStreamsCreated(VideoSendStream* send_stream,
                               const std::vector<VideoReceiveStreamInterface*>&
                               /* receive_streams */) override {
      send_stream_ = send_stream;
    }

    void OnFrameGeneratorCapturerCreated(
        test::FrameGeneratorCapturer* frame_generator_capturer) override {
      frame_generator_ = frame_generator_capturer;
    }

    void PerformTest() override {
      ASSERT_TRUE(
          time_to_reconfigure_.Wait(test::VideoTestConstants::kDefaultTimeout))
          << "Timed out before receiving an initial high bitrate.";
      frame_generator_->ChangeResolution(
          test::VideoTestConstants::kDefaultWidth * 2,
          test::VideoTestConstants::kDefaultHeight * 2);
      SendTask(task_queue_, [&]() {
        send_stream_->ReconfigureVideoEncoder(encoder_config_.Copy());
      });
      EXPECT_TRUE(Wait())
          << "Timed out while waiting for a couple of high bitrate estimates "
             "after reconfiguring the send stream.";
    }

   private:
    Event time_to_reconfigure_;
    int encoder_inits_;
    uint32_t last_set_bitrate_kbps_;
    VideoSendStream* send_stream_;
    test::FrameGeneratorCapturer* frame_generator_;
    test::VideoEncoderProxyFactory encoder_factory_;
    std::unique_ptr<VideoBitrateAllocatorFactory> bitrate_allocator_factory_;
    VideoEncoderConfig encoder_config_;
    TaskQueueBase* task_queue_;
  } test(env(), task_queue());

  RunBaseTest(&test);
}

// Discovers the minimal supported audio+video bitrate. The test bitrate is
// considered supported if Rtt does not go above 400ms with the network
// contrained to the test bitrate.
//
// |test_bitrate_from test_bitrate_to| bitrate constraint range
// `test_bitrate_step` bitrate constraint update step during the test
// |min_bwe max_bwe| BWE range
// `start_bwe` initial BWE
void CallPerfTest::TestMinAudioVideoBitrate(int test_bitrate_from,
                                            int test_bitrate_to,
                                            int test_bitrate_step,
                                            int min_bwe,
                                            int start_bwe,
                                            int max_bwe) {
  static const std::string kAudioTrackId = "audio_track_0";
  static constexpr int kBitrateStabilizationMs = 10000;
  static constexpr int kBitrateMeasurements = 10;
  static constexpr int kBitrateMeasurementMs = 1000;
  static constexpr int kShortDelayMs = 10;
  static constexpr int kMinGoodRttMs = 400;

  class MinVideoAndAudioBitrateTester : public test::EndToEndTest {
   public:
    MinVideoAndAudioBitrateTester(int test_bitrate_from,
                                  int test_bitrate_to,
                                  int test_bitrate_step,
                                  int min_bwe,
                                  int start_bwe,
                                  int max_bwe,
                                  TaskQueueBase* task_queue)
        : EndToEndTest(),
          test_bitrate_from_(test_bitrate_from),
          test_bitrate_to_(test_bitrate_to),
          test_bitrate_step_(test_bitrate_step),
          min_bwe_(min_bwe),
          start_bwe_(start_bwe),
          max_bwe_(max_bwe),
          task_queue_(task_queue) {}

   protected:
    BuiltInNetworkBehaviorConfig GetFakeNetworkPipeConfig() const {
      BuiltInNetworkBehaviorConfig pipe_config;
      pipe_config.link_capacity = DataRate::KilobitsPerSec(test_bitrate_from_);
      return pipe_config;
    }

    BuiltInNetworkBehaviorConfig GetSendTransportConfig() const override {
      return GetFakeNetworkPipeConfig();
    }
    BuiltInNetworkBehaviorConfig GetReceiveTransportConfig() const override {
      return GetFakeNetworkPipeConfig();
    }

    void OnTransportCreated(
        test::PacketTransport* /* to_receiver */,
        SimulatedNetworkInterface* sender_network,
        test::PacketTransport* /* to_sender */,
        SimulatedNetworkInterface* receiver_network) override {
      send_simulated_network_ = sender_network;
      receive_simulated_network_ = receiver_network;
    }

    void PerformTest() override {
      // Quick test mode, just to exercise all the code paths without actually
      // caring about performance measurements.
      const bool quick_perf_test = absl::GetFlag(FLAGS_webrtc_quick_perf_test);

      int last_passed_test_bitrate = -1;
      for (int test_bitrate = test_bitrate_from_;
           test_bitrate_from_ < test_bitrate_to_
               ? test_bitrate <= test_bitrate_to_
               : test_bitrate >= test_bitrate_to_;
           test_bitrate += test_bitrate_step_) {
        BuiltInNetworkBehaviorConfig pipe_config;
        pipe_config.link_capacity = DataRate::KilobitsPerSec(test_bitrate);
        send_simulated_network_->SetConfig(pipe_config);
        receive_simulated_network_->SetConfig(pipe_config);

        Thread::SleepMs(quick_perf_test ? kShortDelayMs
                                        : kBitrateStabilizationMs);

        int64_t avg_rtt = 0;
        for (int i = 0; i < kBitrateMeasurements; i++) {
          Call::Stats call_stats;
          SendTask(task_queue_, [this, &call_stats]() {
            call_stats = sender_call_->GetStats();
          });
          avg_rtt += call_stats.rtt_ms;
          Thread::SleepMs(quick_perf_test ? kShortDelayMs
                                          : kBitrateMeasurementMs);
        }
        avg_rtt = avg_rtt / kBitrateMeasurements;
        if (avg_rtt > kMinGoodRttMs) {
          RTC_LOG(LS_WARNING)
              << "Failed test bitrate: " << test_bitrate << " RTT: " << avg_rtt;
          break;
        } else {
          RTC_LOG(LS_INFO) << "Passed test bitrate: " << test_bitrate
                           << " RTT: " << avg_rtt;
          last_passed_test_bitrate = test_bitrate;
        }
      }
      EXPECT_GT(last_passed_test_bitrate, -1)
          << "Minimum supported bitrate out of the test scope";
      GetGlobalMetricsLogger()->LogSingleValueMetric(
          "min_test_bitrate_", "min_bitrate", last_passed_test_bitrate,
          Unit::kUnitless, ImprovementDirection::kNeitherIsBetter);
    }

    void OnCallsCreated(Call* sender_call, Call* /* receiver_call */) override {
      sender_call_ = sender_call;
      BitrateConstraints bitrate_config;
      bitrate_config.min_bitrate_bps = min_bwe_;
      bitrate_config.start_bitrate_bps = start_bwe_;
      bitrate_config.max_bitrate_bps = max_bwe_;
      sender_call->GetTransportControllerSend()->SetSdpBitrateParameters(
          bitrate_config);
    }

    size_t GetNumVideoStreams() const override { return 1; }

    size_t GetNumAudioStreams() const override { return 1; }

   private:
    const int test_bitrate_from_;
    const int test_bitrate_to_;
    const int test_bitrate_step_;
    const int min_bwe_;
    const int start_bwe_;
    const int max_bwe_;
    SimulatedNetworkInterface* send_simulated_network_;
    SimulatedNetworkInterface* receive_simulated_network_;
    Call* sender_call_;
    TaskQueueBase* const task_queue_;
  } test(test_bitrate_from, test_bitrate_to, test_bitrate_step, min_bwe,
         start_bwe, max_bwe, task_queue());

  RunBaseTest(&test);
}

TEST_F(CallPerfTest, Min_Bitrate_VideoAndAudio) {
  TestMinAudioVideoBitrate(110, 40, -10, 10000, 70000, 200000);
}

void CallPerfTest::TestEncodeFramerate(VideoEncoderFactory* encoder_factory,
                                       absl::string_view payload_name,
                                       const std::vector<int>& max_framerates) {
  static constexpr double kAllowedFpsDiff = 1.5;
  static constexpr TimeDelta kMinGetStatsInterval = TimeDelta::Millis(400);
  static constexpr TimeDelta kMinRunTime = TimeDelta::Seconds(15);
  static constexpr DataRate kMaxBitrate = DataRate::KilobitsPerSec(1000);

  class FramerateObserver
      : public test::EndToEndTest,
        public test::FrameGeneratorCapturer::SinkWantsObserver {
   public:
    FramerateObserver(VideoEncoderFactory* encoder_factory,
                      absl::string_view payload_name,
                      const std::vector<int>& max_framerates,
                      TaskQueueBase* task_queue)
        : EndToEndTest(test::VideoTestConstants::kDefaultTimeout),
          clock_(Clock::GetRealTimeClock()),
          encoder_factory_(encoder_factory),
          payload_name_(payload_name),
          max_framerates_(max_framerates),
          task_queue_(task_queue),
          start_time_(clock_->CurrentTime()),
          last_getstats_time_(start_time_),
          send_stream_(nullptr) {}

    void OnFrameGeneratorCapturerCreated(
        test::FrameGeneratorCapturer* frame_generator_capturer) override {
      frame_generator_capturer->ChangeResolution(640, 360);
    }

    void OnSinkWantsChanged(VideoSinkInterface<VideoFrame>* /* sink */,
                            const VideoSinkWants& /* wants */) override {}

    void ModifySenderBitrateConfig(
        BitrateConstraints* bitrate_config) override {
      bitrate_config->start_bitrate_bps = kMaxBitrate.bps() / 2;
    }

    void OnVideoStreamsCreated(VideoSendStream* send_stream,
                               const std::vector<VideoReceiveStreamInterface*>&
                               /* receive_streams */) override {
      send_stream_ = send_stream;
    }

    size_t GetNumVideoStreams() const override {
      return max_framerates_.size();
    }

    void ModifyVideoConfigs(
        VideoSendStream::Config* send_config,
        std::vector<VideoReceiveStreamInterface::Config>* /* receive_configs */,
        VideoEncoderConfig* encoder_config) override {
      send_config->encoder_settings.encoder_factory = encoder_factory_;
      send_config->rtp.payload_name = payload_name_;
      send_config->rtp.payload_type =
          test::VideoTestConstants::kVideoSendPayloadType;
      encoder_config->video_format.name = payload_name_;
      encoder_config->codec_type = PayloadStringToCodecType(payload_name_);
      encoder_config->max_bitrate_bps = kMaxBitrate.bps();
      for (size_t i = 0; i < max_framerates_.size(); ++i) {
        encoder_config->simulcast_layers[i].max_framerate = max_framerates_[i];
        configured_framerates_[send_config->rtp.ssrcs[i]] = max_framerates_[i];
      }
    }

    void PerformTest() override {
      EXPECT_TRUE(Wait()) << "Timeout while waiting for framerate stats.";
    }

    void VerifyStats() const {
      const bool quick_perf_test = absl::GetFlag(FLAGS_webrtc_quick_perf_test);
      double input_fps = 0.0;
      for (const auto& configured_framerate : configured_framerates_) {
        input_fps = std::max(configured_framerate.second, input_fps);
      }
      for (const auto& encode_frame_rate_list : encode_frame_rate_lists_) {
        const SamplesStatsCounter& values = encode_frame_rate_list.second;
        GetGlobalMetricsLogger()->LogMetric(
            "substream_fps", "encode_frame_rate", values, Unit::kUnitless,
            ImprovementDirection::kNeitherIsBetter);
        if (values.IsEmpty()) {
          continue;
        }
        double average_fps = values.GetAverage();
        uint32_t ssrc = encode_frame_rate_list.first;
        double expected_fps = configured_framerates_.find(ssrc)->second;
        if (quick_perf_test && expected_fps != input_fps)
          EXPECT_NEAR(expected_fps, average_fps, kAllowedFpsDiff);
      }
    }

    Action OnSendRtp(ArrayView<const uint8_t> /* packet */) override {
      const Timestamp now = clock_->CurrentTime();
      if (now - last_getstats_time_ > kMinGetStatsInterval) {
        last_getstats_time_ = now;
        task_queue_->PostTask([this, now]() {
          VideoSendStream::Stats stats = send_stream_->GetStats();
          for (const auto& stat : stats.substreams) {
            encode_frame_rate_lists_[stat.first].AddSample(
                stat.second.encode_frame_rate);
          }
          if (now - start_time_ > kMinRunTime) {
            VerifyStats();
            observation_complete_.Set();
          }
        });
      }
      return SEND_PACKET;
    }

    Clock* const clock_;
    VideoEncoderFactory* const encoder_factory_;
    const std::string payload_name_;
    const std::vector<int> max_framerates_;
    TaskQueueBase* const task_queue_;
    const Timestamp start_time_;
    Timestamp last_getstats_time_;
    VideoSendStream* send_stream_;
    std::map<uint32_t, SamplesStatsCounter> encode_frame_rate_lists_;
    std::map<uint32_t, double> configured_framerates_;
  } test(encoder_factory, payload_name, max_framerates, task_queue());

  RunBaseTest(&test);
}

TEST_F(CallPerfTest, TestEncodeFramerateVp8Simulcast) {
  InternalEncoderFactory internal_encoder_factory;
  test::FunctionVideoEncoderFactory encoder_factory(
      [&internal_encoder_factory](const Environment& env,
                                  const SdpVideoFormat& /* format */) {
        return std::make_unique<SimulcastEncoderAdapter>(
            env, &internal_encoder_factory, nullptr, SdpVideoFormat::VP8());
      });

  TestEncodeFramerate(&encoder_factory, "VP8",
                      /*max_framerates=*/{20, 30});
}

TEST_F(CallPerfTest, TestEncodeFramerateVp8SimulcastLowerInputFps) {
  InternalEncoderFactory internal_encoder_factory;
  test::FunctionVideoEncoderFactory encoder_factory(
      [&internal_encoder_factory](const Environment& env,
                                  const SdpVideoFormat& /* format */) {
        return std::make_unique<SimulcastEncoderAdapter>(
            env, &internal_encoder_factory, nullptr, SdpVideoFormat::VP8());
      });

  TestEncodeFramerate(&encoder_factory, "VP8",
                      /*max_framerates=*/{14, 20});
}

}  // namespace webrtc
