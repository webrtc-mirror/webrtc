/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "net/dcsctp/socket/state_cookie.h"

#include <cstdint>
#include <vector>

#include "absl/strings/string_view.h"
#include "net/dcsctp/common/internal_types.h"
#include "net/dcsctp/socket/capabilities.h"
#include "net/dcsctp/testing/testing_macros.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace dcsctp {
namespace {
using ::testing::SizeIs;

TEST(StateCookieTest, SerializeAndDeserialize) {
  Capabilities capabilities = {.partial_reliability = true,
                               .message_interleaving = false,
                               .reconfig = true,
                               .zero_checksum = true,
                               .negotiated_maximum_incoming_streams = 123,
                               .negotiated_maximum_outgoing_streams = 234};
  StateCookie cookie(/*peer_tag=*/VerificationTag(123),
                     /*my_tag=*/VerificationTag(321),
                     /*peer_initial_tsn=*/TSN(456), /*my_initial_tsn=*/TSN(654),
                     /*a_rwnd=*/789, TieTag(101112), capabilities);
  std::vector<uint8_t> serialized = cookie.Serialize();
  EXPECT_THAT(serialized, SizeIs(StateCookie::kCookieSize));
  ASSERT_HAS_VALUE_AND_ASSIGN(StateCookie deserialized,
                              StateCookie::Deserialize(serialized));
  EXPECT_EQ(deserialized.peer_tag(), VerificationTag(123));
  EXPECT_EQ(deserialized.my_tag(), VerificationTag(321));
  EXPECT_EQ(deserialized.peer_initial_tsn(), TSN(456));
  EXPECT_EQ(deserialized.my_initial_tsn(), TSN(654));
  EXPECT_EQ(deserialized.a_rwnd(), 789u);
  EXPECT_EQ(deserialized.tie_tag(), TieTag(101112));
  EXPECT_TRUE(deserialized.capabilities().partial_reliability);
  EXPECT_FALSE(deserialized.capabilities().message_interleaving);
  EXPECT_TRUE(deserialized.capabilities().reconfig);
  EXPECT_TRUE(deserialized.capabilities().zero_checksum);
  EXPECT_EQ(deserialized.capabilities().negotiated_maximum_incoming_streams,
            123);
  EXPECT_EQ(deserialized.capabilities().negotiated_maximum_outgoing_streams,
            234);
}

TEST(StateCookieTest, ValidateMagicValue) {
  Capabilities capabilities = {.partial_reliability = true,
                               .message_interleaving = false,
                               .reconfig = true};
  StateCookie cookie(/*peer_tag=*/VerificationTag(123),
                     /*my_tag=*/VerificationTag(321),
                     /*peer_initial_tsn=*/TSN(456), /*my_initial_tsn=*/TSN(654),
                     /*a_rwnd=*/789, TieTag(101112), capabilities);
  std::vector<uint8_t> serialized = cookie.Serialize();
  ASSERT_THAT(serialized, SizeIs(StateCookie::kCookieSize));

  absl::string_view magic(reinterpret_cast<const char*>(serialized.data()), 8);
  EXPECT_EQ(magic, "dcSCTP00");
}

}  // namespace
}  // namespace dcsctp
