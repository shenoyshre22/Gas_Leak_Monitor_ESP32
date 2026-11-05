
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "secrets.h"

// ===== 1. CONFIGURATION =====

const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;
const char* TS_API_KEY = SECRET_TS_API_KEY;

const bool AUTO_BASELINE = true;

// Sensitivity settings
int MARGIN = 300;
int THRESHOLD = 1800;

const int HYSTERESIS = 75;

const unsigned long WARMUP_MS = 180000UL;
const unsigned long READ_MS = 1000;
const unsigned long UPLOAD_MS = 20000;

// Safe initial mode: existing HTTP upload stays enabled until MQTT is verified.
// To switch to MQTT-only later, set MQTT true and HTTP false.
const bool ENABLE_MQTT_PUBLISHING = false;
const bool ENABLE_HTTP_PUBLISHING = true;

const char* MQTT_BROKER = "mqtt3.thingspeak.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC = "channels/3566022/publish";
const unsigned long MQTT_RETRY_MS = 5000UL;

const char* BLE_DEVICE_NAME = "GasLeakMonitor";
const char* BLE_SERVICE_UUID = "12345678-1234-1234-1234-123456789000";
const char* BLE_SENSOR_READING_UUID = "12345678-1234-1234-1234-123456789001";
const char* BLE_THRESHOLD_UUID = "12345678-1234-1234-1234-123456789002";
const char* BLE_ALERT_STATUS_UUID = "12345678-1234-1234-1234-123456789003";
const char* BLE_MAX_READING_UUID = "12345678-1234-1234-1234-123456789004";
const char* BLE_AVERAGE_READING_UUID = "12345678-1234-1234-1234-123456789005";
const char* BLE_WARNING_COUNT_UUID = "12345678-1234-1234-1234-123456789006";
const char* BLE_WARNING_DURATION_UUID = "12345678-1234-1234-1234-123456789007";

// ===== 2. SENSOR PIN =====

const int PIN_MQ2 = 34;

// ===== 3. WEB SERVER =====

WebServer server(80);

// MQTT networking runs in its own FreeRTOS task so a slow broker connection
// cannot hold up sensor sampling in loop().
WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);

struct MqttPublishMessage {
  char payload[192];
};

QueueHandle_t mqttPublishQueue = nullptr;
volatile bool mqttConnected = false;

struct BleSnapshot {
  char sensorReading[16];
  char threshold[16];
  char alertStatus[12];
  char maxReading[16];
  char averageReading[20];
  char warningCount[16];
  char warningDuration[24];
};

BLEServer* bleServer = nullptr;
BLECharacteristic* bleSensorReading = nullptr;
BLECharacteristic* bleThreshold = nullptr;
BLECharacteristic* bleAlertStatus = nullptr;
BLECharacteristic* bleMaxReading = nullptr;
BLECharacteristic* bleAverageReading = nullptr;
BLECharacteristic* bleWarningCount = nullptr;
BLECharacteristic* bleWarningDuration = nullptr;
QueueHandle_t bleUpdateQueue = nullptr;
volatile bool bleClientConnected = false;
volatile bool bleRestartAdvertising = false;

// ===== 4. VARIABLES =====

bool alarmOn = false;
bool simulate = false;
bool warming = true;
bool ipShown = false;
bool realAlarmOn = false;

int lastRaw = 0;
int baseline = 0;

// Session analytics reset on ESP32 reboot. Warm-up, calibration, and test-mode
// samples are excluded from these real-sensor statistics.
int maxReading = 0;
uint64_t readingSum = 0;
uint32_t readingCount = 0;
uint32_t warningCount = 0;
uint64_t warningDurationMs = 0;
unsigned long warningLastTick = 0;
int rollingReadings[10] = {0};
uint8_t rollingIndex = 0;
uint8_t rollingCount = 0;
uint32_t rollingSum = 0;

unsigned long lastRead = 0;
unsigned long lastUpload = 0;
unsigned long warmEnd = 0;


// ===== BLE GATT SERVER =====

class GasLeakBleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    (void)server;
    bleClientConnected = true;
    Serial.println("BLE client connected");
  }

  void onDisconnect(BLEServer* server) override {
    (void)server;
    bleClientConnected = false;
    bleRestartAdvertising = true;
    Serial.println("BLE client disconnected");
  }
};


