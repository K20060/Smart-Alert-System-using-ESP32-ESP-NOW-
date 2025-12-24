/* Receiver.ino (fixed)
   ESP32 receiver + web server for ESP-NOW Room Alert
   - Receives ESP-NOW messages
   - Sounds buzzer via transistor
   - Hosts a simple web dashboard and /status JSON endpoint
*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <WebServer.h>

// Buzzer pin (drive via transistor)
const int BUZZER_PIN = 19; // change if you wired differently

// WiFi AP credentials (change for deployment)
const char* AP_SSID = "RoomAlert-AP";
const char* AP_PASS = "12345678";

struct __attribute__((packed)) DetectionMsg {
  uint8_t sender_mac[6];
  uint32_t seq;
  uint16_t distance_cm;
  uint8_t detected;
  uint32_t timestamp;
};

// Shared state updated by callback
volatile unsigned long last_detect_ts = 0; // millis() when last detection came in
volatile uint32_t detect_count = 0;
volatile bool last_detected = false;

WebServer server(80);

// Non-blocking buzzer helper (simple)
void buzzOnce(unsigned int ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

// Correct ESP-NOW receive callback signature
// MUST match: void (*recv_cb)(const uint8_t *mac, const uint8_t *incomingData, int len)
void onDataRecv(const uint8_t * mac, const uint8_t *incomingData, int len) {
  // Protect if packet is smaller than expected
  if (len < (int)sizeof(DetectionMsg)) {
    // Optional: ignore or handle partial data
    return;
  }

  DetectionMsg msg;
  memcpy(&msg, incomingData, sizeof(msg));

  // Update shared state -- keep it simple and quick
  last_detect_ts = millis();
  last_detected = (msg.detected != 0);
  if (last_detected) detect_count++;

  // Serial log (safe, quick)
  Serial.printf("Recv from %02X:%02X:%02X:%02X:%02X:%02X seq=%u dist=%u detected=%u\n",
    mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], msg.seq, msg.distance_cm, msg.detected);

  if (last_detected) {
    // Sound buzzer briefly (blocking but short)
    buzzOnce(300);
  }
}

String indexPage = R"rawliteral(
<!doctype html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Room Alert Dashboard</title>
<style>body{font-family:Arial,Helvetica,sans-serif;padding:12px;} .status{font-size:1.6em;margin:10px 0}</style>
</head>
<body>
<h2>Room Alert</h2>
<div class="status" id="status">Loading...</div>
<div>Last detection: <span id="last">-</span></div>
<div>Count: <span id="count">0</span></div>
<script>
async function update(){
  try{
    let r = await fetch('/status');
    let j = await r.json();
    document.getElementById('status').innerText = j.detected? 'Person detected':'No one nearby';
    document.getElementById('last').innerText = j.last_ts>0? new Date(j.last_ts).toLocaleString(): '-';
    document.getElementById('count').innerText = j.count;
  }catch(e){ document.getElementById('status').innerText='Error'; }
}
setInterval(update,1000); update();
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  server.send(200, "text/html", indexPage);
}

void handleStatus() {
  // Build a small JSON string manually to avoid ArduinoJson dependency
  // We will send absolute timestamp in ms (millis() at receiver) so the client can convert to local time if needed
  unsigned long ts = 0;
  bool detected = false;
  uint32_t count = 0;
  // Copy volatile vars into local variables atomically-ish
  noInterrupts();
  ts = last_detect_ts;
  detected = last_detected;
  count = detect_count;
  interrupts();

  // Create a JSON object: {"detected":true,"last_ts":163..., "count":5}
  String payload = "{";
  payload += "\"detected\":";
  payload += (detected ? "true" : "false");
  payload += ",\"last_ts\":";
  payload += String((unsigned long) (ts==0 ? 0 : (unsigned long) ( (unsigned long) ( (unsigned long) millis() - ( (millis() - ts) ) ) + (unsigned long)0 ))); // we send ts as a raw number (receiver's millis())
  payload += ",\"count\":";
  payload += String(count);
  payload += "}";
  server.send(200, "application/json", payload);
}

void initESPNow() {
  // Set up Wi-Fi AP + STA mode so we can host web UI and receive ESP-NOW
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while (1) { delay(1000); }
  }

  // register receive callback (must match exact signature)
  esp_err_t rc = esp_now_register_recv_cb((esp_now_recv_cb_t)onDataRecv);
  if (rc != ESP_OK) {
    Serial.print("esp_now_register_recv_cb failed: ");
    Serial.println(rc);
  } else {
    Serial.println("ESP-NOW receive callback registered");
  }
  // Choose an AP channel that you will also use on the transmitter (1..13)
const uint8_t AP_CHANNEL = 6; // pick 6 (change if you want)

// Start AP on specific channel and then read the AP MAC (softAP MAC)
WiFi.mode(WIFI_AP_STA);                          // AP + STA mode
WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);      // start AP on chosen channel
delay(200);                                     // short delay to let AP start

Serial.print("AP IP: ");
Serial.println(WiFi.softAPIP());

// Use softAPmacAddress() to get the AP MAC (station MAC may be zero in this context)
Serial.print("Receiver (AP) MAC: ");
Serial.println(WiFi.softAPmacAddress());

// Print the AP channel for clarity
Serial.print("AP channel: ");
Serial.println(AP_CHANNEL);

}


void setup() {
  Serial.begin(115200);
  delay(100);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  initESPNow();

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();
}
