#ifndef SERIAL_ADAPTER_H
#define SERIAL_ADAPTER_H

#include "src/Adapter/Adapter.h"
#include "src/Mesh/serialization/nanopb/pb.h"
#include "src/Mesh/serialization/nanopb/pb_encode.h"
#include "src/Mesh/serialization/nanopb/pb_decode.h"
#include "src/Mesh/serialization/mesh.pb.h"

namespace planetopia {
namespace adapter {

class Serial_Adapter : public Adapter {
public:
  explicit Serial_Adapter(int pin);

  bool init() override;
  void loop() override;
  void onMeshDataImpl(const planetopia::mesh::mesh_message& message) override;

  // Serial control opcodes (shared between serial and mesh paths)
  static constexpr uint8_t OP_CONFIG_SET       = 0xA0;  // [A0][6B targetMac][1B adapterType]
  static constexpr uint8_t OP_TX_POWER_SET     = 0xA1;  // [A1][1B preset: 0=short 1=indoor 2=outdoor]
  static constexpr uint8_t OP_HEALTH_REQ       = 0xB0;  // [B0]
  static constexpr uint8_t OP_HEALTH_REPORT    = 0xB1;  // [B1][1B adapterType][6B mac][4B uptime]
  // Relay a completed enrollment public key to the server over serial
  static void relayEnrollmentToServer(const uint8_t mac[6], const uint8_t pubKey[32]);

#if SIMULATE_MODE
  static constexpr uint8_t OP_SIM_PIR_TRIGGER = 0xD0;  // Inject fake PIR event
  static constexpr uint8_t OP_SIM_FAKE_BEACON = 0xD1;  // Inject fake master beacon [D1][6B mac][4B epoch][2B seq]
  static constexpr uint8_t OP_SIM_FAKE_PEER   = 0xD2;  // Inject fake peer [D2][6B mac][32B pubkey]
  static constexpr uint8_t OP_SIM_DUMP_STATE  = 0xD3;  // Dump current mesh state to serial
#endif

private:
  // Protobuf-over-serial framing: 2-byte little-endian length prefix + protobuf payload
  enum class FrameState : uint8_t { AwaitingLen1,
                                    AwaitingLen2,
                                    AwaitingPayload };
  FrameState frameState;
  uint16_t frameLength;
  size_t frameIndex;
  static constexpr size_t MAX_PAYLOAD = 256;
  uint8_t payloadBuffer[MAX_PAYLOAD];

  static size_t encodeMeshMessage(const planetopia::mesh::mesh_message& msg, uint8_t* out, size_t outCap);
  static bool decodeMeshMessage(const uint8_t* data, size_t len, planetopia::mesh::mesh_message& outMsg);
  void handleCompleteFrame(const uint8_t* data, size_t len);
  // Interpret messageType for Serial control (uses planetopia::mesh::MeshMessageType):
  // MESH_TYPE_ADAPTER_DATA (0)         : targeted send via normal mesh transmit (to master)
  // MESH_TYPE_SERIAL_CMD_BROADCAST (3) : broadcast adapter data via mesh (server→device)
  // MESH_TYPE_JOIN_ACK (4)             : server approved or rejected enrollment (server→device)

  // Health reporter
  static void sendHealthReport();
  static uint32_t lastHealthMillis;
  uint32_t lastReportedHopCount;

#ifdef UNIT_TEST
public:
  // Feed one byte into the frame state machine.
  // Returns true when a complete frame has been received and processed.
  bool injectByte(uint8_t b);
  uint8_t lastOpcode() const { return _lastCompletedOpcode; }
private:
  uint8_t _lastCompletedOpcode = 0;
#endif
};

}  // namespace adapter
}  // namespace planetopia

#endif  // SERIAL_ADAPTER_H
