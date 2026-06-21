#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <PID_v2.h>
#include <EEPROM.h>

#include "secrets.h"  // WiFi, Shelly – copy secrets.h.example to secrets.h and fill in

// DS18B20 Sensor (GPIO2)
#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// Sensor: valid range
// DS18B20 returns -127 on read error and 85.0 as power-on default — both must be rejected.
// TEMP_VALID_MAX is set to 100°C (above any realistic setpoint) rather than the hardware
// max of 125°C, to reject implausible high readings.
#define TEMP_VALID_MIN      -40.0f
#define TEMP_VALID_MAX      100.0f
#define TEMP_SPIKE_MAX_DELTA 10.0f  // Reject reading if it jumps > this far from last valid
#define NUM_SAMPLES          5
#define DS18B20_WAIT_MS    160      // 9-bit conversion ~94ms, with margin
#define SHELLY_TIMEOUT_MS 1000      // TCP connect timeout – keeps loop responsive

// NAN = no valid reading received yet; suppresses PID until sensor is confirmed working
float lastValidInput = NAN;
bool hasValidInput = false;

// True non-blocking temperature state machine (no delay() calls)
enum TempPhase { PHASE_REQUEST, PHASE_READ };
TempPhase tempPhase = PHASE_REQUEST;
unsigned long nextTempTime = 0;
float sampleSum = 0.0f;
int sampleCount = 0;
int sampleAttempts = 0;

// EEPROM: Setpoint persistence
#define EEPROM_SIZE          64
#define EEPROM_MAGIC         0xAB
#define EEPROM_MAGIC_ADDR    0
#define EEPROM_SETPOINT_ADDR 1   // float = 4 bytes

// Setpoint bounds — enforced both in the HTML form and server-side
#define SETPOINT_MIN  5.0f
#define SETPOINT_MAX  95.0f

// PID
float Input = 0.0f, Output = 0.0f, Setpoint = 65.0f;  // Default, overwritten from EEPROM
double Kp = 1.5, Ki = 2.0, Kd = 2.5;
PID_v2 myPID(Kp, Ki, Kd, PID::Direct);

float loadSetpointFromEEPROM() {
  uint8_t magic = EEPROM.read(EEPROM_MAGIC_ADDR);
  if (magic != EEPROM_MAGIC) {
    Serial.println("EEPROM not initialized, using default setpoint");
    return 65.0f;
  }
  float sp;
  EEPROM.get(EEPROM_SETPOINT_ADDR, sp);
  // Guard against corrupted float (NaN, Inf, or out-of-range)
  if (isnan(sp) || isinf(sp) || sp < SETPOINT_MIN || sp > SETPOINT_MAX) {
    Serial.println("EEPROM setpoint invalid, using default");
    return 65.0f;
  }
  Serial.print("Setpoint loaded from EEPROM: ");
  Serial.println(sp, 1);
  return sp;
}

void saveSetpointToEEPROM(float sp) {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  EEPROM.put(EEPROM_SETPOINT_ADDR, sp);
  if (EEPROM.commit()) {
    Serial.print("Setpoint saved to EEPROM: ");
    Serial.println(sp, 1);
  } else {
    Serial.println("EEPROM write failed");
  }
}

// Webserver
ESP8266WebServer server(80);
bool manualMode = false;
bool manualState = false;
bool forceControlUpdate = false;  // triggers immediate PID update when switching to auto
bool shellyCurrentState = false;

// Shelly switch — SHELLY_TIMEOUT_MS caps blocking time if Shelly is unreachable
void shellySwitch(bool state) {
  WiFiClient client;
  if (!client.connect(shelly_ip, 80, SHELLY_TIMEOUT_MS)) {
    Serial.println("Shelly connection failed");
    return;
  }
  String path = "/relay/0?turn=";
  path += state ? "on" : "off";
  String request = "GET " + path + " HTTP/1.1\r\nHost: " + String(shelly_ip) + "\r\nConnection: close\r\n\r\n";
  client.print(request);
  delay(10);
  client.stop();
}

bool shellyReachable() {
  WiFiClient client;
  if (!client.connect(shelly_ip, 80, SHELLY_TIMEOUT_MS)) return false;
  client.print("GET /relay/0 HTTP/1.1\r\nHost: " + String(shelly_ip) + "\r\nConnection: close\r\n\r\n");
  unsigned long t = millis();
  while (client.connected() && (millis() - t < SHELLY_TIMEOUT_MS)) {
    if (client.available()) { client.readString(); break; }
  }
  client.stop();
  return true;
}

