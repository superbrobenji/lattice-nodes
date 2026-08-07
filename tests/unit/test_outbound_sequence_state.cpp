#include <gtest/gtest.h>
#include "mesh/ReplayCache.h"

using namespace lattice::mesh;

TEST(OutboundSequenceStateTest, InitSetsEpochAndResetsSeq) {
  OutboundSequenceState s;
  s.init(5);
  EXPECT_EQ(s.bootEpoch, 5u);
  EXPECT_EQ(s.txSeqNum, 0u);
}

TEST(OutboundSequenceStateTest, NextSeqIncrements) {
  OutboundSequenceState s;
  s.init(5);
  EXPECT_EQ(s.nextSeq(), 1);
  EXPECT_EQ(s.nextSeq(), 2);
  EXPECT_EQ(s.bootEpoch, 5u);
}

TEST(OutboundSequenceStateTest, BumpEpochUpdatesBootEpoch) {
  OutboundSequenceState s;
  s.init(5);
  s.bumpEpoch(6);
  EXPECT_EQ(s.bootEpoch, 6u);
}

TEST(OutboundSequenceStateTest, WasRelayedBeforeFalseInitially) {
  OutboundSequenceState s;
  s.init(1);
  EXPECT_FALSE(s.wasRelayedBefore(1, 5)); // lastRelayedEpoch/Seq both 0, (1,5) is newer
}

TEST(OutboundSequenceStateTest, MarkRelayedThenWasRelayedBeforeTrueForSameOrOlder) {
  OutboundSequenceState s;
  s.init(1);
  s.markRelayed(1, 5);
  EXPECT_TRUE(s.wasRelayedBefore(1, 5));  // same — not newer
  EXPECT_TRUE(s.wasRelayedBefore(1, 4));  // older seq
  EXPECT_FALSE(s.wasRelayedBefore(1, 6)); // newer seq
  EXPECT_FALSE(s.wasRelayedBefore(2, 0)); // newer epoch
}
