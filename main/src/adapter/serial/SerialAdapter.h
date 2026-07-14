#ifndef SERIAL_ADAPTER_H
#define SERIAL_ADAPTER_H

#include "src/adapter/Adapter.h"
#include "src/mesh/serialization/nanopb/pb.h"
#include "src/mesh/serialization/nanopb/pb_encode.h"
#include "src/mesh/serialization/nanopb/pb_decode.h"
#include "src/mesh/serialization/mesh.pb.h"
// Shared protocol constants — source of truth is lattice-protocol repo (git submodule)
#include "lib/lattice-protocol/c/opcodes.h"
#include "lib/lattice-protocol/c/adapter_types.h"
#include "SerialFraming.h"

namespace lattice {
namespace adapter {

class SerialAdapter : public Adapter {
public:
  explicit SerialAdapter(int pin);

  bool init() override;
  void loop() override;
  void onMeshDataImpl(const lattice::mesh::mesh_message& message) override;

  // Opcodes from lattice-protocol/opcodes.h (included above):
  //   OP_HEALTH_REQ    0xB0  [B0]
  //   OP_HEALTH_REPORT 0xB1  [B1][1B adapterType][6B mac][4B uptime]
  //   OP_NODE_HEALTH   0xB2  [B2][1B adapterType][6B mac][4B uptime]
  //   OP_NODE_ID_SET   0xC0  [C0][6B targetMAC][1B nodeId]
  //   OP_CONFIG_SET    0xC1  [C1][6B targetMac][1B adapterType]
  //   OP_TX_POWER_SET  0xC2  [C2][1B preset: 0=short 1=indoor 2=outdoor]
  //   OP_LED_SOLID     0xD0  [D0][1B R][1B G][1B B]
  //   OP_LED_OFF       0xD1
  //   OP_LED_BLINK     0xD2
  //   OP_RELAY_SET     0xD8
  //   OP_COMMAND_ACK   0xE0  [E0][1B commandId]

  // Relay a completed enrollment public key to the server over serial
  static void relayEnrollmentToServer(const uint8_t* mac, const uint8_t* pubKey);

#if SIMULATE_MODE
  // WARNING: SIMULATE_MODE opcodes 0xD0-0xD2 overlap with OP_LED_SOLID/OFF/BLINK from the
  // shared protocol. SIMULATE_MODE must never be enabled alongside LED output handling.
  static constexpr uint8_t OP_SIM_PIR_TRIGGER =
      0xD0; // Inject fake PIR event (conflicts with OP_LED_SOLID when SIMULATE_MODE=1)
  static constexpr uint8_t OP_SIM_FAKE_BEACON =
      0xD1; // Inject fake master beacon [D1][6B mac][4B epoch][2B seq]
  static constexpr uint8_t OP_SIM_FAKE_PEER = 0xD2;  // Inject fake peer [D2][6B mac][32B pubkey]
  static constexpr uint8_t OP_SIM_DUMP_STATE = 0xD3; // Dump current mesh state to serial
#endif

private:
  lattice::adapter::serial::SerialFraming _framing;

  void handleCompleteFrame(const uint8_t* data, size_t len);
  // Interpret messageType for Serial control (uses lattice::mesh::MeshMessageType):
  // MESH_TYPE_ADAPTER_DATA (0)         : targeted send via normal mesh transmit (to master)
  // MESH_TYPE_SERIAL_CMD_BROADCAST (3) : broadcast adapter data via mesh (server→device)
  // MESH_TYPE_JOIN_ACK (4)             : server approved or rejected enrollment (server→device)

  // Health reporter
  static void sendHealthReport();
  static uint32_t lastHealthMillis;
  uint32_t lastReportedHopCount;
};

} // namespace adapter
} // namespace lattice

#endif // SERIAL_ADAPTER_H
