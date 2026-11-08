
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
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
const int CALIBRATION_SAMPLE_COUNT = 50;
// Prototype parameter: validate this MAD limit with controlled sensor data.
const int CALIBRATION_MAD_LIMIT = 100;

// Safe initial mode: existing HTTP upload stays enabled until MQTT is verified.
// To switch to MQTT-only later, set MQTT true and HTTP false.
const bool ENABLE_MQTT_PUBLISHING = true;
const bool ENABLE_HTTP_PUBLISHING = true;

const char* MQTT_BROKER = "mqtt3.thingspeak.com";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC = "channels/3566022/publish";
const unsigned long MQTT_RETRY_MS = 5000UL;

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
  int raw;
  bool realAlert;
  int threshold;
};

QueueHandle_t mqttPublishQueue = nullptr;
volatile bool mqttConnected = false;

bool mqttCredentialConfigured(const char* value) {

  return value != nullptr &&
         value[0] != '\0' &&
         strncmp(value, "YOUR_", 5) != 0;
}

const char* mqttStateExplanation(int state) {

  switch (state) {
    case -4:
      return "Connection timeout";
    case -3:
      return "Connection lost";
    case -2:
      return "Failed to connect to broker";
    case -1:
      return "Disconnected";
    case 0:
      return "Connected";
    case 1:
      return "Incorrect MQTT protocol";
    case 2:
      return "Incorrect client ID";
    case 3:
      return "Broker unavailable";
    case 4:
      return "Bad username or password";
    case 5:
      return "Unauthorized";
    default:
      return "Unknown PubSubClient state";
  }
}

// ===== 4. VARIABLES =====

bool simulate = false;
bool warming = true;
bool ipShown = false;
bool realAlarmOn = false;

int lastRaw = 0;
int baseline = 0;
int calibrationSpread = 0;
bool calibrationValid = false;
const char* calibrationStatus = "Not calibrated";
uint8_t highReadingCount = 0;

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


