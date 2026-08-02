#pragma once
#include <cstddef>
#include <cstdint>
#include "lib/lattice-protocol/c/mesh_message.h"

// Bring ::mesh_message into lattice::mesh namespace (mirrors Adapter.h)
namespace lattice {
namespace mesh {
using ::mesh_message;
} // namespace mesh
} // namespace lattice

namespace lattice {
namespace adapter {
namespace serial {

class SerialFraming {
public:
  // Encode a mesh_message into a nanopb protobuf byte stream.
  // Returns number of bytes written, or 0 on failure.
  // The caller is responsible for adding the 2-byte length prefix before transmission.
  static size_t encode(const lattice::mesh::mesh_message& msg, uint8_t* out, size_t maxLen);

  // Decode a nanopb protobuf byte stream into a mesh_message.
  // Returns true on success.
  static bool decode(const uint8_t* data, size_t len, lattice::mesh::mesh_message& out);

  // Feed one byte into the framing state machine.
  // Returns true when a complete frame is ready; read it via frameBuffer()/frameLen().
  bool injectByte(uint8_t b);

  const uint8_t* frameBuffer() const { return payloadBuffer; }
  size_t frameLen() const { return frameIndex; }

private:
  enum class FrameState : uint8_t { AwaitingLen1, AwaitingLen2, AwaitingPayload };
  FrameState frameState{FrameState::AwaitingLen1};
  uint16_t frameLength{0};
  size_t frameIndex{0};
  static constexpr size_t MAX_PAYLOAD = 256;
  uint8_t payloadBuffer[MAX_PAYLOAD]{};
};

} // namespace serial
} // namespace adapter
} // namespace lattice