bool shellyGetState() {
  WiFiClient client;
  if (!client.connect(shelly_ip, 80, SHELLY_TIMEOUT_MS)) return false;
  client.print("GET /relay/0 HTTP/1.1\r\nHost: " + String(shelly_ip) + "\r\nConnection: close\r\n\r\n");
  String response = "";
  unsigned long t = millis();
  while (client.connected() && (millis() - t < SHELLY_TIMEOUT_MS)) {
    while (client.available()) response += (char)client.read();
    if (response.indexOf("ison") >= 0) break;
    delay(10);
  }
  client.stop();
  return response.indexOf("\"ison\":true") >= 0;
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>";
  html += "<title>Temperatursteuerung</title>";
  html += "<style>";
  html += "body{font-family:sans-serif;max-width:360px;margin:20px auto;padding:0 16px;background:#f5f5f5;}";
  html += ".card{background:#fff;border-radius:12px;padding:20px;margin-bottom:16px;box-shadow:0 2px 8px rgba(0,0,0,.08);}";
  html += "h1{margin:0 0 16px;font-size:1.35rem;color:#333;}";
  html += ".temp{font-size:2rem;font-weight:700;color:#1565c0;margin:8px 0;}";
  html += ".temp span{font-size:1rem;font-weight:400;color:#666;}";
  html += ".status{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:20px;font-size:0.9rem;font-weight:600;}";
  html += ".status.on{background:#e8f5e9;color:#2e7d32;}";
  html += ".status.off{background:#f5f5f5;color:#616161;}";
  html += ".status-dot{width:8px;height:8px;border-radius:50%;}";
  html += ".status.on .status-dot{background:#4caf50;}";
  html += ".status.off .status-dot{background:#9e9e9e;}";
  html += ".msg{color:#2e7d32;font-size:0.9rem;margin:8px 0;}";
  html += ".warn{color:#e65100;font-size:0.9rem;margin:8px 0;}";
  html += "label{display:block;font-size:0.85rem;color:#666;margin-bottom:4px;}";
  html += "input[type='number']{width:100%;box-sizing:border-box;padding:10px;border:1px solid #ddd;border-radius:8px;font-size:1rem;}";
  html += "select{padding:10px;border:1px solid #ddd;border-radius:8px;font-size:1rem;min-width:120px;}";
  html += "button,.btn{padding:10px 16px;border:none;border-radius:8px;font-size:0.95rem;cursor:pointer;background:#1565c0;color:#fff;}";
  html += "button:hover,.btn:hover{background:#0d47a1;}";
  html += ".row{margin-bottom:12px;}";
  html += ".foot{font-size:0.75rem;color:#999;margin-top:12px;}";
  html += "</style></head><body>";

  html += "<div class='card'>";
  html += "<h1>Temperatursteuerung</h1>";

  if (!hasValidInput) {
    html += "<div class='temp'>-- <span>°C</span></div>";
    html += "<p class='warn'>Warte auf ersten gueltigen Messwert...</p>";
  } else {
    html += "<div class='temp'>" + String(Input, 1) + " <span>°C</span></div>";
  }
  html += "<p style='margin:0 0 12px;font-size:0.85rem;color:#666'>Soll: " + String(Setpoint, 1) + " °C</p>";

  if (server.hasArg("msg")) {
    String m = server.arg("msg");
    if (m == "setpoint_ok")  html += "<p class='msg'>Soll-Temperatur wurde uebernommen.</p>";
    else if (m == "mode_ok") html += "<p class='msg'>Modus wurde uebernommen.</p>";
    else if (m == "sp_err")  html += "<p class='warn'>Ungueltige Soll-Temperatur (5–95 °C).</p>";
  }

  html += "<form method='POST' action='/set'>";
  html += "<div class='row'><label>Soll-Temperatur</label>";
  html += "<input name='setpoint' value='" + String(Setpoint) + "' type='number' step='0.5' min='5' max='95'>";
  html += "</div><button type='submit'>Soll uebernehmen</button></form>";

  html += "<form method='POST' action='/set' style='margin-top:12px'>";
  html += "<div class='row'><label>Modus</label>";
  html += "<select name='mode'>";
  html += manualMode
          ? "<option value='auto'>Auto</option><option value='man' selected>Manuell</option>"
          : "<option value='auto' selected>Auto</option><option value='man'>Manuell</option>";
  html += "</select></div><button type='submit'>Modus uebernehmen</button></form>";

  if (manualMode) {
    html += "<form method='POST' action='/manual' style='margin-top:12px'>";
    html += "<button type='submit' class='btn'>" + String(manualState ? "Ausschalten" : "Einschalten") + "</button>";
    html += "</form>";
  }

  html += "<div style='margin-top:16px;padding-top:12px;border-top:1px solid #eee'>";
  html += "<span class='status " + String(shellyCurrentState ? "on" : "off") + "'>";
  html += "<span class='status-dot'></span>";
  html += String(shellyCurrentState ? "Steckdose Ein" : "Steckdose Aus");
  html += "</span>";
  html += "<p class='foot'>Shelly Plug S @ " + String(shelly_ip) + " &nbsp; PID: Kp=" + String(Kp) + " Ki=" + String(Ki) + " Kd=" + String(Kd) + "</p>";
  html += "</div></div></body></html>";

  server.send(200, "text/html", html);
}