uint64_t warningDurationAt(unsigned long now);


void setBleCharacteristicIfChanged(
  BLECharacteristic* characteristic,
  const char* value
) {
  if (characteristic == nullptr) {
    return;
  }

  String nextValue(value);
  if (characteristic->getValue() == nextValue) {
    return;
  }

  characteristic->setValue(nextValue);

  // Notify only after a subscribed client has enabled the CCCD.
  BLE2902* cccd = (BLE2902*)characteristic->getDescriptorByUUID(
    BLEUUID((uint16_t)0x2902)
  );
  if (
    bleClientConnected &&
    cccd != nullptr &&
    cccd->getNotifications()
  ) {
    characteristic->notify();
  }
}


void applyBleSnapshot(const BleSnapshot& snapshot) {
  setBleCharacteristicIfChanged(bleSensorReading, snapshot.sensorReading);
  setBleCharacteristicIfChanged(bleThreshold, snapshot.threshold);
  setBleCharacteristicIfChanged(bleAlertStatus, snapshot.alertStatus);
  setBleCharacteristicIfChanged(bleMaxReading, snapshot.maxReading);
  setBleCharacteristicIfChanged(bleAverageReading, snapshot.averageReading);
  setBleCharacteristicIfChanged(bleWarningCount, snapshot.warningCount);
  setBleCharacteristicIfChanged(bleWarningDuration, snapshot.warningDuration);
}


