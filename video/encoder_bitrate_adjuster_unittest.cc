/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "video/encoder_bitrate_adjuster.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/field_trials.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "api/video/video_bitrate_allocation.h"
#include "api/video/video_codec_constants.h"
#include "api/video/video_codec_type.h"
#include "api/video_codecs/scalability_mode.h"
#include "api/video_codecs/video_codec.h"
#include "api/video_codecs/video_encoder.h"
#include "rtc_base/checks.h"
#include "rtc_base/time_utils.h"
#include "test/create_test_field_trials.h"
#include "test/gtest.h"
#include "test/time_controller/simulated_time_controller.h"

namespace webrtc {
namespace test {

using ::testing::Test;
using ::testing::Values;
using ::testing::WithParamInterface;

class EncoderBitrateAdjusterTest : public Test,
                                   public WithParamInterface<std::string> {
 public:
  static constexpr int64_t kWindowSizeMs = 3000;
  static constexpr int kDefaultBitrateBps = 300000;
  static constexpr int kDefaultFrameRateFps = 30;
  // For network utilization higher than media utilization, loop over a
  // sequence where the first half undershoots and the second half overshoots
  // by the same amount.
  static constexpr int kSequenceLength = 4;
  static_assert(kSequenceLength % 2 == 0, "Sequence length must be even.");

  EncoderBitrateAdjusterTest()
      : time_controller_(/*start_time=*/Timestamp::Millis(123)),
        target_bitrate_(DataRate::BitsPerSec(kDefaultBitrateBps)),
        target_framerate_fps_(kDefaultFrameRateFps),
        tl_pattern_idx_{},
        sequence_idx_{},
        field_trials_(CreateTestFieldTrials(GetParam())) {}

 protected:
  void SetUpAdjusterWithCodec(size_t num_spatial_layers,
                              size_t num_temporal_layers,
                              const VideoCodec& codec) {
    codec_ = codec;
    for (size_t si = 0; si < num_spatial_layers; ++si) {
      encoder_info_.fps_allocation[si].resize(num_temporal_layers);
      double fraction = 1.0;
      for (int ti = num_temporal_layers - 1; ti >= 0; --ti) {
        encoder_info_.fps_allocation[si][ti] = static_cast<uint8_t>(
            VideoEncoder::EncoderInfo::kMaxFramerateFraction * fraction + 0.5);
        fraction /= 2.0;
      }
    }

    adjuster_ = std::make_unique<EncoderBitrateAdjuster>(
        codec_, field_trials_, *time_controller_.GetClock());
    adjuster_->OnEncoderInfo(encoder_info_);
    current_adjusted_allocation_ =
        adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
            current_input_allocation_, target_framerate_fps_));
  }

  void SetUpAdjuster(size_t num_spatial_layers,
                     size_t num_temporal_layers,
                     bool vp9_svc) {
    // Initialize some default VideoCodec instance with the given number of
    // layers.
    VideoCodec codec;
    if (vp9_svc) {
      codec.codecType = VideoCodecType::kVideoCodecVP9;
      codec.numberOfSimulcastStreams = 1;
      codec.VP9()->numberOfSpatialLayers = num_spatial_layers;
      codec.VP9()->numberOfTemporalLayers = num_temporal_layers;
      for (size_t si = 0; si < num_spatial_layers; ++si) {
        codec.spatialLayers[si].minBitrate = 100 * (1 << si);
        codec.spatialLayers[si].targetBitrate = 200 * (1 << si);
        codec.spatialLayers[si].maxBitrate = 300 * (1 << si);
        codec.spatialLayers[si].active = true;
        codec.spatialLayers[si].numberOfTemporalLayers = num_temporal_layers;
      }
    } else {
      codec.codecType = VideoCodecType::kVideoCodecVP8;
      codec.numberOfSimulcastStreams = num_spatial_layers;
      codec.VP8()->numberOfTemporalLayers = num_temporal_layers;
      for (size_t si = 0; si < num_spatial_layers; ++si) {
        codec.simulcastStream[si].minBitrate = 100 * (1 << si);
        codec.simulcastStream[si].targetBitrate = 200 * (1 << si);
        codec.simulcastStream[si].maxBitrate = 300 * (1 << si);
        codec.simulcastStream[si].active = true;
        codec.simulcastStream[si].numberOfTemporalLayers = num_temporal_layers;
      }
    }
    SetUpAdjusterWithCodec(num_spatial_layers, num_temporal_layers, codec);
  }

