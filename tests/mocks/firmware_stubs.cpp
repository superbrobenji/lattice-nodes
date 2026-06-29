// firmware_stubs.cpp — stub implementations for firmware symbols not compiled in host tests
// These stubs satisfy the linker for translation units that reference Mesh and other
// hardware-coupled symbols that are excluded from the host build.

#include "Arduino.h"
#include "esp_wifi_mock.h"
#include <cstdint>
#include <cstddef>

// ---- Mesh stubs ----
// Mesh.cpp is not compiled on host (it pulls in mbedtls, esp_now internals, etc.)
// Provide minimal stubs for the symbols Serial_Adapter.cpp and Adapter.cpp reference.

namespace planetopia {
namespace mesh {

// Forward declaration matching Mesh.h types
struct mesh_message;

// Mesh::instance static member
class Mesh;
Mesh* Mesh_instance_stub = nullptr;

} // namespace mesh
} // namespace planetopia

// Mesh::instance — defined as a static member in Mesh.h; we need the storage
// We can't easily stub the class here without including Mesh.h (which drags in esp_now.h etc.)
// Instead we use an asm trick to provide the symbol.
// Actually: include Mesh.h since we have mock headers for all its deps.

#include "esp_now.h"
#include "WiFi.h"

// Now include Mesh.h to get the class definition
#include "src/Mesh/Mesh.h"

namespace planetopia {
namespace mesh {

// Static member definition
Mesh* Mesh::instance = nullptr;

// transmit — static function pointer; defined as a static member
void Mesh::transmit(const planetopia::adapter::adapter_types, const uint8_t[64]) {
  // stub: no-op in tests
}

void Mesh::broadcastAdapterDataStatic(planetopia::adapter::adapter_types, const uint8_t[64]) {
  // stub: no-op in tests
}

// Mesh::enrollPeer — stub
void Mesh::enrollPeer(const uint8_t*, const uint8_t*) {
  // stub: no-op in tests
}

// All other Mesh methods required by the linker — minimal stubs
Mesh::Mesh()
    : meshKey{}, deviceMacAddress{}, lastSeenMasterMac{}, peerInfo{}, peerMacs{}, peerCount(0),
      externalRecvCallback(nullptr), currentMaster{}, isMaster(false), lastBeaconMillis(0),
      lastMasterBeaconReceivedMs(0), devicePrivateKey{}, devicePublicKey{}, bootEpoch(0),
      txSeqNum(0), replayCache{}, replayCacheIdx(0), lastRelayedEpoch(0), lastRelayedSeqNum(0),
      relayPendingMsg{}, relayPendingAt(0), relayPending(false), knownMasterMac{},
      hasMasterMac(false), knownMasterMacSecondary{}, hasMasterMacSecondary(false),
      _dualMasterMode(false), recvQueueHead(0), recvQueueTail(0), lastBeaconMs(0) {}

bool Mesh::init() {
  return true;
}

// linkDataRecvCallback is implemented in Mesh.cpp (real logic)
void Mesh::broadcastMasterBeacon() {}
void Mesh::checkMasterTimeout() {}
void Mesh::loop() {}

void Mesh::addPeer(const uint8_t*) {}
void Mesh::removePeer(const uint8_t*) {}

bool Mesh::isEnrolled() const {
  return false;
}
void Mesh::debugDumpRadio() {}

// Private methods needed for linking
void Mesh::readMacAddress() {}
void Mesh::printMac(const uint8_t*) {}
void Mesh::printMeshMessage(const mesh_message&) {}

void Mesh::onDataSentCallback(const wifi_tx_info_t*, esp_now_send_status_t) {}
void Mesh::IRAM_ATTR dataRecvTrampoline(const esp_now_recv_info*, const uint8_t*, int) {}
void Mesh::IRAM_ATTR onDataRecvCallback(const esp_now_recv_info*, const uint8_t*, int) {}

mesh_message Mesh::buildMessage(adapter_types, const uint8_t[64], MeshMessageType) {
  return mesh_message{};
}

void Mesh::loadPeersFromEEPROM() {}
void Mesh::savePeersToEEPROM() {}
void Mesh::addPeerToEEPROM(const uint8_t*) {}
void Mesh::removePeerFromEEPROM(const uint8_t*) {}

// findPeer, isPeerInRange, findNextHopToMaster are implemented in mesh_logic_impl.cpp (real logic)
// appendPeer is implemented in mesh_logic_impl.cpp (real logic)

// sendMessage is implemented in mesh_logic_impl.cpp (real logic)
// broadcastToAllPeers is implemented in mesh_logic_impl.cpp (real logic)
// transmitCore is implemented in mesh_logic_impl.cpp (real logic)

void Mesh::loadMeshKeyFromEEPROM() {}
void Mesh::saveMeshKeyToEEPROM(const uint8_t*) {}
void Mesh::generateRandomMeshKey() {}
bool Mesh::meshKeyIsSet() const {
  return false;
}

void Mesh::updatePeerLastSeen(const uint8_t*) {}
// processMasterBeacon is implemented in mesh_logic_impl.cpp (real logic)
// processAdapterData is implemented in mesh_logic_impl.cpp (real logic)

bool Mesh::setupWiFi() {
  return true;
}
bool Mesh::setupEspNow() {
  return true;
}
void Mesh::loadPersistentState() {}

// sendEnrollmentRequest and processEnrollmentRequest are implemented in mesh_logic_impl.cpp (real
// logic) processJoinAck is implemented in mesh_logic_impl.cpp (real logic)

void Mesh::loadOrGenerateKeypair() {}
// isReplay, processMasterBeacon, processAdapterData, processJoinAck, and
// drainRecvQueue are implemented in mesh_logic_impl.cpp (real logic)

} // namespace mesh
} // namespace planetopia

// skipField is defined in ProtobufCodec.cpp with external linkage — no stub needed here.