void bleWorkerTask(void* parameter) {
  (void)parameter;

  for (;;) {
    if (bleRestartAdvertising) {
      bleRestartAdvertising = false;
      BLEDevice::startAdvertising();
      Serial.println("BLE advertising restarted");
    }

    BleSnapshot snapshot;
    if (
      bleUpdateQueue != nullptr &&
      xQueueReceive(bleUpdateQueue, &snapshot, 0) == pdTRUE
    ) {
      applyBleSnapshot(snapshot);
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}


void makeBleSnapshot(BleSnapshot& snapshot, unsigned long now) {
  snprintf(snapshot.sensorReading, sizeof(snapshot.sensorReading), "%d", lastRaw);
  snprintf(snapshot.threshold, sizeof(snapshot.threshold), "%d", THRESHOLD);
  snprintf(
    snapshot.alertStatus,
    sizeof(snapshot.alertStatus),
    "%s",
    alarmOn ? "WARNING" : "NORMAL"
  );

  if (readingCount > 0) {
    snprintf(snapshot.maxReading, sizeof(snapshot.maxReading), "%d", maxReading);
    snprintf(
      snapshot.averageReading,
      sizeof(snapshot.averageReading),
      "%.1f",
      (double)readingSum / readingCount
    );
  } else {
    snprintf(snapshot.maxReading, sizeof(snapshot.maxReading), "N/A");
    snprintf(snapshot.averageReading, sizeof(snapshot.averageReading), "N/A");
  }

  snprintf(snapshot.warningCount, sizeof(snapshot.warningCount), "%lu", (unsigned long)warningCount);
  snprintf(
    snapshot.warningDuration,
    sizeof(snapshot.warningDuration),
    "%llu",
    (unsigned long long)warningDurationAt(now)
  );
}


bool sameBleSnapshot(const BleSnapshot& left, const BleSnapshot& right) {
  return strcmp(left.sensorReading, right.sensorReading) == 0 &&
         strcmp(left.threshold, right.threshold) == 0 &&
         strcmp(left.alertStatus, right.alertStatus) == 0 &&
         strcmp(left.maxReading, right.maxReading) == 0 &&
         strcmp(left.averageReading, right.averageReading) == 0 &&
         strcmp(left.warningCount, right.warningCount) == 0 &&
         strcmp(left.warningDuration, right.warningDuration) == 0;
}


void queueBleSnapshotIfChanged(unsigned long now) {
  if (bleUpdateQueue == nullptr) {
    return;
  }

  static BleSnapshot lastQueued = {};
  static bool haveLastQueued = false;
  BleSnapshot snapshot;
  makeBleSnapshot(snapshot, now);

  if (haveLastQueued && sameBleSnapshot(snapshot, lastQueued)) {
    return;
  }

  if (xQueueOverwrite(bleUpdateQueue, &snapshot) == pdPASS) {
    lastQueued = snapshot;
    haveLastQueued = true;
  }
}


void addBleCharacteristic(BLEService* service, const char* uuid, BLECharacteristic** target) {
  *target = service->createCharacteristic(
    uuid,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  (*target)->addDescriptor(new BLE2902());
}


void initializeBle() {
  BLEDevice::init(BLE_DEVICE_NAME);
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new GasLeakBleServerCallbacks());

  // Seven characteristics plus seven CCCD descriptors need more than the
  // BLE library's default attribute table size.
  BLEService* service = bleServer->createService(BLEUUID(BLE_SERVICE_UUID), 40);
  addBleCharacteristic(service, BLE_SENSOR_READING_UUID, &bleSensorReading);
  addBleCharacteristic(service, BLE_THRESHOLD_UUID, &bleThreshold);
  addBleCharacteristic(service, BLE_ALERT_STATUS_UUID, &bleAlertStatus);
  addBleCharacteristic(service, BLE_MAX_READING_UUID, &bleMaxReading);
  addBleCharacteristic(service, BLE_AVERAGE_READING_UUID, &bleAverageReading);
  addBleCharacteristic(service, BLE_WARNING_COUNT_UUID, &bleWarningCount);
  addBleCharacteristic(service, BLE_WARNING_DURATION_UUID, &bleWarningDuration);

  BleSnapshot initialSnapshot;
  makeBleSnapshot(initialSnapshot, millis());
  applyBleSnapshot(initialSnapshot);

  service->start();
  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);

  bleUpdateQueue = xQueueCreate(1, sizeof(BleSnapshot));
  if (bleUpdateQueue == nullptr) {
    Serial.println("BLE update queue allocation failed; characteristic updates unavailable");
  } else {
    BaseType_t taskCreated = xTaskCreatePinnedToCore(
      bleWorkerTask,
      "GasLeakBLE",
      4096,
      nullptr,
      1,
      nullptr,
      0
    );
    if (taskCreated != pdPASS) {
      vQueueDelete(bleUpdateQueue);
      bleUpdateQueue = nullptr;
      Serial.println("BLE worker task creation failed; characteristic updates unavailable");
    }
  }

  BLEDevice::startAdvertising();
  Serial.println("BLE initialized");
  Serial.println("BLE advertising started");
}


// ===== SESSION ANALYTICS =====

void accumulateWarningDuration(unsigned long now) {

  if (realAlarmOn) {

    // Unsigned subtraction remains correct across millis() rollover because
    // this function runs continuously in loop().
    warningDurationMs += (unsigned long)(now - warningLastTick);
    warningLastTick = now;
  }
}


uint64_t warningDurationAt(unsigned long now) {

  uint64_t duration = warningDurationMs;

  if (realAlarmOn) {

    duration += (unsigned long)(now - warningLastTick);
  }

  return duration / 1000ULL;
}


String uint64ToString(uint64_t value) {

  char buffer[21];
  snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
  return String(buffer);
}


void addRealReading(int reading, unsigned long now) {

  if (reading < 0 || reading > 4095) {
    return;
  }

  if (readingCount == 0 || reading > maxReading) {
    maxReading = reading;
  }

  readingSum += (uint64_t)reading;
  readingCount++;

  if (rollingCount == 10) {
    rollingSum -= rollingReadings[rollingIndex];
  } else {
    rollingCount++;
  }

  rollingReadings[rollingIndex] = reading;
  rollingSum += (uint32_t)reading;
  rollingIndex = (rollingIndex + 1) % 10;

  bool nextRealAlarm = realAlarmOn;

  if (!realAlarmOn && reading >= THRESHOLD) {
    nextRealAlarm = true;
  } else if (
    realAlarmOn &&
    reading < (THRESHOLD - HYSTERESIS)
  ) {
    nextRealAlarm = false;
  }

  if (nextRealAlarm != realAlarmOn) {

    // Close the prior interval before changing state. Count only a real
    // normal-to-warning transition; simulation never calls this function.
    accumulateWarningDuration(now);

    if (nextRealAlarm) {
      warningCount++;
      warningLastTick = now;
    }

    realAlarmOn = nextRealAlarm;
  }
}


// ===== 5. READ MQ-2 =====

int readMQ2() {

  long sum = 0;

  // Average 8 sensor readings
  for (int i = 0; i < 8; i++) {

    sum += analogRead(PIN_MQ2);

    delay(2);
  }

  return sum / 8;
}


// ===== 6. CONNECT TO WI-FI =====

void connectWiFi(unsigned long timeoutMs) {

  WiFi.mode(WIFI_STA);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long t0 = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - t0 < timeoutMs
  ) {

    delay(500);

    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("\nWi-Fi connected");

  } else {

    Serial.println("\nWi-Fi NOT connected (running offline)");
  }
}


