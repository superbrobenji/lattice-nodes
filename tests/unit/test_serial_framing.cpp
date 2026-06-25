#include <gtest/gtest.h>
#include "EEPROM.h"
#include "time_mock.h"
#include "serial_mock.h"
#include "Adapter/Serial_Adapter/Serial_Adapter.h"

using planetopia::adapter::Serial_Adapter;

class SerialFramingTest : public ::testing::Test {
protected:
  void SetUp() override {
    EEPROM.reset();
    resetMillis();
    Serial.reset();
  }

  // Build a raw 2-byte LE length-prefixed frame for a given payload
  std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>(payload.size() & 0xFF));
    frame.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFF));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
  }
};

TEST_F(SerialFramingTest, ValidFrame_SingleOpcode_Parsed) {
  // OP_HEALTH_REQ = 0xB0, 11 bytes padding
  std::vector<uint8_t> payload(12, 0x00);
  payload[0] = 0xB0;
  auto frame = buildFrame(payload);

  Serial_Adapter adapter(-1);
  bool frameComplete = false;
  for (uint8_t b : frame) {
    frameComplete = adapter.injectByte(b);
  }
  EXPECT_TRUE(frameComplete);
  EXPECT_EQ(adapter.lastOpcode(), 0xB0);
}

TEST_F(SerialFramingTest, TruncatedFrame_NotComplete) {
  std::vector<uint8_t> payload(12, 0xAA);
  auto frame = buildFrame(payload);

  Serial_Adapter adapter(-1);
  // Feed only half the frame
  for (size_t i = 0; i < frame.size() / 2; ++i) {
    EXPECT_FALSE(adapter.injectByte(frame[i]));
  }
}

TEST_F(SerialFramingTest, ZeroLengthFrame_Rejected) {
  // Length = 0 should be rejected without a complete-frame signal
  std::vector<uint8_t> frame = {0x00, 0x00};
  Serial_Adapter adapter(-1);
  bool result = false;
  for (uint8_t b : frame) result = adapter.injectByte(b);
  EXPECT_FALSE(result);
}

TEST_F(SerialFramingTest, TooLargeFrame_Rejected) {
  // Length > 256 (MAX_PAYLOAD) should reset state
  std::vector<uint8_t> header = {0x00, 0x20};  // 0x2000 = 8192 — too large
  Serial_Adapter adapter(-1);
  for (uint8_t b : header) adapter.injectByte(b);
  // After rejection, next valid frame should parse correctly
  std::vector<uint8_t> payload(12, 0xB1);
  auto validFrame = buildFrame(payload);
  bool frameComplete = false;
  for (uint8_t b : validFrame) frameComplete = adapter.injectByte(b);
  EXPECT_TRUE(frameComplete);
}

TEST_F(SerialFramingTest, MultipleFrames_ParsedSequentially) {
  Serial_Adapter adapter(-1);
  for (int i = 0; i < 3; ++i) {
    std::vector<uint8_t> payload(12, static_cast<uint8_t>(i));
    auto frame = buildFrame(payload);
    bool frameComplete = false;
    for (uint8_t b : frame) frameComplete = adapter.injectByte(b);
    EXPECT_TRUE(frameComplete) << "Frame " << i << " did not complete";
  }
}
