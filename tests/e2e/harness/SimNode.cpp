#include "SimNode.h"
#include "Arduino.h"
#include "esp_wifi_mock.h"
#include "esp_now_mock.h"
#include "src/mesh/Mesh.h"
#include "src/adapter/AdapterFactory.h"
#include "src/adapter/serial/SerialAdapter.h"
#include "src/adapter/pir/PirAdapter.h"
#include "src/app/BootManager.h"
#include "src/error/ErrorCore.h"
#include "src/error/Error.h"
#include "src/hardware/output/Led.h"
#include "src/persistence/EepromManager.h"
#include "src/logging/Logger.h"
#include "project_config.h"
#include <cstring>
#include <stdexcept>

namespace sim {

SimNode::SimNode(const NodeConfig& cfg) : cfg_(cfg) {
  memcpy(ctx_.mac, cfg.mac, 6);
}

SimNode::~SimNode() {
  // Destroy firmware objects while OUR globals are live so any dtor-side
  // effects land in this node's context, then capture.
  swapIn(ctx_);
  lattice::mesh::Mesh::instance = nullptr;
  lattice::adapter::PirAdapter::instance = nullptr;
  adapter_.reset();
  mesh_.reset();
  swapOut(ctx_);
}

void SimNode::boot() {
  swapIn(ctx_);
  try {
    Serial.begin(115200);
    lattice::utils::Logger::setLogLevel(lattice::utils::LogLevel::LOG_NONE);

    lattice::eeprom::init();
    lattice::app::BootManager::check();
    lattice::eeprom::setDevMode(false);
    lattice::adapter::AdapterFactory::setDevMode(false);

    greenLed_ = std::make_unique<lattice::hardware::Led>(lattice::config::GREEN_LED_PIN);
    redLed_ = std::make_unique<lattice::hardware::Led>(lattice::config::RED_LED_PIN);
    greenLed_->init();
    redLed_->init();
    lattice::hardware::Led::setSystemErrorLed(redLed_.get());
    lattice::err_core::init(redLed_.get(), nullptr);

    if (!booted_) {
      // First boot: seed role + adapter type (a provisioned device's EEPROM)
      lattice::eeprom::saveMasterFlag(cfg_.isMaster);
      lattice::adapter::AdapterFactory::saveAdapterTypeToEEPROM(cfg_.adapterType);
      // Phase D (#42): seed a fixed keypair BEFORE mesh_->init() (below) calls
      // Enrollment::init(), so its loadKeypair() succeeds and skips generating a
      // fresh random one — see NodeConfig::seedPrivateKey32/seedPublicKey32.
      if (cfg_.seedPrivateKey32 && cfg_.seedPublicKey32) {
        lattice::eeprom::saveKeypair(cfg_.seedPrivateKey32, cfg_.seedPublicKey32);
      }
      lattice::eeprom::forceFlush();
    }

    lattice::adapter::AdapterFactory::initializeDefaultsIfUnset();
    adapter_.reset(lattice::adapter::AdapterFactory::createFromEEPROM());
    if (!adapter_ || !adapter_->init())
      throw lattice::err::FatalError("SimNode: adapter init failed");

    mesh_ = std::make_unique<lattice::mesh::Mesh>();
    if (!mesh_->init())
      throw lattice::err::FatalError("SimNode: mesh init failed");

    if (!booted_) {
      // PeerRegistry::loadFromEEPROM() falls back to lattice::config::DEFAULT_PEERS
      // (two placeholder MACs meant to be replaced before real flashing) whenever a
      // freshly-provisioned node's persisted peer list is empty -- which every SimNode
      // is on first boot. Those placeholder MACs don't correspond to any node in a
      // simulated world, so the moment a broadcast actually targets them,
      // VirtualBus::deliver() throws ("frame to unknown MAC"). Strip them here, once,
      // for every node, so tests observe real firmware/mesh behavior instead of
      // crashing on this harness/config artifact.
      //
      // Phase D (#42) fix: this MUST run only on first boot (matching the comment's
      // stated intent -- it didn't, previously: it ran unconditionally on every
      // boot() call, including reboots). lattice::config::DEFAULT_PEERS[0] is
      // {0xAA,0xBB,0xCC,0xDD,0xEE,0x01}, which is also lattice::mesh::pin::MASTER_MAC
      // (tests/mocks/master_pubkey_pin.h) -- now that the pinned e2e master's real
      // MAC equals that same value (see MeshSimTest::MAC_MASTER), re-running this
      // strip on every reboot would remove a node's legitimately-enrolled master
      // peer entry on its next reboot, mistaking it for the unprovisioned
      // placeholder and corrupting PeerRegistry state.
      for (int i = 0; i < lattice::config::NUM_DEFAULT_PEERS; ++i)
        mesh_->peers.removeAndPersist(lattice::config::DEFAULT_PEERS[i]);
    }

    mesh_->setEnrollmentRelayFn(lattice::adapter::SerialAdapter::relayEnrollmentToServer);
    mesh_->setIsMaster(lattice::eeprom::loadMasterFlag());
    adapter_->setTransmitFn(&lattice::mesh::Mesh::transmit);
    mesh_->linkDataRecvCallback([this](const mesh_message& m) {
      if (adapter_)
        adapter_->onMeshData(m);
    });

    booted_ = true;
  } catch (...) {
    swapOut(ctx_);
    throw;
  }
  swapOut(ctx_);
}

void SimNode::tick() {
  swapIn(ctx_);
  try {
    lattice::err_core::drainPendingBlink();
    mesh_->loop();
    mesh_->checkMasterTimeout();

    // Enrollment state machine (mirrors main.ino loop)
    if (!mesh_->isEnrolled() && !mesh_->getIsMaster()) {
      if (millis() - lastEnrollmentBroadcastMs_ > 10000) {
        lastEnrollmentBroadcastMs_ = millis();
        mesh_->sendEnrollmentRequest();
      }
      swapOut(ctx_);
      return;
    }
    if (adapter_)
      adapter_->loop();
  } catch (...) {
    swapOut(ctx_);
    throw;
  }
  swapOut(ctx_);
}

void SimNode::reboot() {
  swapIn(ctx_);
  lattice::mesh::Mesh::instance = nullptr;
  lattice::adapter::PirAdapter::instance = nullptr;
  adapter_.reset();
  mesh_.reset();
  ESP._restartRequested = false;
  // EEPROM image survives; everything volatile resets
  Serial.reset();
  resetEspNowMock();
  swapOut(ctx_);
  boot();
}

bool SimNode::isEnrolled() {
  return with([](lattice::mesh::Mesh& m, lattice::adapter::Adapter*) { return m.isEnrolled(); });
}

void SimNode::simulatePirMotion() {
  with([](lattice::mesh::Mesh&, lattice::adapter::Adapter* a) {
    auto* pir = dynamic_cast<lattice::adapter::PirAdapter*>(a);
    if (!pir)
      throw std::runtime_error("simulatePirMotion: node has no PIR adapter");
    pir->simulateMotion();
    return 0;
  });
}

} // namespace sim