// ===== 7. DISPLAY ESP32 IP =====

void showIP() {

  if (!ipShown && WiFi.status() == WL_CONNECTED) {

    ipShown = true;

    Serial.print("ESP32 IP address: ");

    Serial.println(WiFi.localIP());
  }
}


// ===== 8. UPLOAD TO THINGSPEAK =====

bool buildThingSpeakPayload(
  char* payload,
  size_t payloadSize,
  int raw,
  unsigned long now
) {

  double average = readingCount
    ? (double)readingSum / readingCount
    : 0.0;
  double rollingAverage = rollingCount
    ? (double)rollingSum / rollingCount
    : 0.0;

  int length = snprintf(
    payload,
    payloadSize,
    "field1=%d&field2=%d&field3=%d&field4=%d&field5=%.1f&field6=%lu&field7=%llu&field8=%.1f",
    raw,
    realAlarmOn ? 1 : 0,
    THRESHOLD,
    maxReading,
    average,
    (unsigned long)warningCount,
    (unsigned long long)warningDurationAt(now),
    rollingAverage
  );

  return length > 0 && (size_t)length < payloadSize;
}


void uploadToThingSpeak(int raw, unsigned long now) {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Upload skipped: no Wi-Fi");

    return;
  }

  HTTPClient http;

  char payload[192];
  if (!buildThingSpeakPayload(payload, sizeof(payload), raw, now)) {
    Serial.println("ThingSpeak HTTP payload build failed");
    return;
  }

  String url =
    String("http://api.thingspeak.com/update?api_key=") +
    TS_API_KEY +
    "&" + payload;

  http.begin(url);

  int code = http.GET();

  String body = http.getString();

  Serial.printf(
    "ThingSpeak HTTP %d, entry id: %s (0 = rejected)\n",
    code,
    body.c_str()
  );

  http.end();
}


void queueThingSpeakMqttPublish(int raw, unsigned long now) {

  if (mqttPublishQueue == nullptr) {
    Serial.println("MQTT publish skipped: queue unavailable");
    return;
  }

  MqttPublishMessage message;
  if (!buildThingSpeakPayload(
        message.payload,
        sizeof(message.payload),
        raw,
        now
      )) {
    Serial.println("MQTT payload build failed");
    return;
  }

  // Queue depth is one: when offline, retain only the newest scheduled sample.
  xQueueOverwrite(mqttPublishQueue, &message);
}


