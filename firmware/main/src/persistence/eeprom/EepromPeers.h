#ifndef LATTICE_EEPROM_PEERS_H
#define LATTICE_EEPROM_PEERS_H
#include <cstdint>
#include <cstddef>
namespace lattice {
namespace eeprom {
bool loadPeerList(uint8_t* peerRecords, size_t maxPeers);
void savePeerList(const uint8_t* peerRecords, size_t numPeers);
bool hasPeers();
void clearPeerList();

// Phase I Task 6 (JJ): per-key peer-record accessors — each record lives at
// its own "peer0".."peer9" NVS key instead of one combined "peers" blob
// (real nvs_get_blob has no offset/partial-read mode, so per-key naming is
// the only way for a caller to stream records one at a time without holding
// the whole MAX_PEERS*PEER_RECORD_SIZE array on the stack). loadPeerList/
// savePeerList above are now thin loops over these three, kept for their
// existing callers (main.cpp's default-peer bootstrap, host tests) — the
// per-record functions exist so PeerRegistry can avoid the combined buffer
// entirely. `record` must point to EEPROM_SIZES::PEER_RECORD_SIZE bytes.
// index must be < EEPROM_SIZES::MAX_PEERS.
bool loadPeerRecord(uint8_t index, uint8_t* record);
void savePeerRecord(uint8_t index, const uint8_t* record);
void erasePeerRecord(uint8_t index);
} // namespace eeprom
} // namespace lattice
#endif
