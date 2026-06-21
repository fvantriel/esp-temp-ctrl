# ESP8266 Temperature Controller

A WiFi-connected temperature controller running on an ESP8266. It reads a DS18B20 sensor, runs a PID loop, and switches a **Shelly Plug S** on/off to control a heater. A simple web interface lets you monitor the temperature, adjust the setpoint, and toggle manual/auto mode — no app required.

---

## Hardware

| Part | Notes |
|------|-------|
| ESP8266 (e.g. ESP-01, NodeMCU) | Any ESP8266 board works |
| DS18B20 temperature sensor | Waterproof probe recommended |
| 4.7 kΩ resistor | Pull-up between DATA and 3.3 V |
| Shelly Plug S | Controlled via local HTTP API (Gen1) |

### Wiring

```
DS18B20 pin 1 (GND)  → ESP8266 GND
DS18B20 pin 2 (DATA) → ESP8266 GPIO2  +  4.7 kΩ pull-up to 3.3 V
DS18B20 pin 3 (VDD)  → ESP8266 3.3 V
```

The Shelly Plug S only needs to be on the same WiFi network — no physical wiring to the ESP.

---

## Software dependencies

Install these libraries via the Arduino Library Manager:

- **ESP8266WiFi** — part of the ESP8266 Arduino core
- **OneWire** by Paul Stoffregen
- **DallasTemperature** by Miles Burton
- **PID_v2** by Brett Beauregard / Mike Karlesky

Board support: add `http://arduino.esp8266.com/stable/package_esp8266com_index.json` to *Additional Board Manager URLs* and install **esp8266 by ESP8266 Community**.

---

## Setup

1. **Clone the repo**
   ```bash
   git clone https://github.com/YOUR_USER/esp-temp-ctrl.git
   cd esp-temp-ctrl/esp_web_temp_ctrl
   ```

2. **Create your secrets file**
   ```bash
   cp secrets.h.example secrets.h
   ```
   Edit `secrets.h` and fill in your WiFi credentials and Shelly IP address. This file is listed in `.gitignore` and will never be committed.

3. **Find your Shelly's IP**
   Open the Shelly app or check your router's DHCP table. Assign a static IP to the Shelly so it doesn't change.

4. **Flash the ESP8266**
   Open `esp_web_temp_ctrl.ino` in the Arduino IDE, select your board and port, then click Upload.

5. **Open the web UI**
   After flashing, open the Serial Monitor at 115200 baud to see the assigned IP address, then navigate to it in a browser.

---

## Web interface

The page auto-refreshes every 5 seconds. No JavaScript or app required.

- **Current temperature** — live reading from the DS18B20 (shows `--` until the first valid measurement)
- **Setpoint** — target temperature (5–95 °C), persisted in EEPROM across reboots
- **Mode** — *Auto* (PID controls the plug) or *Manual* (you control the plug directly)
- **Plug status** — shows whether the Shelly is currently on or off

---

## How it works

- The DS18B20 is sampled in **async mode** (non-blocking) with 9-bit resolution (~94 ms conversion time). Five samples are averaged per control cycle.
- Readings are validated: the DS18B20 power-on default (85 °C), out-of-range values, and spikes larger than 10 °C from the last valid reading are all discarded.
- The **PID controller** runs in Direct mode with output clamped to 0–1 (bang-bang). Kp=1.5, Ki=2.0, Kd=2.5 — tune these in the `.ino` if needed.
- The **Shelly Plug S** is switched via a simple HTTP GET (`/relay/0?turn=on|off`). Switching only happens when the desired state changes, to avoid hammering the relay.
- The setpoint survives reboots via **EEPROM** (with a magic byte guard and bounds check on load).

---

## Configuration constants

All tunable values are at the top of `esp_web_temp_ctrl.ino`:

| Constant | Default | Description |
|----------|---------|-------------|
| `ONE_WIRE_BUS` | `2` | GPIO pin for DS18B20 data line |
| `TEMP_VALID_MIN` | `-40.0` | Reject readings below this (°C) |
| `TEMP_VALID_MAX` | `100.0` | Reject readings above this (°C) |
| `TEMP_SPIKE_MAX_DELTA` | `10.0` | Max allowed jump from last valid reading (°C) |
| `NUM_SAMPLES` | `5` | Samples averaged per PID cycle |
| `DS18B20_WAIT_MS` | `160` | Conversion wait time (ms) |
| `SHELLY_TIMEOUT_MS` | `1000` | TCP connect timeout for Shelly (ms) |
| `SETPOINT_MIN` | `5.0` | Minimum allowed setpoint (°C) |
| `SETPOINT_MAX` | `95.0` | Maximum allowed setpoint (°C) |
| `Kp / Ki / Kd` | `1.5 / 2.0 / 2.5` | PID gains |

---

## Testing without hardware

A Python test server is included that simulates the ESP web UI with a drifting temperature:

```bash
python3 esp_web_temp_ctrl/test_web_server.py
# Open http://127.0.0.1:8080
```
