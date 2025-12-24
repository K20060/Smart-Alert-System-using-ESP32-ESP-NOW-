/*
  Transmitter.ino
  ESP32 + HC-SR04 transmitter for ESP-NOW Room Alert
  - Robust single-read ultrasonic routine with retries
  - Forces WiFi channel to match receiver
  - Adds receiver as ESP-NOW peer
  - Sends DetectionMsg containing sender MAC, seq, distance, detected flag, timestamp
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "esp_err.h"

// --- Pins (use the pins you tested) ---
const int TRIG_PIN = 14;
const int ECHO_PIN = 27;

// Send interval (ms)
const unsigned long SEND_INTERVAL = 700;

// Receiver info (replace with the MAC printed by receiver)
uint8_t receiverMAC[6] = { 0x58, 0xBF, 0x25, 0x82, 0x78, 0xF9 };
const uint8_t tx_channel = 6; // must match receiver AP channel

// Message structure
struct __attribute__((packed)) DetectionMsg {
  uint8_t sender_mac[6];
  uint32_t seq;
  uint16_t distance_cm;
  uint8_t detected;
  uint32_t timestamp;
};

// Globals
DetectionMsg msg;
uint32_t seq = 0;
unsigned long lastSend = 0;

// --- Helper: add peer (must be declared before initESPNow)
bool addEspNowPeer(const uint8_t *peerMac, uint8_t channel = 0) {
  esp_now_peer_info_t peerInfo;
  memset(&peerInfo, 0, sizeof(peerInfo));
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = channel;   // 0 = use current channel; else explicit
  peerInfo.encrypt = false;

  esp_err_t rc = esp_now_add_peer(&peerInfo);
  if (rc == ESP_OK) {
    Serial.println("Added ESP-NOW peer OK");
    return true;
  } else if (rc == ESP_ERR_ESPNOW_EXIST) {
    Serial.println("Peer already exists");
    return true;
  } else {
    Serial.print("esp_now_add_peer failed: ");
    Serial.print(rc);
    Serial.print(" ");
    Serial.println(esp_err_to_name(rc));
    return false;
  }
}

// ---------- Ultrasonic single-shot read ----------
uint16_t readUltrasonicOnceCM() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // wait for echo up to 30ms (30000us)
  unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
  if (duration == 0) return 0;
  uint16_t distance = (uint16_t)((duration / 2.0) * 0.0343 + 0.5);
  return distance;
}

// ---------- init ESP-NOW and peers ----------
void initESPNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // force radio to receiver channel
  esp_err_t r = esp_wifi_set_channel(tx_channel, WIFI_SECOND_CHAN_NONE);
  if (r != ESP_OK) {
    Serial.print("esp_wifi_set_channel failed: ");
    Serial.println(esp_err_to_name(r));
  } else {
    Serial.print("WiFi channel set to ");
    Serial.println(tx_channel);
  }

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while (1) { delay(1000); }
  }
  Serial.println("ESP-NOW initialized");

  // Add receiver as peer (explicit channel)
  addEspNowPeer(receiverMAC, tx_channel);

  // Optional: add broadcast fallback
  uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  addEspNowPeer(broadcastAddress, tx_channel);
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(100);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  delay(200);

  // Fill sender MAC into message (from Arduino API)
  String macStr = WiFi.macAddress(); // "AA:BB:CC:DD:EE:FF"
  char macBuf[20];
  macStr.toCharArray(macBuf, sizeof(macBuf));
  char *tok = strtok(macBuf, ":");
  int m = 0;
  while (tok != NULL && m < 6) {
    msg.sender_mac[m++] = (uint8_t) strtol(tok, NULL, 16);
    tok = strtok(NULL, ":");
  }
  for (; m < 6; ++m) msg.sender_mac[m] = 0;

  Serial.print("Transmitter MAC: ");
  for (int i = 0; i < 6; ++i) {
    if (msg.sender_mac[i] < 16) Serial.print('0');
    Serial.print(msg.sender_mac[i], HEX);
    if (i < 5) Serial.print(':');
  }
  Serial.println();

  initESPNow();
  Serial.println("Starting measurement + send loop...");
}

// ---------- loop: single read, retry, send ----------
void loop() {
  unsigned long now = millis();
  if (now - lastSend < SEND_INTERVAL) return;
  lastSend = now;

  // Try up to 3 times if we get 0 (timeout)
  uint16_t d = 0;
  const int MAX_RETRIES = 3;
  for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
    d = readUltrasonicOnceCM();
    if (d != 0) break;
    delay(80); // pause before retry
  }

  // Diagnostic print (same value we will send)
  if (d == 0) {
    Serial.println("Measured: raw_us=0  (no echo / timeout)");
  } else {
    Serial.print("Measured dist_cm=");
    Serial.println(d);
  }

  // Build and send message using the exact measured value
  bool detected = (d > 0 && d <= 80); // threshold (adjust if desired)
  msg.seq = seq++;
  msg.distance_cm = d;
  msg.detected = detected ? 1 : 0;
  msg.timestamp = now;

  esp_err_t result = esp_now_send(receiverMAC, (uint8_t *)&msg, sizeof(msg));
  if (result == ESP_OK) {
    Serial.printf("Sent seq=%u dist=%u detected=%u\n", msg.seq, msg.distance_cm, msg.detected);
  } else {
    Serial.print("Send failed: 0x");
    Serial.print(result, HEX);
    Serial.print(" ");
    Serial.println(esp_err_to_name(result));
  }

  // small safety delay
  delay(10);
}