  void InsertFrames(std::vector<std::vector<double>> media_utilization_factors,
                    int64_t duration_ms) {
    InsertFrames(media_utilization_factors, media_utilization_factors,
                 duration_ms);
  }

  void InsertFrames(
      std::vector<std::vector<double>> media_utilization_factors,
      std::vector<std::vector<double>> network_utilization_factors,
      int64_t duration_ms) {
    RTC_DCHECK_EQ(media_utilization_factors.size(),
                  network_utilization_factors.size());

    const int64_t start_us = TimeMicros();
    while (TimeMicros() < start_us + (duration_ms * kNumMicrosecsPerMillisec)) {
      time_controller_.AdvanceTime(TimeDelta::Seconds(1) /
                                   target_framerate_fps_);
      for (size_t si = 0; si < NumSpatialLayers(); ++si) {
        const std::vector<int>& tl_pattern =
            kTlPatterns[NumTemporalLayers(si) - 1];
        const size_t ti =
            tl_pattern[(tl_pattern_idx_[si]++) % tl_pattern.size()];

        uint32_t layer_bitrate_bps =
            current_adjusted_allocation_.GetBitrate(si, ti);
        double layer_framerate_fps = target_framerate_fps_;
        if (encoder_info_.fps_allocation[si].size() > ti) {
          uint8_t layer_fps_fraction = encoder_info_.fps_allocation[si][ti];
          if (ti > 0) {
            // We're interested in the frame rate for this layer only, not
            // cumulative frame rate.
            layer_fps_fraction -= encoder_info_.fps_allocation[si][ti - 1];
          }
          layer_framerate_fps =
              (target_framerate_fps_ * layer_fps_fraction) /
              VideoEncoder::EncoderInfo::kMaxFramerateFraction;
        }
        double media_utilization_factor = 1.0;
        double network_utilization_factor = 1.0;
        if (media_utilization_factors.size() > si) {
          RTC_DCHECK_EQ(media_utilization_factors[si].size(),
                        network_utilization_factors[si].size());
          if (media_utilization_factors[si].size() > ti) {
            media_utilization_factor = media_utilization_factors[si][ti];
            network_utilization_factor = network_utilization_factors[si][ti];
          }
        }
        RTC_DCHECK_GE(network_utilization_factor, media_utilization_factor);

        // Frame size based on constant (media) overshoot.
        const size_t media_frame_size = media_utilization_factor *
                                        (layer_bitrate_bps / 8.0) /
                                        layer_framerate_fps;

        constexpr int kFramesWithPenalty = (kSequenceLength / 2) - 1;
        RTC_DCHECK_GT(kFramesWithPenalty, 0);

        // The positive/negative size diff needed to achieve network rate but
        // not media rate penalty is the difference between the utilization
        // factors times the media rate frame size, then scaled by the fraction
        // between total frames and penalized frames in the sequence.
        // Cap to media frame size to avoid negative size undershoot.
        const size_t network_frame_size_diff_bytes = std::min(
            media_frame_size,
            static_cast<size_t>(
                (((network_utilization_factor - media_utilization_factor) *
                  media_frame_size) *
                 kSequenceLength) /
                    kFramesWithPenalty +
                0.5));

        int sequence_idx = sequence_idx_[si][ti];
        sequence_idx_[si][ti] = (sequence_idx_[si][ti] + 1) % kSequenceLength;
        const DataSize frame_size = DataSize::Bytes(
            (sequence_idx < kSequenceLength / 2)
                ? media_frame_size - network_frame_size_diff_bytes
                : media_frame_size + network_frame_size_diff_bytes);

        adjuster_->OnEncodedFrame(frame_size, si, ti);
        sequence_idx = ++sequence_idx % kSequenceLength;
      }
    }
  }

  size_t NumSpatialLayers() const {
    if (codec_.codecType == VideoCodecType::kVideoCodecVP9) {
      return codec_.VP9().numberOfSpatialLayers;
    }
    return codec_.numberOfSimulcastStreams;
  }

