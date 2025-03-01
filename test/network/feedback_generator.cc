/*
 *  Copyright 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "test/network/feedback_generator.h"

#include <cstddef>
#include <utility>
#include <vector>

#include "absl/memory/memory.h"
#include "api/test/network_emulation_manager.h"
#include "api/test/simulated_network.h"
#include "api/transport/network_types.h"
#include "api/units/data_rate.h"
#include "api/units/data_size.h"
#include "api/units/time_delta.h"
#include "api/units/timestamp.h"
#include "rtc_base/checks.h"
#include "test/network/network_emulation_manager.h"
#include "test/network/simulated_network.h"

namespace webrtc {

FeedbackGeneratorImpl::FeedbackGeneratorImpl(
    FeedbackGeneratorImpl::Config config)
    : conf_(config),
      net_({.time_mode = TimeMode::kSimulated}),
      send_link_{new SimulatedNetwork(conf_.send_link)},
      ret_link_{new SimulatedNetwork(conf_.return_link)},
      route_(this,
             net_.CreateRoute(
                 {net_.CreateEmulatedNode(absl::WrapUnique(send_link_))}),
             net_.CreateRoute(
                 {net_.CreateEmulatedNode(absl::WrapUnique(ret_link_))})) {}

Timestamp FeedbackGeneratorImpl::Now() {
  return net_.Now();
}

void FeedbackGeneratorImpl::Sleep(TimeDelta duration) {
  net_.time_controller()->AdvanceTime(duration);
}

void FeedbackGeneratorImpl::SendPacket(size_t size) {
  SentPacket sent;
  sent.send_time = Now();
  sent.size = DataSize::Bytes(size);
  sent.sequence_number = sequence_number_++;
  sent_packets_.push(sent);
  route_.SendRequest(size, sent);
}

std::vector<TransportPacketsFeedback> FeedbackGeneratorImpl::PopFeedback() {
  std::vector<TransportPacketsFeedback> ret;
  ret.swap(feedback_);
  return ret;
}

void FeedbackGeneratorImpl::SetSendConfig(BuiltInNetworkBehaviorConfig config) {
  conf_.send_link = config;
  send_link_->SetConfig(conf_.send_link);
}

void FeedbackGeneratorImpl::SetReturnConfig(
    BuiltInNetworkBehaviorConfig config) {
  conf_.return_link = config;
  ret_link_->SetConfig(conf_.return_link);
}

void FeedbackGeneratorImpl::SetSendLinkCapacity(DataRate capacity) {
  conf_.send_link.link_capacity = capacity;
  send_link_->SetConfig(conf_.send_link);
}

void FeedbackGeneratorImpl::OnRequest(SentPacket packet,
                                      Timestamp arrival_time) {
  PacketResult result;
  result.sent_packet = packet;
  result.receive_time = arrival_time;
  received_packets_.push_back(result);
  Timestamp first_recv = received_packets_.front().receive_time;
  if (Now() - first_recv > conf_.feedback_interval) {
    route_.SendResponse(conf_.feedback_packet_size.bytes<size_t>(),
                        std::move(received_packets_));
    received_packets_ = {};
  }
}

void FeedbackGeneratorImpl::OnResponse(std::vector<PacketResult> packet_results,
                                       Timestamp arrival_time) {
  TransportPacketsFeedback feedback;
  feedback.feedback_time = arrival_time;
  std::vector<PacketResult>::const_iterator received_packet_iterator =
      packet_results.begin();
  while (received_packet_iterator != packet_results.end()) {
    RTC_DCHECK(!sent_packets_.empty() &&
               sent_packets_.front().sequence_number <=
                   received_packet_iterator->sent_packet.sequence_number)
        << "reordering not implemented";
    if (sent_packets_.front().sequence_number <
        received_packet_iterator->sent_packet.sequence_number) {
      // Packet lost.
      PacketResult lost;
      lost.sent_packet = sent_packets_.front();
      feedback.packet_feedbacks.push_back(lost);
    }
    if (sent_packets_.front().sequence_number ==
        received_packet_iterator->sent_packet.sequence_number) {
      feedback.packet_feedbacks.push_back(*received_packet_iterator);
      ++received_packet_iterator;
    }
    sent_packets_.pop();
  }
  feedback_.push_back(feedback);
}

}  // namespace webrtc
