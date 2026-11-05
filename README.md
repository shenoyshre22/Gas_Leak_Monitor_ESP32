# IoT-Based Real-Time Gas and Smoke Leakage Detection and Monitoring System

**Group 37**

## Team Members

| Name | SRN |
|---|---|
| Shreya Shenoy | PES2UG24CS487 |
| Manapriya Kudurebyalya | PES2UG24CS677 |
| Tatam. Mahalakshmi | PES2UG24CS558 |
| Sneha Anand | PES2UG24CS506 |

## Project Overview

An IoT-based gas and smoke monitoring system that uses an MQ-2 sensor and ESP32 to detect potentially hazardous conditions. Sensor readings are processed using threshold-based detection and uploaded to ThingSpeak for cloud monitoring.

A web-based dashboard provides real-time sensor readings, system status, LED indicator simulations and an audible browser-based alarm.

## Technologies Used

- **Hardware:** ESP32 DevKit V1, MQ-2 sensor, resistors
- **Programming:** Arduino C++, HTML, CSS, JavaScript
- **Cloud Platform:** ThingSpeak
- **Tools:** Arduino IDE, Visual Studio Code

## Working

The system follows five IoT stages:

1. **Sense:** MQ-2 detects gas and smoke levels.
2. **Ingest:** ESP32 collects analog sensor readings.
3. **Process:** Readings are compared against a calibrated threshold to identify warning conditions.
4. **Store:** Sensor readings, alert status and threshold are uploaded to ThingSpeak.
5. **Analyze:** ThingSpeak and the web dashboard visualize sensor data and system status.

## Dashboard

The web dashboard displays live sensor readings, configured thresholds, ESP32 connectivity and system status. It also includes simulated green/red LED indicators and a browser-based audible alarm.

### Normal Operating Condition

The following screenshot shows the system operating normally, with sensor readings below the configured threshold, the green indicator active and the warning alarm inactive.

<img width="1840" height="868" alt="image" src="https://github.com/user-attachments/assets/1adfe301-34b2-4541-acab-11279bdbe7ed" />


### Warning Condition

The following screenshot shows the system detecting elevated gas/smoke readings. The warning status, red indicator and simulated audible alarm are activated.

<img width="1837" height="849" alt="image" src="https://github.com/user-attachments/assets/a85914b1-8155-43ee-aa9f-e8ef20769220" />

### ThingSpeak Cloud Monitoring

The following screenshot showcases the ThingSpeak dashboard, displaying real-time MQ-2 sensor readings, alert status and dynamically configured detection thresholds.

<img width="1356" height="807" alt="image" src="https://github.com/user-attachments/assets/5228987c-0c50-4080-ae3d-7a8191d042ca" />



## Setup and Execution

1. Copy `secrets.example.h` to `secrets.h` and enter the local Wi-Fi and ThingSpeak credentials. `secrets.h` is ignored by Git.
2. In Arduino IDE Library Manager, install **PubSubClient** by Nick O'Leary.
3. Open `GasLeakMonitor.ino` in Arduino IDE.
4. Select the ESP32 board and COM port, then upload the firmware.
5. Allow the MQ-2 sensor to warm up and complete baseline calibration.
6. Open `index.html` using VS Code Live Server.
7. Connect the dashboard to the ESP32 using its IP address.

## ThingSpeak MQTT Publishing

The firmware supports the ThingSpeak MQTT broker at `mqtt3.thingspeak.com:1883` and publishes to channel `3566022` using the topic `channels/3566022/publish`. It sends the eight channel fields in the same order as the HTTP upload: raw reading, real alert, threshold, maximum, session average, warning count, warning duration in seconds, and 10-reading average.

Publishing defaults in `GasLeakMonitor.ino` are deliberately safe:

```cpp
const bool ENABLE_MQTT_PUBLISHING = false;
const bool ENABLE_HTTP_PUBLISHING = true;
```

The existing HTTP path therefore remains active until MQTT is selected. To test MQTT while retaining the HTTP fallback, set `ENABLE_MQTT_PUBLISHING` to `true` and leave `ENABLE_HTTP_PUBLISHING` as `true`. At each 20-second update, the firmware uses MQTT if connected; otherwise it uses HTTP. It does not send the same scheduled sample over both transports. If an MQTT publish attempt fails after connection, that sample is logged and is not retried through HTTP to avoid a possible duplicate entry.

For MQTT-only operation, first confirm repeated `MQTT connected to ThingSpeak` and `MQTT publish sent to ThingSpeak` messages in Serial Monitor and verify new entries in channel 3566022. Then set both `ENABLE_MQTT_PUBLISHING` to `true` and `ENABLE_HTTP_PUBLISHING` to `false`, upload again, and monitor the MQTT status messages. If MQTT is unavailable in this mode, cloud updates pause while local sensor monitoring and the dashboard continue.

MQTT client ID, username and password belong to the ThingSpeak MQTT device credentials. Enter them only in the local, Git-ignored `secrets.h`; do not place them in dashboard files or commit them. Enable channel Fields 1 through 8 in ThingSpeak to view all values.

## Repository Structure

```text
GasLeakMonitor/
├── GasLeakMonitor.ino
├── secrets.h
├── secrets.example.h
├── .gitignore
├── index.html
├── style.css
├── script.js
└── screenshots/
    ├── normal-state.png
    └── warning-state.png
```

## Note

The LED indicators and audible alarm are simulated through the browser. Sensor values represent raw ADC counts, not calibrated gas concentrations in ppm. This project is an academic prototype and is not a certified safety device.