  size_t NumTemporalLayers(int spatial_index) {
    if (codec_.codecType == VideoCodecType::kVideoCodecVP9) {
      return codec_.spatialLayers[spatial_index].numberOfTemporalLayers;
    }
    return codec_.simulcastStream[spatial_index].numberOfTemporalLayers;
  }

  void ExpectNear(const VideoBitrateAllocation& expected_allocation,
                  const VideoBitrateAllocation& actual_allocation,
                  double allowed_error_fraction) {
    for (size_t si = 0; si < kMaxSpatialLayers; ++si) {
      for (size_t ti = 0; ti < kMaxTemporalStreams; ++ti) {
        if (expected_allocation.HasBitrate(si, ti)) {
          EXPECT_TRUE(actual_allocation.HasBitrate(si, ti));
          uint32_t expected_layer_bitrate_bps =
              expected_allocation.GetBitrate(si, ti);
          EXPECT_NEAR(expected_layer_bitrate_bps,
                      actual_allocation.GetBitrate(si, ti),
                      static_cast<uint32_t>(expected_layer_bitrate_bps *
                                            allowed_error_fraction));
        } else {
          EXPECT_FALSE(actual_allocation.HasBitrate(si, ti));
        }
      }
    }
  }

  VideoBitrateAllocation MultiplyAllocation(
      const VideoBitrateAllocation& allocation,
      double factor) {
    VideoBitrateAllocation multiplied_allocation;
    for (size_t si = 0; si < kMaxSpatialLayers; ++si) {
      for (size_t ti = 0; ti < kMaxTemporalStreams; ++ti) {
        if (allocation.HasBitrate(si, ti)) {
          multiplied_allocation.SetBitrate(
              si, ti,
              static_cast<uint32_t>(factor * allocation.GetBitrate(si, ti) +
                                    0.5));
        }
      }
    }
    return multiplied_allocation;
  }

  GlobalSimulatedTimeController time_controller_;

  VideoCodec codec_;
  VideoEncoder::EncoderInfo encoder_info_;
  std::unique_ptr<EncoderBitrateAdjuster> adjuster_;
  VideoBitrateAllocation current_input_allocation_;
  VideoBitrateAllocation current_adjusted_allocation_;

  DataRate target_bitrate_;
  double target_framerate_fps_;
  int tl_pattern_idx_[kMaxSpatialLayers];
  int sequence_idx_[kMaxSpatialLayers][kMaxTemporalStreams];
  FieldTrials field_trials_;

  const std::vector<int> kTlPatterns[kMaxTemporalStreams] = {
      {0},
      {0, 1},
      {0, 2, 1, 2},
      {0, 3, 2, 3, 1, 3, 2, 3}};
};

