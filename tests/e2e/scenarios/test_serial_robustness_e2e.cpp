// Task 14b: the master's serial framing recovers from garbage input.
//
// Garbage bytes on the serial line (line noise, a desynced server) must not
// wedge the framing state machine: a valid command after the garbage must
// still be processed. In the current firmware an undecodable frame raises
// err::fail(COMM, ADAPTER, 5, "Failed to decode protobuf frame") — this test
// documents that (asserts the error fired, then clears the counter) and, more
// importantly, proves the framing resynchronizes afterward. (That garbage
// escalates to err::fail at all is arguably too aggressive, mirroring finding
// #9 for the mesh side; left as-is here since serial-frame errors may warrant
// operator attention. Flagged, not changed.)
#include "harness/MeshSimTest.h"
#include "lib/lattice-protocol/c/opcodes.h"

TEST_F(MeshSimTest, GarbageOnSerialDoesNotBreakSubsequentFrames) {
  addMaster();
  auto* sensor = addSensor(MAC_NODE_A);
  world.bus.link(master, sensor);
  enroll(sensor);

  // A plausible 2-byte length prefix (32) followed by undecodable payload.
  uint8_t junk[34];
  junk[0] = 32;
  junk[1] = 0;
  for (int i = 2; i < 34; ++i)
    junk[i] = 0x5A;
  auto& rx = master->ctx().serialRx;
  rx.insert(rx.end(), junk, junk + sizeof(junk));

  runPolled(1000);
  EXPECT_GE(lattice_test_errFailCount, 1)
      << "an undecodable serial frame raises a decode error in the current firmware";
  lattice_test_errFailCount = 0; // absolve the expected garbage error for the strict TearDown

  // Framing must resynchronize: a valid command afterward is still answered.
  size_t before = hub->received.size();
  hub->sendHealthReq();
  runPolled(1000);
  EXPECT_GT(hub->received.size(), before)
      << "the master must recover and answer a valid command after garbage input";
}
