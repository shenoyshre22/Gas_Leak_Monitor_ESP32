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

1. Configure Wi-Fi credentials and ThingSpeak API key in the local `secrets.h` file.
2. Open `GasLeakMonitor.ino` in Arduino IDE.
3. Select the ESP32 board and COM port, then upload the firmware.
4. Allow the MQ-2 sensor to warm up and complete baseline calibration.
5. Open `index.html` using VS Code Live Server.
6. Connect the dashboard to the ESP32 using its IP address.

## Repository Structure

```text
GasLeakMonitor/
├── GasLeakMonitor.ino
├── secrets.h
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
