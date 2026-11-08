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

An IoT-based gas and smoke leakage detection and monitoring system built using an MQ-2 gas sensor and an ESP32 microcontroller. The system continuously monitors changes in the surrounding air, processes sensor readings using a calibrated threshold-based detection mechanism, and transmits monitoring data to the ThingSpeak cloud platform.

A browser-based dashboard provides local real-time monitoring, visual system status, sensor analytics, and simulated warning indicators.

The project demonstrates an end-to-end IoT pipeline covering sensing, data ingestion, processing, cloud storage, analysis, and visualization.

## Technologies Used

- **Hardware:** ESP32 DevKit V1, MQ-2 gas and smoke sensor, two 10kΩ resistors.
- **Programming:** Arduino C++, HTML, CSS, JavaScript.
- **Cloud Platform:** ThingSpeak.
- **Communication Protocols:** Wi-Fi, MQTT, HTTP.
- **Libraries:** PubSubClient.
- **Development Tools:** Arduino IDE, Visual Studio Code.

## System Architecture

The system follows five stages of an IoT pipeline:

1. **Sense:** The MQ-2 sensor detects changes in gas and smoke concentration in the surrounding environment.
2. **Ingest:** The ESP32 reads the sensor's analog output through its ADC pin.
3. **Process:** Sensor readings are processed using baseline calibration, threshold comparison, and hysteresis-based alert handling.
4. **Store:** Sensor readings and monitoring statistics are transmitted to ThingSpeak using the configured cloud communication mechanism.
5. **Analyze:** ThingSpeak visualizations and the local web dashboard display sensor trends, warning events, and monitoring statistics.

## Hardware Implementation

### Components

| Component | Purpose |
|---|---|
| ESP32 DevKit V1 | Main microcontroller and Wi-Fi connectivity |
| MQ-2 Sensor Module | Gas and smoke detection |
| 10kΩ Resistors (2) | Voltage divider for ESP32 ADC protection |

### Pin Connections

| MQ-2 / Circuit Connection | ESP32 Connection |
|---|---|
| VCC | VIN / 5V |
| GND | GND |
| Analog Output (AO) | GPIO34 through voltage divider |
| Digital Output (DO) | Not used |

The MQ-2 analog output is connected to GPIO34 through a two-resistor voltage divider to reduce the sensor output voltage to a level suitable for the ESP32 ADC.

**Important:** The MQ-2 analog output must not be connected directly to the ESP32 ADC when it can exceed the ADC's permitted input voltage.

## Sensor Processing and Alert Mechanism

The ESP32 periodically collects analog readings from the MQ-2 sensor.

The firmware includes:

- **Baseline Calibration:** Establishes a reference sensor reading during startup.
- **Median and MAD-Based Calibration:** Uses robust statistical measures to estimate the baseline and identify unstable calibration samples.
- **Threshold-Based Detection:** Compares incoming readings against a configured detection threshold.
- **Hysteresis:** Uses a separate clearing threshold to reduce repeated alert transitions when readings fluctuate near the warning threshold.
- **Sensor Statistics:** Maintains monitoring information such as maximum readings, average readings, rolling averages, warning counts, and warning duration.

The sensor readings are represented as raw ADC counts. They are not calibrated gas concentrations in parts per million (ppm).

## Web Dashboard

The browser-based dashboard provides a local interface for monitoring the ESP32 and visualizing sensor data.

Its features include:

- Live MQ-2 sensor readings.
- Configured detection threshold.
- ESP32 connectivity and system status.
- Normal and warning status indicators.
- Maximum and average sensor readings.
- Warning event statistics.
- Simulated green and red LED indicators.
- Browser-based audible warning simulation.

The dashboard communicates with the ESP32 through its local HTTP endpoints. The laptop running the dashboard and the ESP32 must be connected to the same Wi-Fi network.

### Normal Operating Condition

The following screenshot shows the system operating normally, with sensor readings below the configured threshold, the green indicator active, and the warning alarm inactive.

<img width="1840" height="868" alt="Normal operating condition" src="https://github.com/user-attachments/assets/1adfe301-34b2-4541-acab-11279bdbe7ed" />

### Warning Condition

The following screenshot shows the dashboard displaying an elevated gas/smoke reading and the corresponding warning state, with the red indicator and simulated audible alarm activated.

<img width="1837" height="849" alt="Warning condition" src="https://github.com/user-attachments/assets/a85914b1-8155-43ee-aa9f-e8ef20769220" />

**Note:** The LED indicators and audible alarm are browser simulations. They are not physical hardware actuators.

## ThingSpeak Cloud Monitoring

ThingSpeak is used to store and visualize sensor readings and monitoring statistics.

**ThingSpeak Channel ID:** `3566022`

The system uses eight channel fields:

| Field | Description |
|---|---|
| Field 1 | Raw MQ-2 ADC reading |
| Field 2 | Real sensor alert status (0 or 1) |
| Field 3 | Configured detection threshold |
| Field 4 | Maximum sensor reading |
| Field 5 | Session average reading |
| Field 6 | Warning event count |
| Field 7 | Warning duration in seconds |
| Field 8 | Rolling average reading (10-reading average) |

