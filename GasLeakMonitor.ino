
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
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

int lastRaw = 0;
int baseline = 0;

unsigned long lastRead = 0;
unsigned long lastUpload = 0;
unsigned long warmEnd = 0;


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

void uploadToThingSpeak(int raw, bool alert) {

  if (WiFi.status() != WL_CONNECTED) {

    Serial.println("Upload skipped: no Wi-Fi");

    return;
  }

  HTTPClient http;

  String url =
    String("http://api.thingspeak.com/update?api_key=") +
    TS_API_KEY +
    "&field1=" + raw +
    "&field2=" + (alert ? 1 : 0) +
    "&field3=" + THRESHOLD;

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

  String j =
    "{\"raw\":" + String(lastRaw) +
    ",\"threshold\":" + String(THRESHOLD) +
    ",\"alert\":" + (alarmOn ? "true" : "false") +
    ",\"warming\":" + (warming ? "true" : "false") +
    ",\"left\":" + String(left) +
    ",\"sim\":" + (simulate ? "true" : "false") +
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

    lastRaw = simulate
      ? THRESHOLD + 500
      : readMQ2();


    // Finish warm-up and calibrate

    if (warming && now >= warmEnd) {

      calibrate();

      warming = false;
    }


    // ===== ALERT DETECTION =====

    bool wasAlarm = alarmOn;

    if (warming && !simulate) {

      alarmOn = false;

    } else {

      // Activate immediately when threshold is crossed

      if (!alarmOn && lastRaw >= THRESHOLD) {

        alarmOn = true;

      }

      // Clear only after falling below threshold
      // minus hysteresis

      else if (
        alarmOn &&
        lastRaw < (THRESHOLD - HYSTERESIS)
      ) {

        alarmOn = false;
      }
    }


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


  // ===== THINGSPEAK UPLOAD =====

  if (!warming && now - lastUpload >= UPLOAD_MS) {

    lastUpload = now;

    if (WiFi.status() != WL_CONNECTED) {

      WiFi.reconnect();
    }

    uploadToThingSpeak(lastRaw, alarmOn);
  }
}
