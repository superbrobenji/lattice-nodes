#include "mesh_controller.h"

//TODO turn file into class
// REPLACE WITH THE MAC Address of your receiver
uint8_t broadcastAddress[] = { 0xEC, 0x64, 0xC9, 0x5D, 0x22, 0x20 };
uint8_t deviceMacAddress[6];

// Create a struct_message to hold incoming sensor readings
mesh_message dataToReceive;

esp_now_peer_info_t peerInfo;

// Callback when data is sent
void OnDataSentCallback(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("\r\nLast Packet Send Status:\t");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Delivery Success" : "Delivery Fail");
  if (status == 0) {
    Serial.print("\r\nDelivery Sucess\t");
  } else {
    Serial.print("\r\nDelivery Fail :(\t");
    // digitalWrite(redLed, HIGH);
    //delay(1000);
    //digitalWrite(redLed, LOW);
  }
}

// Callback when data is received
void OnDataRecvCallback(const esp_now_recv_info *mac, const uint8_t *incomingData, int len) {
  memcpy(&dataToReceive, incomingData, sizeof(dataToReceive));
  Serial.print("Bytes received: ");
  //digitalWrite(greenLed, HIGH);
  //delay(500);
  //digitalWrite(greenLed, LOW);
  //TODO create way to hook adapter into here to recieve data from mesh
  //TODO create way to chain messages across nodes

  Serial.println(len);
}

uint8_t *readMacAddress() {
  uint8_t baseMac[6];
  esp_err_t ret = esp_wifi_get_mac(WIFI_IF_STA, baseMac);
  if (ret != ESP_OK) {
    Serial.println("Failed to read MAC address");
  }
  return baseMac;
}

void setupMesh() {

  // Set device as a Wi-Fi Station
  WiFi.mode(WIFI_STA);

  WiFi.STA.begin();
  memcpy(deviceMacAddress, readMacAddress(), 6);
  WiFi.STA.end();

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    //digitalWrite(redLed, HIGH);
    return;
  }

  // Once ESPNow is successfully Init, we will register for Send CB to
  // get the status of Trasnmitted packet
  esp_now_register_send_cb(OnDataSentCallback);

  //TODOadd peers dynamically
  // Register peer
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  // Add peer
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    //digitalWrite(redLed, HIGH);
    return;
  }
  // Register for a callback function that will be called when data is received and sent
  esp_now_register_recv_cb(OnDataRecvCallback);
}

void transmitData(mesh_message dataToTransmit) {
  // Send message via ESP-NOW
  memcpy(dataToTransmit.originMacAddress, deviceMacAddress, 6);

  esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *)&dataToTransmit, sizeof(dataToTransmit));
  if (result == ESP_OK) {
    Serial.println("Sent with success");
  } else {
    Serial.println("Error sending the data");
    //digitalWrite(redLed, HIGH);
    //delay(1000);
    //digitalWrite(redLed, LOW);
  }
}