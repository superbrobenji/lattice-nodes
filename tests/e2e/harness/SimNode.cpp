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

using lattice::utils::EepromManager;

SimNode::SimNode(const NodeConfig& cfg) : cfg_(cfg) {
  memcpy(ctx_.mac, cfg.mac, 6);
}

SimNode::~SimNode() {
  // Destroy firmware objects while OUR globals are live so any dtor-side
  // effects land in this node's context, then capture.
  swapIn(ctx_);
  adapter_.reset();
  mesh_.reset();
  swapOut(ctx_);
}

void SimNode::boot() {
  swapIn(ctx_);
  Serial.begin(115200);
  lattice::utils::Logger::setLogLevel(lattice::utils::LogLevel::LOG_NONE);

  auto& em = EepromManager::getInstance();
  em.init();
  lattice::app::BootManager::check(em);
  em.setDevMode(false);
  lattice::adapter::AdapterFactory::setDevMode(false);

  greenLed_ = std::make_unique<lattice::hardware::Led>(lattice::config::GREEN_LED_PIN);
  redLed_ = std::make_unique<lattice::hardware::Led>(lattice::config::RED_LED_PIN);
  greenLed_->init();
  redLed_->init();
  lattice::hardware::Led::setSystemErrorLed(redLed_.get());
  lattice::utils::ErrorCore::getInstance().init(redLed_.get(), nullptr);

  if (!booted_) {
    // First boot: seed role + adapter type (a provisioned device's EEPROM)
    em.saveMasterFlag(cfg_.isMaster);
    lattice::adapter::AdapterFactory::saveAdapterTypeToEEPROM(cfg_.adapterType);
    em.forceFlush();
  }

  lattice::adapter::AdapterFactory::initializeDefaultsIfUnset();
  adapter_.reset(lattice::adapter::AdapterFactory::createFromEEPROM());
  if (!adapter_ || !adapter_->init()) throw lattice::err::FatalError("SimNode: adapter init failed");

  mesh_ = std::make_unique<lattice::mesh::Mesh>();
  if (!mesh_->init()) throw lattice::err::FatalError("SimNode: mesh init failed");
  mesh_->setEnrollmentRelayFn(lattice::adapter::SerialAdapter::relayEnrollmentToServer);
  mesh_->setIsMaster(EepromManager::getInstance().loadMasterFlag());
  adapter_->setTransmitFn(&lattice::mesh::Mesh::transmit);
  mesh_->linkDataRecvCallback([this](const mesh_message& m) {
    if (adapter_) adapter_->onMeshData(m);
  });

  booted_ = true;
  swapOut(ctx_);
}

void SimNode::tick() {
  swapIn(ctx_);
  try {
    lattice::utils::ErrorCore::getInstance().drainPendingBlink();
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
    if (adapter_) adapter_->loop();
  } catch (...) {
    swapOut(ctx_);
    throw;
  }
  swapOut(ctx_);
}

void SimNode::reboot() {
  swapIn(ctx_);
  adapter_.reset();
  mesh_.reset();
  lattice::mesh::Mesh::instance = nullptr;
  lattice::adapter::PirAdapter::instance = nullptr;
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
    if (!pir) throw std::runtime_error("simulatePirMotion: node has no PIR adapter");
    pir->simulateMotion();
    return 0;
  });
}

} // namespace sim
