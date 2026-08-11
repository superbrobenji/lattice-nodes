#include <gtest/gtest.h>
#include "mesh/ReplayCache.h"
#include "src/error/Error.h"

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

// --- nextSeqGuarded/checkEpochRollback (Round 2 Task 8 — moved from Mesh) ---

TEST(OutboundSequenceStateTest, NextSeqGuardedBumpsEpochOnWrap) {
  OutboundSequenceState s;
  s.init(5);
  s.txSeqNum = 0xFFFE; // one below wrap
  EXPECT_EQ(s.nextSeqGuarded(), 0xFFFF);
  EXPECT_EQ(s.bootEpoch, 5u);
  uint16_t wrapped = s.nextSeqGuarded(); // this draw wraps 0xFFFF -> 0 -> guarded redraw
  EXPECT_EQ(wrapped, 1);                 // epoch bumped, fresh sequence starts at 1
  EXPECT_EQ(s.bootEpoch, 6u);
}

TEST(OutboundSequenceStateTest, CheckEpochRollbackAcceptsFirstCall) {
  OutboundSequenceState s;
  s.init(1);
  s.checkEpochRollback(1, 5); // must not fail — first call always passes
}

TEST(OutboundSequenceStateTest, CheckEpochRollbackAcceptsStrictlyNewer) {
  OutboundSequenceState s;
  s.init(1);
  s.checkEpochRollback(1, 5);
  s.checkEpochRollback(1, 6); // same epoch, higher seq — ok
  s.checkEpochRollback(2, 0); // higher epoch — ok
}

TEST(OutboundSequenceStateTest, CheckEpochRollbackThrowsOnRollback) {
  OutboundSequenceState s;
  s.init(1);
  s.checkEpochRollback(1, 5);
  EXPECT_THROW(s.checkEpochRollback(1, 4), lattice::err::FatalError);
}