TEST_P(EncoderBitrateAdjusterTest, SingleLayerOptimal) {
  // Single layer, well behaved encoder.
  current_input_allocation_.SetBitrate(0, 0, 300000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 1, false);
  InsertFrames({{1.0}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  // Adjusted allocation near input. Allow 1% error margin due to rounding
  // errors etc.
  ExpectNear(current_input_allocation_, current_adjusted_allocation_, 0.01);
}

TEST_P(EncoderBitrateAdjusterTest, SingleLayerOveruse) {
  // Single layer, well behaved encoder.
  current_input_allocation_.SetBitrate(0, 0, 300000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 1, false);
  InsertFrames({{1.2}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  // Adjusted allocation lowered by 20%.
  ExpectNear(MultiplyAllocation(current_input_allocation_, 1 / 1.2),
             current_adjusted_allocation_, 0.01);
}

TEST_P(EncoderBitrateAdjusterTest, SingleLayerUnderuse) {
  // Single layer, well behaved encoder.
  current_input_allocation_.SetBitrate(0, 0, 300000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 1, false);
  InsertFrames({{0.5}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  // Undershoot, adjusted should exactly match input.
  ExpectNear(current_input_allocation_, current_adjusted_allocation_, 0.00);
}

TEST_P(EncoderBitrateAdjusterTest, ThreeTemporalLayersOptimalSize) {
  // Three temporal layers, 60%/20%/20% bps distro, well behaved encoder.
  current_input_allocation_.SetBitrate(0, 0, 180000);
  current_input_allocation_.SetBitrate(0, 1, 60000);
  current_input_allocation_.SetBitrate(0, 2, 60000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 3, false);
  InsertFrames({{1.0, 1.0, 1.0}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  ExpectNear(current_input_allocation_, current_adjusted_allocation_, 0.01);
}

TEST_P(EncoderBitrateAdjusterTest, ThreeTemporalLayersOvershoot) {
  // Three temporal layers, 60%/20%/20% bps distro.
  // 10% overshoot on all layers.
  current_input_allocation_.SetBitrate(0, 0, 180000);
  current_input_allocation_.SetBitrate(0, 1, 60000);
  current_input_allocation_.SetBitrate(0, 2, 60000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 3, false);
  InsertFrames({{1.1, 1.1, 1.1}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  // Adjusted allocation lowered by 10%.
  ExpectNear(MultiplyAllocation(current_input_allocation_, 1 / 1.1),
             current_adjusted_allocation_, 0.01);
}

TEST_P(EncoderBitrateAdjusterTest, ThreeTemporalLayersUndershoot) {
  // Three temporal layers, 60%/20%/20% bps distro, undershoot all layers.
  current_input_allocation_.SetBitrate(0, 0, 180000);
  current_input_allocation_.SetBitrate(0, 1, 60000);
  current_input_allocation_.SetBitrate(0, 2, 60000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 3, false);
  InsertFrames({{0.8, 0.8, 0.8}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  // Adjusted allocation identical since we don't boost bitrates.
  ExpectNear(current_input_allocation_, current_adjusted_allocation_, 0.0);
}

TEST_P(EncoderBitrateAdjusterTest, ThreeTemporalLayersSkewedOvershoot) {
  // Three temporal layers, 60%/20%/20% bps distro.
  // 10% overshoot on base layer, 20% on higher layers.
  current_input_allocation_.SetBitrate(0, 0, 180000);
  current_input_allocation_.SetBitrate(0, 1, 60000);
  current_input_allocation_.SetBitrate(0, 2, 60000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 3, false);
  InsertFrames({{1.1, 1.2, 1.2}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  // Expected overshoot is weighted by bitrate:
  // (0.6 * 1.1 + 0.2 * 1.2 + 0.2 * 1.2) = 1.14
  ExpectNear(MultiplyAllocation(current_input_allocation_, 1 / 1.14),
             current_adjusted_allocation_, 0.01);
}

TEST_P(EncoderBitrateAdjusterTest, ThreeTemporalLayersNonLayeredEncoder) {
  // Three temporal layers, 60%/20%/20% bps allocation, 10% overshoot,
  // encoder does not actually support temporal layers.
  current_input_allocation_.SetBitrate(0, 0, 180000);
  current_input_allocation_.SetBitrate(0, 1, 60000);
  current_input_allocation_.SetBitrate(0, 2, 60000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 1, false);
  InsertFrames({{1.1}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  // Expect the actual 10% overuse to be detected and the allocation to
  // only contain the one entry.
  VideoBitrateAllocation expected_allocation;
  expected_allocation.SetBitrate(
      0, 0,
      static_cast<uint32_t>(current_input_allocation_.get_sum_bps() / 1.10));
  ExpectNear(expected_allocation, current_adjusted_allocation_, 0.01);
}

TEST_P(EncoderBitrateAdjusterTest, IgnoredStream) {
  // Encoder with three temporal layers, but in a mode that does not support
  // deterministic frame rate. Those are ignored, even if bitrate overshoots.
  current_input_allocation_.SetBitrate(0, 0, 180000);
  current_input_allocation_.SetBitrate(0, 1, 60000);
  target_framerate_fps_ = 30;
  SetUpAdjuster(1, 1, false);
  encoder_info_.fps_allocation[0].clear();
  adjuster_->OnEncoderInfo(encoder_info_);

  InsertFrames({{1.1}}, kWindowSizeMs);
  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));

  // Values passed through.
  ExpectNear(current_input_allocation_, current_adjusted_allocation_, 0.00);
}

TEST_P(EncoderBitrateAdjusterTest, DifferentSpatialOvershoots) {
  // Two streams, both with three temporal layers.
  // S0 has 5% overshoot, S1 has 25% overshoot.
  current_input_allocation_.SetBitrate(0, 0, 180000);
  current_input_allocation_.SetBitrate(0, 1, 60000);
  current_input_allocation_.SetBitrate(0, 2, 60000);
  current_input_allocation_.SetBitrate(1, 0, 400000);
  current_input_allocation_.SetBitrate(1, 1, 150000);
  current_input_allocation_.SetBitrate(1, 2, 150000);
  target_framerate_fps_ = 30;
  // Run twice, once configured as simulcast and once as VP9 SVC.
  for (int i = 0; i < 2; ++i) {
    SetUpAdjuster(2, 3, i == 0);
    InsertFrames({{1.05, 1.05, 1.05}, {1.25, 1.25, 1.25}}, kWindowSizeMs);
    current_adjusted_allocation_ =
        adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
            current_input_allocation_, target_framerate_fps_));
    VideoBitrateAllocation expected_allocation;
    for (size_t ti = 0; ti < 3; ++ti) {
      expected_allocation.SetBitrate(
          0, ti,
          static_cast<uint32_t>(current_input_allocation_.GetBitrate(0, ti) /
                                1.05));
      expected_allocation.SetBitrate(
          1, ti,
          static_cast<uint32_t>(current_input_allocation_.GetBitrate(1, ti) /
                                1.25));
    }
    ExpectNear(expected_allocation, current_adjusted_allocation_, 0.01);
  }
}

TEST_P(EncoderBitrateAdjusterTest, HeadroomAllowsOvershootToMediaRate) {
  if (GetParam() == "WebRTC-VideoRateControl/adjuster_use_headroom:false/") {
    // This test does not make sense without headroom adjustment.
    GTEST_SKIP();
  }

  // Two streams, both with three temporal layers.
  // Media rate is 1.0, but network rate is higher.
  const uint32_t kS0Bitrate = 300000;
  const uint32_t kS1Bitrate = 900000;
  current_input_allocation_.SetBitrate(0, 0, kS0Bitrate / 3);
  current_input_allocation_.SetBitrate(0, 1, kS0Bitrate / 3);
  current_input_allocation_.SetBitrate(0, 2, kS0Bitrate / 3);
  current_input_allocation_.SetBitrate(1, 0, kS1Bitrate / 3);
  current_input_allocation_.SetBitrate(1, 1, kS1Bitrate / 3);
  current_input_allocation_.SetBitrate(1, 2, kS1Bitrate / 3);

  target_framerate_fps_ = 30;

  // Run twice, once configured as simulcast and once as VP9 SVC.
  for (int i = 0; i < 2; ++i) {
    SetUpAdjuster(2, 3, i == 0);
    // Network rate has 10% overshoot, but media rate is correct at 1.0.
    InsertFrames({{1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}},
                 {{1.1, 1.1, 1.1}, {1.1, 1.1, 1.1}},
                 kWindowSizeMs * kSequenceLength);

    // Push back by 10%.
    current_adjusted_allocation_ =
        adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
            current_input_allocation_, target_framerate_fps_));
    ExpectNear(MultiplyAllocation(current_input_allocation_, 1 / 1.1),
               current_adjusted_allocation_, 0.01);

    // Add 10% link headroom, overshoot is now allowed.
    current_adjusted_allocation_ =
        adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
            current_input_allocation_, target_framerate_fps_,
            DataRate::BitsPerSec(current_input_allocation_.get_sum_bps() *
                                 1.1)));
    ExpectNear(current_input_allocation_, current_adjusted_allocation_, 0.01);
  }
}

TEST_P(EncoderBitrateAdjusterTest, DontExceedMediaRateEvenWithHeadroom) {
  if (GetParam() == "WebRTC-VideoRateControl/adjuster_use_headroom:false/") {
    // This test does not make sense without headroom adjustment.
    GTEST_SKIP();
  }

  // Two streams, both with three temporal layers.
  // Media rate is 1.1, but network rate is higher.

  const uint32_t kS0Bitrate = 300000;
  const uint32_t kS1Bitrate = 900000;
  current_input_allocation_.SetBitrate(0, 0, kS0Bitrate / 3);
  current_input_allocation_.SetBitrate(0, 1, kS0Bitrate / 3);
  current_input_allocation_.SetBitrate(0, 2, kS0Bitrate / 3);
  current_input_allocation_.SetBitrate(1, 0, kS1Bitrate / 3);
  current_input_allocation_.SetBitrate(1, 1, kS1Bitrate / 3);
  current_input_allocation_.SetBitrate(1, 2, kS1Bitrate / 3);

  target_framerate_fps_ = 30;

  // Run twice, once configured as simulcast and once as VP9 SVC.
  for (const bool is_svc : {false, true}) {
    SetUpAdjuster(/*num_spatial_layers=*/2,
                  /*num_temporal_layers=*/3, is_svc);

    // First insert frames with no overshoot.
    InsertFrames({{1.0, 1.0, 1.0}}, kWindowSizeMs * kSequenceLength);
    // Verify encoder is not pushed backed.
    current_adjusted_allocation_ =
        adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
            current_input_allocation_, target_framerate_fps_));
    // The up-down causes a bit more noise, allow slightly more error margin.
    ExpectNear(MultiplyAllocation(current_input_allocation_, 1.0),
               current_adjusted_allocation_, 0.015);

    // Change network rate to 30% overshoot, media rate has 10% overshoot.
    InsertFrames({{1.1, 1.1, 1.1}, {1.1, 1.1, 1.1}},
                 {{1.3, 1.3, 1.3}, {1.3, 1.3, 1.3}},
                 kWindowSizeMs * kSequenceLength);

    // Add 100% link headroom, overshoot from network to media rate is allowed.
    current_adjusted_allocation_ =
        adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
            current_input_allocation_, target_framerate_fps_,
            DataRate::BitsPerSec(current_input_allocation_.get_sum_bps() * 2)));

    ExpectNear(MultiplyAllocation(current_input_allocation_, 1 / 1.1),
               current_adjusted_allocation_, 0.02);
  }
}

TEST_P(EncoderBitrateAdjusterTest, HonorsMinBitrateWithAv1) {
  // Single layer, well behaved encoder.
  const DataRate kHighBitrate = DataRate::KilobitsPerSec(20);
  const DataRate kALowerMinBitrate = DataRate::KilobitsPerSec(15);

  current_input_allocation_.SetBitrate(0, 0, kHighBitrate.bps());

  VideoBitrateAllocation expected_input_allocation;
  expected_input_allocation.SetBitrate(0, 0, kALowerMinBitrate.bps());

  target_framerate_fps_ = 30;

  VideoCodec codec;
  codec.codecType = VideoCodecType::kVideoCodecAV1;
  codec.numberOfSimulcastStreams = 1;
  codec.SetScalabilityMode(ScalabilityMode::kL1T1);
  codec.spatialLayers[0].minBitrate = kALowerMinBitrate.kbps();
  codec.spatialLayers[0].targetBitrate = 500;
  codec.spatialLayers[0].maxBitrate = 1000;
  codec.spatialLayers[0].active = true;
  codec.spatialLayers[0].numberOfTemporalLayers = 1;

  SetUpAdjusterWithCodec(/*num_spatial_layers=*/1, /*num_temporal_layers=*/1,
                         codec);

  InsertFrames({{2.0}}, kWindowSizeMs);

  current_adjusted_allocation_ =
      adjuster_->AdjustRateAllocation(VideoEncoder::RateControlParameters(
          current_input_allocation_, target_framerate_fps_));
  // Adjusted allocation near input. Allow 1% error margin due to rounding
  // errors etc.
  ExpectNear(expected_input_allocation, current_adjusted_allocation_, 0.01);
}

INSTANTIATE_TEST_SUITE_P(
    AdjustWithHeadroomVariations,
    EncoderBitrateAdjusterTest,
    Values("WebRTC-VideoRateControl/adjuster_use_headroom:false/",
           "WebRTC-VideoRateControl/adjuster_use_headroom:true/",
           "WebRTC-VideoRateControl/adjuster_use_headroom:true/"
           "WebRTC-BitrateAdjusterUseNewfangledHeadroomAdjustment/Enabled/"));

}  // namespace test
}  // namespace webrtc
