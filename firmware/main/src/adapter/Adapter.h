#ifndef ADAPTER_H
#define ADAPTER_H

#include <cstdint>
#include <cstddef>

// Include generated mesh message type — no circular dependency since lattice-protocol
// headers only include standard types and each other, not local firmware headers.
#include "../../lib/lattice-protocol/c/mesh_message.h"

namespace lattice {
namespace mesh {
using ::mesh_message;
} // namespace mesh
} // namespace lattice

namespace lattice {
namespace adapter {

// Enum for identifying adapter types.
// Unscoped so that int32_t data_type (generated wire struct field) and adapter_types
// can be compared/assigned without explicit casts in both directions.
enum adapter_types : int32_t {
  UNKNOWN_ADAPTER = 0,
  SERIAL_ADAPTER = 1,
  PIR_ADAPTER = 2,
};

// Abstract base class for all adapters
class Adapter {

protected:
  // Hardware pin associated with the adapter. ESP32 GPIO pins are 0-39 (uint8_t
  // is ample headroom); Phase G audit item L shrinks this from int.
  uint8_t _pin;
  adapter_types _adapterType; // Type identifier for the adapter
  typedef void (*TransmitPtr)(adapter_types, const uint8_t*);
  TransmitPtr mesh_transmit_fn;

public:
  explicit Adapter(uint8_t pin); // Constructor
  virtual ~Adapter() = default;  // Ensure polymorphic destruction

  adapter_types getAdapterType() const; // Returns the adapter type
  void sendDataThroughMesh(const adapter_types type,
                           const uint8_t data[64]); // sends data through mesh
  void setTransmitFn(TransmitPtr fn);

  virtual bool init() = 0; // To be implemented by derived classes
  virtual void loop() = 0; // Called repeatedly in the main loop
  // Called when mesh data is received.
  // Handles OP_CONFIG_SET (SERIAL_ADAPTER dataType) for ALL node types in the base class
  // so that any node can be reconfigured regardless of its current adapter type.
  // For all other message types, filters by adapter type before dispatching to onMeshDataImpl().
  void onMeshData(const lattice::mesh::mesh_message& message);

protected:
  // Implement in subclasses: only called when message.dataType == this adapter's type
  virtual void onMeshDataImpl(const lattice::mesh::mesh_message& message);

  // ------------------------------------------------------------------------
  // Phase H2 audit item W: health-report frame builder + shared interval tick.
  // ------------------------------------------------------------------------

  // Builds the health-report payload shared by every adapter's periodic
  // health report: [opcode][1B adapterType][6B MAC][4B little-endian
  // uptime-seconds]. Only fills bytes — callers own transmission, since that
  // differs by adapter (PIR routes through the mesh to reach the master via
  // sendDataThroughMesh; the serial/master adapter injects directly into its
  // own outbound serial pipeline via Mesh::transmitSelfOriginated, see
  // sendSelfHealthReport() below). No-op if bufsize < 12.
  void buildHealthFrame(uint8_t opcode, uint8_t* buf, size_t bufsize) const;

  // Builds an OP_HEALTH_REPORT frame and delivers it via
  // Mesh::transmitSelfOriginated. Meaningful only for a node acting as the
  // serial-attached master (the only adapter with a direct serial link to
  // the server); shared by SerialAdapter's periodic health tick and the
  // OP_HEALTH_REQ control-op handler (see dispatchControlOp below).
  void sendSelfHealthReport() const;

  // Shared "has the health-report interval elapsed" tick used by every
  // adapter's loop(). Returns true exactly once per HEALTH_REPORT_INTERVAL_MS
  // and resets the internal timer when it does. Callers that need to reset
  // the timer early for another reason (e.g. SerialAdapter firing a report
  // immediately on hop-count change) should call resetHealthTick() themselves
  // once they've sent.
  bool healthTickDue(uint64_t now);
  void resetHealthTick(uint64_t now);

  // ------------------------------------------------------------------------
  // Phase H2 audit item V: shared control-op dispatch table.
  // ------------------------------------------------------------------------
  //
  // OP_CONFIG_SET / OP_NODE_ID_SET / OP_HEALTH_REQ / OP_TX_POWER_SET are each
  // handled in exactly one place (below) and reached from both entry points
  // that see them: the mesh-received path (onMeshData, above) and
  // SerialAdapter's direct-serial path (handleCompleteFrame). Target-mac
  // addressing for CONFIG_SET/NODE_ID_SET always comes from the opcode's own
  // payload (data[1..6] — see lattice-protocol/c/opcodes.h), which both
  // entry points populate identically, so handlers don't need it supplied
  // separately. OP_HEALTH_REQ/OP_TX_POWER_SET are meaningful only for a node
  // acting as the serial-attached master, so those two handlers no-op when
  // _adapterType isn't SERIAL_ADAPTER — this reproduces the original
  // behavior where they lived exclusively in SerialAdapter's code path.
  //
  // rebroadcastOnMaster: OP_TX_POWER_SET must re-broadcast to the rest of
  // the mesh only when the preset change was just introduced locally by the
  // server over direct serial — a TX_POWER_SET received via the mesh has
  // already been broadcast once and must not be re-broadcast (would loop).
  using ControlOpFn = void (Adapter::*)(const lattice::mesh::mesh_message&, bool);
  struct ControlOpEntry {
    uint8_t opcode;
    ControlOpFn handler;
  };
  static const ControlOpEntry kControlOps[];
  static const size_t kControlOpCount;

  void opConfigSet(const lattice::mesh::mesh_message& message, bool rebroadcastOnMaster);
  void opNodeIdSet(const lattice::mesh::mesh_message& message, bool rebroadcastOnMaster);
  void opHealthReq(const lattice::mesh::mesh_message& message, bool rebroadcastOnMaster);
  void opTxPowerSet(const lattice::mesh::mesh_message& message, bool rebroadcastOnMaster);

  // Looks up message.data[0] in kControlOps and, if present, invokes the
  // matching handler. Returns true iff a handler ran.
  bool dispatchControlOp(const lattice::mesh::mesh_message& message, bool rebroadcastOnMaster);

private:
  // True if candidateMac (a 6-byte MAC) is either this node's own station MAC
  // or the FF:FF:FF:FF:FF:FF broadcast placeholder.
  static bool isTargetedAtSelf(const uint8_t* candidateMac);

  // Phase I Task 6 (FF): widened uint32_t -> uint64_t alongside the
  // millis() -> esp_timer_get_time()/1000ULL swap.
  uint64_t _lastHealthMillis = 0;
};

} // namespace adapter
} // namespace lattice
#endif
