#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "Adapter/PIR_Adapter/PIR_Adapter.h"
#include "Adapter/AdapterFactory.h"
#include "esp_wifi_mock.h"
#include "time_mock.h"
#include "EEPROM.h"
#include "persistence/EEPROM_Manager.h"
#include "Mesh/Mesh.h"

using namespace planetopia::adapter;

// Capture buffer for transmitted data
static adapter_types lastTxType;
static std::vector<uint8_t> lastTxData;
static int txCallCount = 0;

static void captureTransmit(adapter_types type, const uint8_t data[64]) {
  lastTxType = type;
  lastTxData.assign(data, data + 64);
  ++txCallCount;
}

class PIRHealthTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    planetopia::utils::EEPROM_Manager::getInstance().init();
    resetMillis();
    resetWifiMock();
    lastTxType = adapter_types::UNKNOWN_ADAPTER;
    lastTxData.clear();
    txCallCount = 0;
  }
};

// Helper: construct + init a PIR_Adapter with the capture transmit fn wired up.
// Pin 27 is valid per GpioInput::isValidInputPin.
static PIR_Adapter* makePir() {
  auto* pir = new PIR_Adapter(27);
  pir->setTransmitFn(captureTransmit);
  pir->init(); // sets PIR_Adapter::instance = pir; also marks _initialized
  return pir;
}

TEST_F(PIRHealthTest, SendsNodeHealthAfter30s) {
  PIR_Adapter* pir = makePir();

  // Advance past the 30s threshold
  advanceMillis(30001);
  pir->loop();

  ASSERT_EQ(txCallCount, 1);
  ASSERT_EQ(lastTxType, adapter_types::SERIAL_ADAPTER);
  ASSERT_EQ(lastTxData.size(), 64u);

  // data[0]: opcode 0xB2
  EXPECT_EQ(lastTxData[0], 0xB2u);
  // data[1]: adapterTypeToEEPROM(PIR_ADAPTER) == 0
  EXPECT_EQ(lastTxData[1], 0x00u);
  // data[2..7]: mockDeviceMac default {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF}
  const uint8_t expectedMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  EXPECT_EQ(memcmp(&lastTxData[2], expectedMac, 6), 0);
  // data[8..11]: uptime in seconds (30001ms / 1000 = 30)
  uint32_t uptime =
      static_cast<uint32_t>(lastTxData[8]) | (static_cast<uint32_t>(lastTxData[9]) << 8) |
      (static_cast<uint32_t>(lastTxData[10]) << 16) | (static_cast<uint32_t>(lastTxData[11]) << 24);
  EXPECT_EQ(uptime, 30u);

  delete pir;
}

TEST_F(PIRHealthTest, DoesNotSendNodeHealthBefore30s) {
  PIR_Adapter* pir = makePir();

  advanceMillis(29999);
  pir->loop();

  EXPECT_EQ(txCallCount, 0);

  delete pir;
}

TEST_F(PIRHealthTest, SendsHealthExactlyAtThreshold) {
  PIR_Adapter* pir = makePir();

  advanceMillis(30000);
  pir->loop();

  EXPECT_EQ(txCallCount, 1);
  EXPECT_EQ(lastTxData[0], 0xB2u);

  delete pir;
}

TEST_F(PIRHealthTest, DoesNotSendHealthTwiceWithinInterval) {
  PIR_Adapter* pir = makePir();

  advanceMillis(30001);
  pir->loop();
  ASSERT_EQ(txCallCount, 1);

  // Advance another 5s (not yet another full interval)
  advanceMillis(5000);
  pir->loop();

  EXPECT_EQ(txCallCount, 1);

  delete pir;
}

TEST_F(PIRHealthTest, SendsHealthAgainAfterSecondInterval) {
  PIR_Adapter* pir = makePir();

  advanceMillis(30001);
  pir->loop();
  ASSERT_EQ(txCallCount, 1);

  // Advance another full interval from when the last health was sent
  advanceMillis(30000);
  pir->loop();

  EXPECT_EQ(txCallCount, 2);

  delete pir;
}

TEST_F(PIRHealthTest, UptimeReflectsActualMillis) {
  PIR_Adapter* pir = makePir();

  // Simulate 2 minutes elapsed
  advanceMillis(120001);
  // Single loop() call fires once at the first threshold; uptime = millis() / 1000 at send time.
  pir->loop();

  ASSERT_EQ(txCallCount, 1);
  uint32_t uptime =
      static_cast<uint32_t>(lastTxData[8]) | (static_cast<uint32_t>(lastTxData[9]) << 8) |
      (static_cast<uint32_t>(lastTxData[10]) << 16) | (static_cast<uint32_t>(lastTxData[11]) << 24);
  EXPECT_EQ(uptime, 120u);

  delete pir;
}

// -----------------------------------------------------------------------
// OP_NODE_ID_SET (0xC0) dispatch via Adapter::onMeshData
// -----------------------------------------------------------------------

TEST_F(PIRHealthTest, OpNodeIdSet_AssignsNodeId_WhenTargetMatchesMac) {
  PIR_Adapter* pir = new PIR_Adapter(2);
  pir->setTransmitFn([](adapter_types, const uint8_t[64]) {});

  // Set mockDeviceMac to known value
  mockDeviceMac[0] = 0x11;
  mockDeviceMac[1] = 0x22;
  mockDeviceMac[2] = 0x33;
  mockDeviceMac[3] = 0x44;
  mockDeviceMac[4] = 0x55;
  mockDeviceMac[5] = 0x66;

  planetopia::mesh::mesh_message msg{};
  msg.dataType = adapter_types::SERIAL_ADAPTER;
  msg.data[0] = 0xC0; // OP_NODE_ID_SET
  // target MAC = mockDeviceMac
  memcpy(&msg.data[1], mockDeviceMac, 6);
  msg.data[7] = 99; // nodeId

  pir->onMeshData(msg);

  EXPECT_EQ(planetopia::utils::EEPROM_Manager::getInstance().loadNodeId(), 99u);
  delete pir;
}

TEST_F(PIRHealthTest, OpNodeIdSet_IgnoresMessage_WhenTargetMismatch) {
  PIR_Adapter* pir = new PIR_Adapter(2);
  pir->setTransmitFn([](adapter_types, const uint8_t[64]) {});

  mockDeviceMac[0] = 0xAA;
  mockDeviceMac[1] = 0xBB;
  mockDeviceMac[2] = 0xCC;
  mockDeviceMac[3] = 0xDD;
  mockDeviceMac[4] = 0xEE;
  mockDeviceMac[5] = 0xFF;

  planetopia::mesh::mesh_message msg{};
  msg.dataType = adapter_types::SERIAL_ADAPTER;
  msg.data[0] = 0xC0;
  uint8_t differentMac[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  memcpy(&msg.data[1], differentMac, 6);
  msg.data[7] = 55;

  pir->onMeshData(msg);

  EXPECT_EQ(planetopia::utils::EEPROM_Manager::getInstance().loadNodeId(), 0u);
  delete pir;
}