void handleSet() {
  bool setpointChanged = false;
  bool modeChanged = false;
  bool setpointErr = false;

  if (server.hasArg("setpoint")) {
    String raw = server.arg("setpoint");
    if (raw.length() > 0) {
      float sp = raw.toFloat();
      if (sp >= SETPOINT_MIN && sp <= SETPOINT_MAX) {
        Setpoint = sp;
        setpointChanged = true;
        saveSetpointToEEPROM(Setpoint);
        myPID.Setpoint(Setpoint);
      } else {
        setpointErr = true;
      }
    }
  }
  if (server.hasArg("mode")) {
    bool wasManual = manualMode;
    manualMode = (server.arg("mode") == "man");
    // Switching from manual → auto: trigger an immediate PID update
    if (wasManual && !manualMode) forceControlUpdate = true;
    modeChanged = true;
  }

  String redirect = "/";
  if (setpointErr)           redirect += "?msg=sp_err";
  else if (setpointChanged)  redirect += "?msg=setpoint_ok";
  else if (modeChanged)      redirect += "?msg=mode_ok";
  server.sendHeader("Location", redirect);
  server.send(303);
}

void handleManual() {
  manualState = !manualState;
  shellySwitch(manualState);
  server.sendHeader("Location", "/");
  server.send(303);
}

// Validate a raw sensor reading before accepting it into the average
bool isValidTemp(float t) {
  if (isnan(t)) return false;
  if (t <= TEMP_VALID_MIN || t >= TEMP_VALID_MAX) return false;
  if (t == 85.0f) return false;  // DS18B20 power-on default, not a real measurement
  // Spike filter: once we have a baseline, reject implausible jumps
  if (hasValidInput && fabsf(t - lastValidInput) > TEMP_SPIKE_MAX_DELTA) {
    Serial.print("Spike rejected: ");
    Serial.print(t, 1);
    Serial.print(" C (last valid: ");
    Serial.print(lastValidInput, 1);
    Serial.println(" C)");
    return false;
  }
  return true;
}

// Run PID and update Shelly. Called only when a valid temperature is available.
void applyPIDControl() {
  if (!hasValidInput) return;
  Output = myPID.Run(Input);
  bool desiredState = (Output > 0);
  if (desiredState != shellyCurrentState) {
    shellySwitch(desiredState);
    shellyCurrentState = desiredState;
  }
}

// True non-blocking temperature sampling using a two-phase state machine.
// PHASE_REQUEST: starts conversion (returns immediately with WaitForConversion=false).
// PHASE_READ:    reads result after DS18B20_WAIT_MS; no delay() calls anywhere.
void updateTemperatureNonBlocking() {
  if (millis() < nextTempTime) return;

  if (tempPhase == PHASE_REQUEST) {
    if (sensors.getDeviceCount() == 0) {
      nextTempTime = millis() + 1000;
      return;
    }
    sensors.requestTemperaturesByIndex(0);  // returns immediately (WaitForConversion=false)
    tempPhase = PHASE_READ;
    nextTempTime = millis() + DS18B20_WAIT_MS;
    return;
  }

  // PHASE_READ — conversion is complete, read the result
  float t = sensors.getTempCByIndex(0);
  Serial.print("Temp (raw): ");
  Serial.print(t, 2);
  Serial.println(" C");

  sampleAttempts++;
  if (isValidTemp(t)) {
    sampleSum += t;
    sampleCount++;
  }

  // Immediately queue next conversion (no extra delay between samples)
  tempPhase = PHASE_REQUEST;
  nextTempTime = millis();

  if (sampleAttempts < NUM_SAMPLES) return;

  // --- Round complete: compute average and update control ---
  if (sampleCount > 0) {
    Input = sampleSum / (float)sampleCount;
    lastValidInput = Input;
    hasValidInput = true;
  } else {
    if (hasValidInput) {
      Input = lastValidInput;
      Serial.println("Sensor: no valid readings this round, keeping last value");
    } else {
      Serial.println("Sensor: no valid readings yet, suppressing control output");
    }
  }
  sampleAttempts = 0;
  sampleSum      = 0.0f;
  sampleCount    = 0;

  if (!manualMode) applyPIDControl();
}

void setup() {
  Serial.begin(115200);

  EEPROM.begin(EEPROM_SIZE);
  Setpoint = loadSetpointFromEEPROM();

  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());

  sensors.begin();
  Serial.print("1-Wire devices found: ");
  Serial.println(sensors.getDeviceCount());
  if (sensors.getDeviceCount() > 0) {
    sensors.setResolution(9);
    sensors.setWaitForConversion(false);  // enables async (non-blocking) reads
    Serial.println("Sensor: 9-bit, async mode");
  }

  if (shellyReachable()) {
    Serial.println("Shelly Plug S reachable");
    manualState = shellyGetState();
    shellyCurrentState = manualState;
    Serial.print("Plug initial state: ");
    Serial.println(manualState ? "On" : "Off");
  } else {
    Serial.println("Shelly Plug S unreachable – check IP: " + String(shelly_ip));
  }

  myPID.Start(Input, Output, Setpoint);
  myPID.SetOutputLimits(0, 1);

  server.on("/", handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/manual", HTTP_POST, handleManual);
  server.begin();
}

void loop() {
  server.handleClient();
  updateTemperatureNonBlocking();
  if (forceControlUpdate && !manualMode) {
    applyPIDControl();
    forceControlUpdate = false;
  }
  yield();  // let WiFi stack run without a fixed sleep
}