The cloud dashboard provides historical data visualization and allows sensor trends and warning statistics to be monitored over time.

### ThingSpeak Dashboard Screenshots

<img width="668" height="212" alt="ThingSpeak monitoring overview" src="https://github.com/user-attachments/assets/e185ab51-6242-4bc9-a048-21750749b5c6" />
<img width="662" height="207" alt="image" src="https://github.com/user-attachments/assets/112ad106-0ae7-4a19-99de-f44e36c1df9d" />
<img width="686" height="218" alt="image" src="https://github.com/user-attachments/assets/8eacf2d3-33b4-4de7-bbfc-fe8276438ff5" />
<img width="645" height="230" alt="ThingSpeak monitoring statistics" src="https://github.com/user-attachments/assets/f2e74146-ee7c-4c7a-a575-b21c6e7ca0b9" />

## ThingSpeak MQTT Publishing

The firmware supports MQTT communication through the ThingSpeak MQTT broker:

- **Broker:** `mqtt3.thingspeak.com`
- **Port:** `1883`
- **Publish Topic:** `channels/3566022/publish`

MQTT authentication uses the credentials associated with the ThingSpeak MQTT device.

The firmware also retains HTTP publishing functionality. The enabled publishing modes and fallback behaviour are controlled by the configuration in `GasLeakMonitor.ino`.

To configure MQTT:

1. Create an MQTT device in ThingSpeak.
2. Authorize the device to publish to channel `3566022`.
3. Enable the required channel fields.
4. Copy the MQTT client ID, username, and password into the local `secrets.h` file.
5. Configure the MQTT and HTTP publishing flags in `GasLeakMonitor.ino`.
6. Upload the firmware and monitor the Serial Monitor for MQTT connection and publishing messages.
7. Verify that new entries appear in the ThingSpeak channel.

**Credential Security:** MQTT credentials and Wi-Fi passwords must remain in the local, Git-ignored `secrets.h` file. Never commit them to the repository or expose them in frontend files.

## Setup and Execution

### Prerequisites

- Arduino IDE with ESP32 board support.
- ESP32 DevKit V1.
- MQ-2 sensor module.
- Two 10kΩ resistors.
- Visual Studio Code with Live Server extension.
- ThingSpeak account and configured channel.

### Step 1: Configure Credentials

1. Clone or download this repository.
2. Copy `secrets.example.h` and rename the copy to `secrets.h`.
3. Enter the required Wi-Fi and ThingSpeak credentials in `secrets.h`.
4. Ensure that `secrets.h` remains excluded from Git using `.gitignore`.

### Step 2: Install Dependencies

Install the **PubSubClient** library by Nick O'Leary through the Arduino IDE Library Manager.

Ensure that ESP32 board support is installed and the appropriate ESP32 board is selected.

### Step 3: Upload Firmware

1. Open `GasLeakMonitor.ino` in Arduino IDE.
2. Select the ESP32 board and corresponding COM port.
3. Verify and compile the firmware.
4. Upload the firmware to the ESP32.
5. Open Serial Monitor to inspect startup, calibration, Wi-Fi, and cloud communication status.

### Step 4: Start the Web Dashboard

1. Open the project folder in Visual Studio Code.
2. Open `index.html`.
3. Start the application using the Live Server extension.
4. Enter the ESP32's local IP address in the dashboard connection settings.
5. Ensure that the laptop and ESP32 are connected to the same Wi-Fi network.

### Step 5: Monitor the System

Once connected:

- View live sensor readings on the local dashboard.
- Monitor system status and warning indicators.
- Inspect historical sensor readings and statistics on ThingSpeak.
- Check Serial Monitor for ESP32 connectivity and cloud publishing diagnostics.

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

```

## Limitations and Safety Considerations

- The MQ-2 sensor requires appropriate preheating and conditioning for stable operation. The firmware's startup warm-up and baseline calibration do not replace the manufacturer's recommended sensor conditioning period.
- Sensor readings are raw ADC values and must not be interpreted as calibrated gas concentrations in ppm.
- The LED indicators and audible alarm are simulated in the browser rather than implemented using physical actuators.
- ThingSpeak cloud updates are subject to the configured publishing interval and platform limitations.
- Sensor response depends on environmental conditions, sensor conditioning, gas concentration, and exposure.
- This project is an academic prototype intended for educational demonstration and is not a certified gas detection or life-safety system.

## Conclusion

The project demonstrates an IoT-based gas and smoke monitoring solution integrating an MQ-2 sensor, ESP32 microcontroller, Wi-Fi communication, cloud data storage, and a browser-based monitoring dashboard.

By combining sensor calibration, threshold-based processing, monitoring statistics, and cloud visualization, the system illustrates how IoT technologies can be used to develop a connected environmental monitoring prototype.