bool displayAlarmOn() {

  return simulate || realAlarmOn;
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

  if (!realAlarmOn) {
    if (reading >= THRESHOLD) {
      if (highReadingCount < 2) {
        highReadingCount++;
      }
      if (highReadingCount >= 2) {
        nextRealAlarm = true;
      }
    } else {
      highReadingCount = 0;
    }
  } else if (reading < (THRESHOLD - HYSTERESIS)) {
    nextRealAlarm = false;
    highReadingCount = 0;
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
  unsigned long now,
  bool realAlert
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
    realAlert ? 1 : 0,
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
  if (!buildThingSpeakPayload(
        payload,
        sizeof(payload),
        raw,
        now,
        realAlarmOn
      )) {
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
  message.raw = raw;
  message.realAlert = realAlarmOn;
  message.threshold = THRESHOLD;
  if (!buildThingSpeakPayload(
        message.payload,
        sizeof(message.payload),
        raw,
        now,
        message.realAlert
      )) {
    Serial.println("MQTT payload build failed");
    return;
  }

  Serial.printf(
    "MQTT queued: raw=%d threshold=%d realAlert=%d field2=%d\n",
    message.raw,
    message.threshold,
    message.realAlert ? 1 : 0,
    message.realAlert ? 1 : 0
  );

  // Queue depth is one: when offline, retain only the newest scheduled sample.
  xQueueOverwrite(mqttPublishQueue, &message);
}


void mqttWorkerTask(void* parameter) {

  (void)parameter;
  unsigned long lastAttempt = 0;
  bool hadConnection = false;
  bool reportedWifiWait = false;

  Serial.printf(
    "MQTT worker task entered on core %d; Wi-Fi status=%d\n",
    xPortGetCoreID(),
    WiFi.status()
  );

  for (;;) {

    if (WiFi.status() != WL_CONNECTED) {

      if (mqttClient.connected()) {
        mqttClient.disconnect();
      }

      mqttConnected = false;
      if (!reportedWifiWait) {
        Serial.printf(
          "MQTT worker waiting for Wi-Fi; status=%d\n",
          WiFi.status()
        );
        reportedWifiWait = true;
      }
      if (hadConnection) {
        Serial.println("MQTT disconnected: Wi-Fi unavailable");
        hadConnection = false;
      }

      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    reportedWifiWait = false;

    if (!mqttClient.connected()) {

      mqttConnected = false;
      unsigned long now = millis();

      if ((unsigned long)(now - lastAttempt) >= MQTT_RETRY_MS) {

        lastAttempt = now;
        Serial.printf(
          "MQTT connection attempt: Wi-Fi status=%d, broker=%s, port=%u\n",
          WiFi.status(),
          MQTT_BROKER,
          MQTT_PORT
        );
        Serial.printf(
          "MQTT credentials configured: client ID=%s, username=%s, password=%s\n",
          mqttCredentialConfigured(SECRET_MQTT_CLIENT_ID) ? "yes" : "no",
          mqttCredentialConfigured(SECRET_MQTT_USERNAME) ? "yes" : "no",
          mqttCredentialConfigured(SECRET_MQTT_PASSWORD) ? "yes" : "no"
        );
        Serial.println("MQTT calling connect(client ID, username, password)...");

        if (mqttClient.connect(
              SECRET_MQTT_CLIENT_ID,
              SECRET_MQTT_USERNAME,
              SECRET_MQTT_PASSWORD
            )) {
          int state = mqttClient.state();
          mqttConnected = true;
          hadConnection = true;
          Serial.printf(
            "MQTT connected to ThingSpeak; state=%d (%s)\n",
            state,
            mqttStateExplanation(state)
          );
        } else {
          int state = mqttClient.state();
          Serial.printf(
            "MQTT connection failed, state=%d (%s); retrying in %lu ms\n",
            state,
            mqttStateExplanation(state),
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

          Serial.printf(
            "MQTT publishing: raw=%d threshold=%d realAlert=%d field2=%d\n",
            message.raw,
            message.threshold,
            message.realAlert ? 1 : 0,
            message.realAlert ? 1 : 0
          );

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

  int samples[CALIBRATION_SAMPLE_COUNT];

  for (int i = 0; i < CALIBRATION_SAMPLE_COUNT; i++) {

    samples[i] = readMQ2();

    delay(100);

    server.handleClient();
  }

  for (int i = 1; i < CALIBRATION_SAMPLE_COUNT; i++) {

    int value = samples[i];
    int j = i - 1;

    while (j >= 0 && samples[j] > value) {
      samples[j + 1] = samples[j];
      j--;
    }

    samples[j + 1] = value;
  }

  int middle = CALIBRATION_SAMPLE_COUNT / 2;
  baseline = (samples[middle - 1] + samples[middle]) / 2;

  for (int i = 0; i < CALIBRATION_SAMPLE_COUNT; i++) {
    samples[i] = abs(samples[i] - baseline);
  }

  for (int i = 1; i < CALIBRATION_SAMPLE_COUNT; i++) {

    int value = samples[i];
    int j = i - 1;

    while (j >= 0 && samples[j] > value) {
      samples[j + 1] = samples[j];
      j--;
    }

    samples[j + 1] = value;
  }

  calibrationSpread =
    (samples[middle - 1] + samples[middle]) / 2;

  Serial.printf(
    "Calibration samples=%d median=%d MAD=%d limit=%d\n",
    CALIBRATION_SAMPLE_COUNT,
    baseline,
    calibrationSpread,
    CALIBRATION_MAD_LIMIT
  );

  calibrationValid = calibrationSpread <= CALIBRATION_MAD_LIMIT;

  if (!calibrationValid) {
    // Fallback: retain the existing threshold instead of applying an
    // untrusted baseline-derived threshold.
    calibrationStatus = "Rejected: unstable baseline (MAD above limit)";
    Serial.printf(
      "Calibration rejected: %s; threshold unchanged at %d\n",
      calibrationStatus,
      THRESHOLD
    );
    return;
  }

  calibrationStatus = "Accepted";

  if (AUTO_BASELINE) {

    THRESHOLD = baseline + MARGIN;
  }

  if (THRESHOLD > 2900) {
    THRESHOLD = 2900;
  }

  Serial.printf(
    "Calibration accepted: baseline=%d MAD=%d threshold=%d\n",
    baseline,
    calibrationSpread,
    THRESHOLD
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
    ",\"baseline\":" + String(baseline) +
    ",\"threshold\":" + String(THRESHOLD) +
    ",\"calibrationValid\":" + (calibrationValid ? "true" : "false") +
    ",\"calibrationSpread\":" + String(calibrationSpread) +
    ",\"calibrationStatus\":\"" + String(calibrationStatus) + "\"" +
    ",\"alert\":" + (displayAlarmOn() ? "true" : "false") +
    ",\"realAlert\":" + (realAlarmOn ? "true" : "false") +
    ",\"warming\":" + (warming ? "true" : "false") +
    ",\"left\":" + String(left) +
    ",\"sim\":" + (simulate ? "true" : "false") +
    ",\"simulate\":" + (simulate ? "true" : "false") +
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

    bool wasAlarm = displayAlarmOn();

    if (validRealReading) {
      addRealReading(lastRaw, now);
    }

    // Keep the combined display state derived from simulation and the real
    // sensor state; simulation never changes realAlarmOn or its analytics.
    // ===== ALERT STATUS MESSAGE =====

    if (displayAlarmOn() != wasAlarm) {

      Serial.println(
        displayAlarmOn()
          ? "*** ALERT: threshold exceeded ***"
          : "--- Alert cleared ---"
      );
    }


    // ===== SERIAL OUTPUT =====

    Serial.printf(
      "raw=%d threshold=%d state=%s%s%s\n",

      lastRaw,

      THRESHOLD,

      displayAlarmOn() ? "WARNING" : "NORMAL",

      warming ? " (warming up)" : "",

      simulate ? " (SIMULATED)" : ""
    );
  }

  // Accumulate an ongoing real warning even when no new sensor sample is due.
  accumulateWarningDuration(now);

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
