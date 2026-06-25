#ifndef SERIAL_ADAPTER_H
#define SERIAL_ADAPTER_H

#include "src/Adapter/Adapter.h"

namespace planetopia {
namespace adapter {

class Serial_Adapter : public Adapter {
public:
  explicit Serial_Adapter(int pin);

  bool init() override;
  void loop() override;
  void onMeshDataImpl(const planetopia::mesh::mesh_message& message) override;

  // Serial control opcodes (shared between serial and mesh paths)
  static constexpr uint8_t OP_CONFIG_SET    = 0xA0;  // [A0][6B targetMac][1B adapterType]
  static constexpr uint8_t OP_HEALTH_REQ    = 0xB0;  // [B0]
  static constexpr uint8_t OP_HEALTH_REPORT = 0xB1;  // [B1][1B adapterType][6B mac][4B uptime]

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

  // Encode/decode MeshMessage as Protobuf (hand-rolled minimal encoder)
  static size_t writeVarint(uint8_t* out, uint32_t value);
  static size_t writeZigZag32(uint8_t* out, int32_t value);
  static size_t writeKey(uint8_t* out, uint32_t fieldNumber, uint8_t wireType);
  static size_t writeBytesField(uint8_t* out, uint32_t fieldNumber, const uint8_t* data, size_t len);
  static size_t writeSint32Field(uint8_t* out, uint32_t fieldNumber, int32_t value);
  static size_t writeUint32Field(uint8_t* out, uint32_t fieldNumber, uint32_t value);

  static bool readVarint(const uint8_t*& ptr, const uint8_t* end, uint32_t& out);
  static bool readZigZag32(const uint8_t*& ptr, const uint8_t* end, int32_t& out);
  static bool readKey(const uint8_t*& ptr, const uint8_t* end, uint32_t& fieldNumber, uint8_t& wireType);
  static bool readLengthDelimited(const uint8_t*& ptr, const uint8_t* end, const uint8_t*& dataPtr, size_t& dataLen);

  size_t encodeMeshMessage(const planetopia::mesh::mesh_message& msg, uint8_t* out, size_t outCap);
  bool decodeMeshMessage(const uint8_t* data, size_t len, planetopia::mesh::mesh_message& outMsg);
  void handleCompleteFrame(const uint8_t* data, size_t len);
  // Interpret messageType for Serial control:
  // messageType == 0 (ADAPTER_DATA): targeted send via normal mesh transmit (to master)
  // messageType == 3 (SERIAL_MSG_BROADCAST): broadcast adapter data via mesh
  static constexpr uint32_t SERIAL_MSG_BROADCAST = 3;

  // Health reporter
  static void sendHealthReport();
  static uint32_t lastHealthMillis;
};

}  // namespace adapter
}  // namespace planetopia

#endif  // SERIAL_ADAPTER_H