void mqttWorkerTask(void* parameter) {

  (void)parameter;
  unsigned long lastAttempt = 0;
  bool hadConnection = false;

  for (;;) {

    if (WiFi.status() != WL_CONNECTED) {

      if (mqttClient.connected()) {
        mqttClient.disconnect();
      }

      mqttConnected = false;
      if (hadConnection) {
        Serial.println("MQTT disconnected: Wi-Fi unavailable");
        hadConnection = false;
      }

      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!mqttClient.connected()) {

      mqttConnected = false;
      unsigned long now = millis();

      if ((unsigned long)(now - lastAttempt) >= MQTT_RETRY_MS) {

        lastAttempt = now;
        Serial.println("MQTT: connecting to ThingSpeak...");

        if (mqttClient.connect(
              SECRET_MQTT_CLIENT_ID,
              SECRET_MQTT_USERNAME,
              SECRET_MQTT_PASSWORD
            )) {
          mqttConnected = true;
          hadConnection = true;
          Serial.println("MQTT connected to ThingSpeak");
        } else {
          Serial.printf(
            "MQTT connection failed, state=%d; retrying in %lu ms\n",
            mqttClient.state(),
            MQTT_RETRY_MS
          );
        }
      }
    } else {

      mqttConnected = true;
      mqttClient.loop();

      if (!mqttClient.connected()) {
        mqttConnected = false;
        hadConnection = false;
        Serial.println("MQTT connection lost");
      } else if (mqttPublishQueue != nullptr) {

        MqttPublishMessage message;
        if (xQueueReceive(mqttPublishQueue, &message, 0) == pdTRUE) {

          if (mqttClient.publish(MQTT_TOPIC, message.payload)) {
            Serial.printf("MQTT publish sent to ThingSpeak: %s\n", message.payload);
          } else {
            Serial.printf(
              "MQTT publish failed, state=%d (no HTTP retry for this sample)\n",
              mqttClient.state()
            );
          }
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


// ===== 9. AUTOMATIC BASELINE CALIBRATION =====

void calibrate() {

  long sum = 0;

  for (int i = 0; i < 50; i++) {

    sum += readMQ2();

    delay(100);

    server.handleClient();
  }

  baseline = sum / 50;

  if (AUTO_BASELINE) {

    THRESHOLD = baseline + MARGIN;
  }

  if (THRESHOLD > 2900) {

    THRESHOLD = 2900;
  }

  Serial.printf(
    "Baseline=%d  Threshold=%d  Margin=%d\n",
    baseline,
    THRESHOLD,
    MARGIN
  );
}


// ===== 10. API: SENSOR DATA =====

void handleData() {

  long left = warming
    ? (long)(warmEnd - millis()) / 1000
    : 0;

  if (left < 0) {

    left = 0;
  }

  unsigned long now = millis();
  String maxJson = readingCount ? String(maxReading) : String("null");
  String averageJson = readingCount
    ? String((double)readingSum / readingCount, 1)
    : String("null");
  String rollingJson = rollingCount
    ? String((double)rollingSum / rollingCount, 1)
    : String("null");

  String j =
    "{\"raw\":" + String(lastRaw) +
    ",\"threshold\":" + String(THRESHOLD) +
    ",\"alert\":" + (alarmOn ? "true" : "false") +
    ",\"warming\":" + (warming ? "true" : "false") +
    ",\"left\":" + String(left) +
    ",\"sim\":" + (simulate ? "true" : "false") +
    ",\"maxReading\":" + maxJson +
    ",\"averageReading\":" + averageJson +
    ",\"warningCount\":" + String(warningCount) +
    ",\"warningDuration\":" + uint64ToString(warningDurationAt(now)) +
    ",\"rollingAverage\":" + rollingJson +
    "}";

  server.sendHeader("Cache-Control", "no-store");

  server.sendHeader("Access-Control-Allow-Origin", "*");

  server.send(200, "application/json", j);
}


// ===== 11. API: SIMULATED ALARM =====

void handleSim() {

  if (server.hasArg("on")) {

    simulate = (server.arg("on") == "1");
  }

  Serial.println(
    simulate
      ? ">> TEST MODE ON (web)"
      : ">> TEST MODE OFF (web)"
  );

  server.sendHeader("Access-Control-Allow-Origin", "*");

  server.send(
    200,
    "text/plain",
    simulate ? "on" : "off"
  );
}


// ===== 12. SETUP =====

void setup() {

  Serial.begin(115200);

  analogReadResolution(12);

  analogSetPinAttenuation(PIN_MQ2, ADC_11db);

  // Start BLE before Wi-Fi connection attempts so the phone can discover the
  // monitor even if the access point is unavailable.
  initializeBle();

  connectWiFi(15000);

  showIP();

  // API endpoints

  server.on("/data", HTTP_GET, handleData);

  server.on("/sim", HTTP_GET, handleSim);

  // CORS preflight support

  server.on("/sim", HTTP_OPTIONS, []() {

    server.sendHeader("Access-Control-Allow-Origin", "*");

    server.sendHeader(
      "Access-Control-Allow-Methods",
      "GET, OPTIONS"
    );

    server.sendHeader(
      "Access-Control-Allow-Headers",
      "Content-Type"
    );

    server.send(204);
  });

  server.begin();

  if (ENABLE_MQTT_PUBLISHING) {

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setKeepAlive(30);
    mqttClient.setSocketTimeout(2);

    mqttPublishQueue = xQueueCreate(1, sizeof(MqttPublishMessage));
    if (mqttPublishQueue == nullptr) {
      Serial.println("MQTT disabled at runtime: queue allocation failed");
    } else {
      BaseType_t taskCreated = xTaskCreatePinnedToCore(
        mqttWorkerTask,
        "ThingSpeakMQTT",
        4096,
        nullptr,
        1,
        nullptr,
        0
      );

      if (taskCreated != pdPASS) {
        vQueueDelete(mqttPublishQueue);
        mqttPublishQueue = nullptr;
        Serial.println("MQTT disabled at runtime: worker task creation failed");
      } else {
        Serial.println("MQTT publisher task started on core 0");
      }
    }
  } else {
    Serial.println("MQTT publishing disabled; HTTP publishing remains selected");
  }

  if (!ENABLE_HTTP_PUBLISHING && !ENABLE_MQTT_PUBLISHING) {
    Serial.println("WARNING: both ThingSpeak publishing methods are disabled");
  }

  warmEnd = millis() + WARMUP_MS;

  Serial.println("Web server started.");

  Serial.println(
    "Warming up MQ-2. Keep the sensor in clean air..."
  );

  Serial.println(
    "Type 't' in Serial Monitor or use the web button to test the alarm."
  );
}


// ===== 13. MAIN LOOP =====

void loop() {

  server.handleClient();

  showIP();

  // Serial Monitor simulation control

  while (Serial.available()) {

    char c = Serial.read();

    if (c == 't' || c == 'T') {

      simulate = !simulate;

      Serial.println(
        simulate
          ? ">> TEST MODE ON"
          : ">> TEST MODE OFF"
      );
    }
  }

  unsigned long now = millis();


  // ===== SENSOR READING =====

  if (now - lastRead >= READ_MS) {

    lastRead = now;

    // Test mode exercises the alarm without replacing the live sensor reading.
    lastRaw = readMQ2();
    bool validRealReading =
      !warming &&
      !simulate &&
      lastRaw >= 0 &&
      lastRaw <= 4095;


    // Finish warm-up and calibrate

    if (warming && now >= warmEnd) {

      calibrate();

      warming = false;
    }


    // ===== ALERT DETECTION =====

    bool wasAlarm = alarmOn;

    if (validRealReading) {
      addRealReading(lastRaw, now);
    }

    // Keep simulated alarm state separate from the real sensor alarm state.
    // This preserves the live raw reading and keeps simulated transitions out
    // of session analytics and ThingSpeak's real alert field.
    alarmOn = simulate || realAlarmOn;


    // ===== ALERT STATUS MESSAGE =====

    if (alarmOn != wasAlarm) {

      Serial.println(
        alarmOn
          ? "*** ALERT: threshold exceeded ***"
          : "--- Alert cleared ---"
      );
    }


    // ===== SERIAL OUTPUT =====

    Serial.printf(
      "raw=%d threshold=%d state=%s%s%s\n",

      lastRaw,

      THRESHOLD,

      alarmOn ? "WARNING" : "NORMAL",

      warming ? " (warming up)" : "",

      simulate ? " (SIMULATED)" : ""
    );
  }

  // Accumulate an ongoing real warning even when no new sensor sample is due.
  accumulateWarningDuration(now);

  // Send only changed values to the BLE worker; the worker owns GATT writes
  // and notifications so the sensor sampling loop remains responsive.
  queueBleSnapshotIfChanged(now);


  // ===== THINGSPEAK UPLOAD =====

  if (!warming && now - lastUpload >= UPLOAD_MS) {

    lastUpload = now;

    if (WiFi.status() != WL_CONNECTED) {

      WiFi.reconnect();
    }

    if (!simulate && readingCount > 0) {

      if (
        ENABLE_MQTT_PUBLISHING &&
        mqttConnected &&
        mqttPublishQueue != nullptr
      ) {
        queueThingSpeakMqttPublish(lastRaw, now);

      } else if (ENABLE_HTTP_PUBLISHING) {

        // Discard stale offline MQTT data before HTTP fallback so the same
        // scheduled sample cannot later be sent through both transports.
        if (mqttPublishQueue != nullptr) {
          xQueueReset(mqttPublishQueue);
        }

        uploadToThingSpeak(lastRaw, now);

      } else {
        Serial.println("ThingSpeak upload skipped: MQTT is not connected");
      }
    } else if (simulate) {
      Serial.println("ThingSpeak upload skipped: test mode is active");
    }
  }
}
