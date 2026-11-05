
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <stdint.h>
#include <stdio.h>
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

// ===== 2. SENSOR PIN =====

const int PIN_MQ2 = 34;

// ===== 3. WEB SERVER =====

WebServer server(80);

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

void uploadToThingSpeak(int raw, unsigned long now) {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Upload skipped: no Wi-Fi");

    return;
  }

  HTTPClient http;

  String url =
    String("http://api.thingspeak.com/update?api_key=") +
    TS_API_KEY +
    "&field1=" + raw +
    "&field2=" + (realAlarmOn ? 1 : 0) +
    "&field3=" + THRESHOLD +
    "&field4=" + (readingCount ? String(maxReading) : String("")) +
    "&field5=" + (readingCount ? String((double)readingSum / readingCount, 1) : String("")) +
    "&field6=" + warningCount +
    "&field7=" + uint64ToString(warningDurationAt(now)) +
    "&field8=" + (rollingCount ? String((double)rollingSum / rollingCount, 1) : String(""));

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


  // ===== THINGSPEAK UPLOAD =====

  if (!warming && now - lastUpload >= UPLOAD_MS) {

    lastUpload = now;

    if (WiFi.status() != WL_CONNECTED) {

      WiFi.reconnect();
    }

    if (!simulate && readingCount > 0) {
      uploadToThingSpeak(lastRaw, now);
    } else if (simulate) {
      Serial.println("ThingSpeak upload skipped: test mode is active");
    }
  }
}
