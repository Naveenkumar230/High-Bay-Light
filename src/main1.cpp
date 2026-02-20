/*
 * ============================================================
 *  AIPL High Bay Light Controller
 *  main1.cpp — ESP32 Hardware Layer v7.0
 *  PlatformIO | Arduino Framework
 *
 *  FULL WIRELESS OTA — 3 methods (NO USB cable ever needed):
 *
 *  METHOD 1 — Arduino IDE / PlatformIO OTA
 *    Upload directly from IDE over WiFi (same network)
 *    Hostname: ESP32-AIPL-Light.local
 *    Password: aipl@OTA#2025
 *
 *  METHOD 2 — Browser Web Upload (http://DEVICE_IP/ota)
 *    Open browser → go to device IP → /ota page
 *    Drag & drop or browse .bin file → click Flash
 *    Works from phone, tablet, any browser on same WiFi
 *    Password protected: username=admin password=aipl1234
 *
 *  METHOD 3 — Auto Update from URL (MQTT trigger)
 *    Send RPC command from ThingsBoard with a .bin URL
 *    Device downloads and flashes itself automatically
 *    {"method":"otaUpdate","params":{"url":"http://your-server/firmware.bin"}}
 *
 *  SAFETY FEATURES:
 *    - Relay turns OFF automatically before any OTA flash
 *    - Rollback protection (verifies flash before reboot)
 *    - OTA progress published to ThingsBoard in real-time
 *    - Failed OTA → device stays on old firmware, does NOT brick
 *    - Watchdog timer: auto-recovers from firmware crashes
 *    - WiFi auto-reconnect: never stays offline
 * ============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>    // Watchdog timer

// ============================================================
//  USER CONFIG — EDIT BEFORE FIRST FLASH
// ============================================================
#define WIFI_SSID        "YOUR_WIFI_SSID"
#define WIFI_PASSWORD    "YOUR_WIFI_PASSWORD"

// ThingsBoard
// ThingsBoard
#define TB_HOST          "mqtt.thingsboard.cloud"
#define TB_PORT          1883
#define TB_ACCESS_TOKEN  "J1R7Lw0dNx17T6HVifjX"

// OTA passwords — change these to something private
#define OTA_IDE_PASSWORD   "aipl@OTA#2025"     // Arduino IDE / PlatformIO OTA
#define OTA_WEB_USER       "admin"              // Browser OTA page username
#define OTA_WEB_PASSWORD   "aipl1234"           // Browser OTA page password

// Firmware version — update this with every new flash so you can track it
#define FIRMWARE_VERSION   "v7.1"

// ============================================================
//  AP MODE (first-time WiFi config)
// ============================================================
const char*     AP_SSID     = "AIPL-Light-Setup";
const char*     AP_PASSWORD = "12345678";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GW(192, 168, 4, 1);
const IPAddress AP_SUB(255, 255, 255, 0);

// ============================================================
//  HARDWARE
// ============================================================
const char* OTA_HOSTNAME = "ESP32-AIPL-Light";
const int   LIGHT_PIN    = 26;
const int   RELAY_ON     = HIGH;
const int   RELAY_OFF    = LOW;
const float WATTAGE      = 150.0f;
const float VOLTAGE      = 120.0f;

// ============================================================
//  MQTT TOPICS
// ============================================================
#define TOPIC_TELE    "v1/devices/me/telemetry"
#define TOPIC_ATTR    "v1/devices/me/attributes"
#define TOPIC_RPC_SUB "v1/devices/me/rpc/request/+"
#define TOPIC_RPC_RES "v1/devices/me/rpc/response/"

// ============================================================
//  INTERVALS
// ============================================================
const unsigned long TELE_INTERVAL    = 5000;    // telemetry every 5s
const unsigned long WIFI_CHECK_MS    = 15000;   // WiFi health check every 15s
const unsigned long WDT_TIMEOUT_S    = 30;      // watchdog resets after 30s hang

// ============================================================
//  STATE
// ============================================================
bool          lightState      = false;
bool          apMode          = true;
bool          otaBusy         = false;   // true during any OTA flash

Preferences   prefs;
String        savedSSID       = "";
String        savedPass       = "";

unsigned long lightOnStart    = 0;
unsigned long totalOnSeconds  = 0;
unsigned long sessionStartMs  = 0;
unsigned long lastTelemetryMs = 0;
unsigned long lastWiFiCheckMs = 0;

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);
WebServer    server(80);

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
void          setLightState(bool state);
void          saveLightState(bool state);
bool          loadLightState();
void          saveOnTime(unsigned long s);
unsigned long loadOnTime();
unsigned long getOnSeconds();
unsigned long getOffSeconds();
float         getKwh();
void          publishTelemetry();
void          publishAttr(const char* key, String val);
void          mqttCallback(char* topic, byte* payload, unsigned int len);
void          mqttReconnect();
void          setupMQTT();
void          setupOTA_IDE();
void          setupOTA_Web();
void          doURLOTA(String url, String reqId);
void          startAPMode();
void          setupWebServer();
void          checkWiFiHealth();
String        getStatusJson();

// ============================================================
//  PREFERENCES
// ============================================================
void saveLightState(bool s) {
  prefs.begin("ls", false); prefs.putBool("l1", s); prefs.end();
}
bool loadLightState() {
  prefs.begin("ls", true); bool s = prefs.getBool("l1", false); prefs.end(); return s;
}
void saveOnTime(unsigned long s) {
  prefs.begin("ot", false); prefs.putULong("t", s); prefs.end();
}
unsigned long loadOnTime() {
  prefs.begin("ot", true); unsigned long t = prefs.getULong("t", 0); prefs.end(); return t;
}

// ============================================================
//  TIME HELPERS
// ============================================================
unsigned long getOnSeconds() {
  unsigned long s = totalOnSeconds;
  if (lightState && lightOnStart > 0)
    s += (millis() - lightOnStart) / 1000;
  return s;
}
unsigned long getOffSeconds() {
  unsigned long up = (millis() - sessionStartMs) / 1000;
  unsigned long on = getOnSeconds();
  return (up > on) ? (up - on) : 0;
}
float getKwh() {
  return (WATTAGE / 1000.0f) * (getOnSeconds() / 3600.0f);
}

// ============================================================
//  STATUS JSON (shared by all endpoints)
// ============================================================
String getStatusJson() {
  return "{\"state\":"       + String(lightState ? "true" : "false") +
         ",\"on_seconds\":"  + String(getOnSeconds())   +
         ",\"off_seconds\":" + String(getOffSeconds())  +
         ",\"kwh\":"         + String(getKwh(), 4)      +
         ",\"rssi\":"        + String(WiFi.RSSI())      +
         ",\"ip\":\""        + WiFi.localIP().toString() + "\"" +
         ",\"mqtt\":"        + String(mqtt.connected() ? "true" : "false") +
         ",\"firmware\":\""  + String(FIRMWARE_VERSION) + "\"" +
         ",\"ota_busy\":"    + String(otaBusy ? "true" : "false") + "}";
}

// ============================================================
//  SET LIGHT — exact desired state
// ============================================================
void setLightState(bool state) {
  if (lightState == state) return;
  if (otaBusy) return;   // block during OTA flash

  if (lightState && !state && lightOnStart > 0) {
    totalOnSeconds += (millis() - lightOnStart) / 1000;
    saveOnTime(totalOnSeconds);
    lightOnStart = 0;
  }
  if (!lightState && state) lightOnStart = millis();

  lightState = state;
  digitalWrite(LIGHT_PIN, state ? RELAY_ON : RELAY_OFF);
  saveLightState(state);
  Serial.printf("[RELAY] %s\n", state ? "ON" : "OFF");

  if (!apMode && mqtt.connected()) {
    publishTelemetry();
    publishAttr("lightState", state ? "true" : "false");
  }
}

// ============================================================
//  MQTT PUBLISH
// ============================================================
void publishTelemetry() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<300> doc;
  doc["light_state"]    = lightState;
  doc["on_seconds"]     = getOnSeconds();
  doc["off_seconds"]    = getOffSeconds();
  doc["kwh_used"]       = getKwh();
  doc["rssi"]           = WiFi.RSSI();
  doc["uptime_s"]       = (millis() - sessionStartMs) / 1000;
  doc["wattage"]        = WATTAGE;
  doc["voltage"]        = VOLTAGE;
  doc["current_amps"]   = WATTAGE / VOLTAGE;
  doc["firmware"]       = FIRMWARE_VERSION;
  doc["ota_busy"]       = otaBusy;
  char buf[300];
  serializeJson(doc, buf);
  mqtt.publish(TOPIC_TELE, buf);
}

void publishAttr(const char* key, String val) {
  if (!mqtt.connected()) return;
  String p = "{\"" + String(key) + "\":" + val + "}";
  mqtt.publish(TOPIC_ATTR, p.c_str());
}

// ============================================================
//  METHOD 3 — URL-BASED OTA
//  Called from MQTT RPC or web endpoint
//  Downloads .bin from a URL and flashes it
// ============================================================
void doURLOTA(String url, String reqId) {
  Serial.println("[OTA-URL] Starting download from: " + url);

  // Notify ThingsBoard: OTA started
  if (mqtt.connected()) {
    String startMsg = "{\"ota_status\":\"DOWNLOADING\",\"url\":\"" + url + "\"}";
    mqtt.publish(TOPIC_TELE, startMsg.c_str());
  }

  otaBusy = true;

  // Safety: turn relay OFF before flashing
  digitalWrite(LIGHT_PIN, RELAY_OFF);
  delay(500);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(30000);   // 30s timeout for download

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[OTA-URL] HTTP error: %d\n", httpCode);
    if (mqtt.connected()) {
      String fail = "{\"ota_status\":\"FAILED\",\"reason\":\"HTTP_" + String(httpCode) + "\"}";
      mqtt.publish(TOPIC_TELE, fail.c_str());
      if (reqId.length() > 0)
        mqtt.publish((String(TOPIC_RPC_RES) + reqId).c_str(), "{\"ota\":\"failed\",\"reason\":\"http_error\"}");
    }
    http.end();
    otaBusy = false;
    // Restore relay to previous state
    digitalWrite(LIGHT_PIN, lightState ? RELAY_ON : RELAY_OFF);
    return;
  }

  int contentLen = http.getSize();
  Serial.printf("[OTA-URL] Firmware size: %d bytes\n", contentLen);

  if (!Update.begin(contentLen > 0 ? contentLen : UPDATE_SIZE_UNKNOWN)) {
    Serial.println("[OTA-URL] Update.begin() failed");
    Update.printError(Serial);
    http.end();
    otaBusy = false;
    digitalWrite(LIGHT_PIN, lightState ? RELAY_ON : RELAY_OFF);
    return;
  }

  WiFiClient* stream  = http.getStreamPtr();
  size_t      written = 0;
  uint8_t     buf[1024];
  unsigned long lastProgress = 0;

  // Stream firmware and write to flash
  while (http.connected() && (written < (size_t)contentLen || contentLen < 0)) {
    size_t avail = stream->available();
    if (avail > 0) {
      size_t toRead = min(avail, sizeof(buf));
      size_t rd     = stream->readBytes(buf, toRead);
      size_t wr     = Update.write(buf, rd);
      written += wr;

      // Report progress every 10%
      int pct = contentLen > 0 ? (written * 100) / contentLen : 0;
      if (pct != lastProgress && pct % 10 == 0) {
        lastProgress = pct;
        Serial.printf("[OTA-URL] Progress: %d%%\n", pct);
        if (mqtt.connected()) {
          String prog = "{\"ota_status\":\"FLASHING\",\"ota_progress\":" + String(pct) + "}";
          mqtt.publish(TOPIC_TELE, prog.c_str());
          mqtt.loop();
        }
        esp_task_wdt_reset();   // pet the watchdog during long flash
      }
    } else {
      delay(1);
    }
  }

  http.end();

  if (Update.end(true)) {
    Serial.println("[OTA-URL] Flash complete! Rebooting...");
    if (mqtt.connected()) {
      mqtt.publish(TOPIC_TELE, "{\"ota_status\":\"COMPLETE\",\"ota_progress\":100}");
      if (reqId.length() > 0)
        mqtt.publish((String(TOPIC_RPC_RES) + reqId).c_str(), "{\"ota\":\"success\"}");
      mqtt.loop();
      delay(500);
    }
    delay(1000);
    ESP.restart();
  } else {
    Serial.println("[OTA-URL] Flash FAILED");
    Update.printError(Serial);
    if (mqtt.connected()) {
      mqtt.publish(TOPIC_TELE, "{\"ota_status\":\"FAILED\",\"reason\":\"write_error\"}");
      if (reqId.length() > 0)
        mqtt.publish((String(TOPIC_RPC_RES) + reqId).c_str(), "{\"ota\":\"failed\",\"reason\":\"write_error\"}");
    }
    otaBusy = false;
    digitalWrite(LIGHT_PIN, lightState ? RELAY_ON : RELAY_OFF);
  }
}

// ============================================================
//  MQTT CALLBACK — handles all RPC from ThingsBoard
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String topicStr = String(topic);
  String msg      = "";
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  String reqId = topicStr.substring(topicStr.lastIndexOf('/') + 1);

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, msg)) return;

  String method = doc["method"] | "";
  Serial.println("[MQTT] RPC: " + method);

  // ── setLight ──
  if (method == "setLight") {
    bool desired = doc["params"]["state"] | false;
    setLightState(desired);
    String resTopic = String(TOPIC_RPC_RES) + reqId;
    mqtt.publish(resTopic.c_str(),
      ("{\"state\":" + String(lightState ? "true" : "false") + "}").c_str());
  }

  // ── getState ──
  else if (method == "getState") {
    mqtt.publish((String(TOPIC_RPC_RES) + reqId).c_str(), getStatusJson().c_str());
  }

  // ── resetOnTime ──
  else if (method == "resetOnTime") {
    totalOnSeconds = 0;
    lightOnStart   = lightState ? millis() : 0;
    saveOnTime(0);
    mqtt.publish((String(TOPIC_RPC_RES) + reqId).c_str(), "{\"reset\":true}");
  }

  // ── restart ── (soft reboot via ThingsBoard)
  else if (method == "restart") {
    mqtt.publish((String(TOPIC_RPC_RES) + reqId).c_str(), "{\"restarting\":true}");
    mqtt.loop();
    delay(500);
    ESP.restart();
  }

  // ── otaUpdate ── (METHOD 3: URL-based OTA via ThingsBoard)
  // Send from ThingsBoard:
  // {"method":"otaUpdate","params":{"url":"http://192.168.1.50:8080/firmware.bin"}}
  else if (method == "otaUpdate") {
    String url = doc["params"]["url"] | "";
    if (url.length() == 0) {
      mqtt.publish((String(TOPIC_RPC_RES) + reqId).c_str(), "{\"error\":\"no url provided\"}");
      return;
    }
    mqtt.publish((String(TOPIC_RPC_RES) + reqId).c_str(), "{\"ota\":\"starting\"}");
    mqtt.loop();
    delay(200);
    doURLOTA(url, reqId);
  }
}

// ============================================================
//  MQTT RECONNECT
// ============================================================
void mqttReconnect() {
  if (mqtt.connected() || apMode) return;
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 5000) return;
  lastTry = millis();
  Serial.print("[MQTT] Connecting...");
  if (mqtt.connect("ESP32-AIPL", TB_ACCESS_TOKEN, NULL)) {
    Serial.println(" OK");
    mqtt.subscribe(TOPIC_RPC_SUB);
    publishTelemetry();
    publishAttr("firmware",  "\"" + String(FIRMWARE_VERSION) + "\"");
    publishAttr("ip",        "\"" + WiFi.localIP().toString() + "\"");
    publishAttr("ota_modes", "\"IDE+Web+URL\"");
  } else {
    Serial.printf(" FAIL rc=%d\n", mqtt.state());
  }
}

void setupMQTT() {
  mqtt.setServer(TB_HOST, TB_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqttReconnect();
}

// ============================================================
//  METHOD 1 — ARDUINO IDE / PLATFORMIO OTA
//  In PlatformIO: set upload_port = ESP32-AIPL-Light.local
//  In Arduino IDE: Tools → Port → ESP32-AIPL-Light.local
// ============================================================
void setupOTA_IDE() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_IDE_PASSWORD);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "firmware" : "filesystem";
    Serial.println("[OTA-IDE] Start: " + type);
    otaBusy = true;
    // Safety: turn relay OFF
    digitalWrite(LIGHT_PIN, RELAY_OFF);
    if (mqtt.connected())
      mqtt.publish(TOPIC_TELE, "{\"ota_status\":\"IDE_UPLOADING\"}");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA-IDE] Complete!");
    if (mqtt.connected())
      mqtt.publish(TOPIC_TELE, "{\"ota_status\":\"COMPLETE\",\"ota_progress\":100}");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int pct = progress / (total / 100);
    Serial.printf("[OTA-IDE] %u%%\r", pct);
    // Publish to ThingsBoard every 20%
    static int lastPct = -1;
    if (pct != lastPct && pct % 20 == 0) {
      lastPct = pct;
      if (mqtt.connected()) {
        String p = "{\"ota_status\":\"IDE_UPLOADING\",\"ota_progress\":" + String(pct) + "}";
        mqtt.publish(TOPIC_TELE, p.c_str());
        mqtt.loop();
      }
    }
    esp_task_wdt_reset();
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA-IDE] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)         Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)   Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)     Serial.println("End Failed");
    otaBusy = false;
    // Restore relay
    digitalWrite(LIGHT_PIN, lightState ? RELAY_ON : RELAY_OFF);
    if (mqtt.connected())
      mqtt.publish(TOPIC_TELE, "{\"ota_status\":\"FAILED\"}");
  });

  ArduinoOTA.begin();
  Serial.println("[OTA-IDE] Ready — hostname: " + String(OTA_HOSTNAME) + ".local");
  Serial.println("[OTA-IDE] Password: " + String(OTA_IDE_PASSWORD));
}

// ============================================================
//  AP MODE
// ============================================================
void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GW, AP_SUB);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.println("[AP] Started @ " + WiFi.softAPIP().toString());
}

// ============================================================
//  WiFi HEALTH CHECK — auto-reconnect if dropped
// ============================================================
void checkWiFiHealth() {
  if (apMode) return;
  if (millis() - lastWiFiCheckMs < WIFI_CHECK_MS) return;
  lastWiFiCheckMs = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected — reconnecting...");
    WiFi.disconnect();
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
      delay(500); Serial.print(".");
      esp_task_wdt_reset();
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] Reconnected: " + WiFi.localIP().toString());
    } else {
      Serial.println("[WiFi] Reconnect failed — will retry");
    }
  }
}

// ============================================================
//  METHOD 2 — BROWSER WEB OTA PAGE
//  Open: http://DEVICE_IP/ota
//  Password protected — username: admin, password: aipl1234
// ============================================================
void setupOTA_Web() {

  // ── /ota — web upload page ──
  server.on("/ota", HTTP_GET, []() {
    // Basic auth check
    if (!server.authenticate(OTA_WEB_USER, OTA_WEB_PASSWORD)) {
      return server.requestAuthentication();
    }
    server.send(200, "text/html", R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>AIPL OTA Update</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:'Segoe UI',sans-serif;background:#0f172a;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:20px}
  .card{background:#1e293b;border-radius:20px;padding:36px;width:100%;max-width:440px;border:1px solid #334155;box-shadow:0 20px 60px rgba(0,0,0,.5)}
  .logo{font-size:10px;letter-spacing:4px;color:#3b82f6;text-transform:uppercase;margin-bottom:6px;font-family:monospace}
  h1{font-size:20px;color:#f1f5f9;margin-bottom:4px}
  .sub{font-size:12px;color:#64748b;margin-bottom:28px}
  .version{display:inline-block;background:#1d4ed8;color:#93c5fd;font-size:11px;font-family:monospace;padding:4px 10px;border-radius:6px;margin-bottom:20px}
  .drop-zone{border:2px dashed #334155;border-radius:14px;padding:36px 20px;text-align:center;cursor:pointer;transition:.2s;position:relative;background:#0f172a}
  .drop-zone:hover,.drop-zone.drag{border-color:#3b82f6;background:#1e3a5f}
  .drop-zone input[type=file]{position:absolute;inset:0;opacity:0;cursor:pointer;width:100%;height:100%}
  .drop-icon{font-size:36px;margin-bottom:10px}
  .drop-text{font-size:13px;color:#64748b}
  .drop-text strong{color:#94a3b8;display:block;margin-top:4px;font-size:12px;font-family:monospace}
  #fileInfo{margin-top:12px;font-size:12px;color:#3b82f6;font-family:monospace;min-height:18px}
  .flash-btn{
    width:100%;margin-top:16px;padding:14px;
    background:linear-gradient(135deg,#1d4ed8,#3b82f6);
    color:#fff;border:none;border-radius:12px;
    font-size:15px;font-weight:700;cursor:pointer;transition:.2s;
  }
  .flash-btn:hover:not(:disabled){background:linear-gradient(135deg,#1e40af,#2563eb);transform:translateY(-1px)}
  .flash-btn:disabled{opacity:.4;cursor:not-allowed;transform:none}
  .progress-wrap{margin-top:16px;display:none}
  .progress-bar{height:8px;background:#1e293b;border:1px solid #334155;border-radius:4px;overflow:hidden}
  .progress-fill{height:100%;background:linear-gradient(90deg,#1d4ed8,#3b82f6);border-radius:4px;width:0;transition:width .3s}
  .progress-label{font-size:12px;color:#64748b;margin-top:6px;font-family:monospace;text-align:center}
  .status{margin-top:14px;font-size:13px;text-align:center;font-family:monospace;min-height:20px}
  .status.ok{color:#22c55e}
  .status.err{color:#ef4444}
  .status.info{color:#3b82f6}
  .info-row{display:flex;gap:8px;margin-bottom:16px}
  .info-chip{flex:1;background:#0f172a;border:1px solid #334155;border-radius:8px;padding:8px;text-align:center}
  .info-chip .val{font-size:13px;font-weight:700;color:#3b82f6;font-family:monospace}
  .info-chip .lbl{font-size:10px;color:#64748b;text-transform:uppercase;letter-spacing:.5px;margin-top:2px}
</style>
</head>
<body>
<div class="card">
  <div class="logo">AIPL Industrial</div>
  <h1>Wireless OTA Update</h1>
  <div class="sub">High Bay Controller — Browser Flash</div>

  <div class="version" id="curVer">Current: ...)rawliteral" FIRMWARE_VERSION R"rawliteral(</div>

  <div class="info-row">
    <div class="info-chip">
      <div class="val" id="chipIP">--</div>
      <div class="lbl">Device IP</div>
    </div>
    <div class="info-chip">
      <div class="val" id="chipRSSI">--</div>
      <div class="lbl">WiFi Signal</div>
    </div>
  </div>

  <div class="drop-zone" id="dropZone">
    <input type="file" id="binFile" accept=".bin"/>
    <div class="drop-icon">&#128190;</div>
    <div class="drop-text">
      Drop firmware .bin here or click to browse
      <strong>Only .bin files from PlatformIO or Arduino IDE</strong>
    </div>
  </div>
  <div id="fileInfo"></div>

  <button class="flash-btn" id="flashBtn" disabled>&#9654;&nbsp; Flash Firmware</button>

  <div class="progress-wrap" id="progWrap">
    <div class="progress-bar"><div class="progress-fill" id="progFill"></div></div>
    <div class="progress-label" id="progLabel">0%</div>
  </div>

  <div class="status info" id="statusMsg">Select a .bin file to begin</div>
</div>

<script>
const dz    = document.getElementById('dropZone');
const fi    = document.getElementById('binFile');
const info  = document.getElementById('fileInfo');
const btn   = document.getElementById('flashBtn');
const wrap  = document.getElementById('progWrap');
const fill  = document.getElementById('progFill');
const label = document.getElementById('progLabel');
const msg   = document.getElementById('statusMsg');

// Fetch current device info
fetch('/api/status').then(r=>r.json()).then(d=>{
  document.getElementById('chipIP').textContent   = d.ip   || location.hostname;
  document.getElementById('chipRSSI').textContent = (d.rssi || '--') + ' dBm';
}).catch(()=>{});

// Drag & drop
dz.addEventListener('dragover',  e=>{ e.preventDefault(); dz.classList.add('drag'); });
dz.addEventListener('dragleave', ()=> dz.classList.remove('drag'));
dz.addEventListener('drop', e=>{
  e.preventDefault(); dz.classList.remove('drag');
  const f = e.dataTransfer.files[0];
  if(f) handleFile(f);
});
fi.addEventListener('change', ()=>{ if(fi.files[0]) handleFile(fi.files[0]); });

function handleFile(f) {
  if (!f.name.endsWith('.bin')) {
    msg.className='status err'; msg.textContent='Error: Only .bin files are allowed'; return;
  }
  info.textContent = '📦 ' + f.name + ' — ' + (f.size/1024).toFixed(1) + ' KB';
  btn.disabled = false;
  msg.className='status info'; msg.textContent='Ready to flash — click Flash Firmware';
}

btn.addEventListener('click', () => {
  const f = fi.files[0] || null;
  if (!f) { alert('Select a .bin file first'); return; }
  if (!confirm('Flash ' + f.name + ' to device? Device will restart after.')) return;

  btn.disabled = true;
  wrap.style.display = 'block';
  msg.className='status info'; msg.textContent='Uploading...';

  const fd  = new FormData();
  fd.append('firmware', f, f.name);

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/ota/upload');

  xhr.upload.onprogress = e => {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      fill.style.width   = pct + '%';
      label.textContent  = 'Uploading... ' + pct + '%';
    }
  };

  xhr.onload = () => {
    if (xhr.status === 200) {
      fill.style.width  = '100%';
      label.textContent = '100% — Done!';
      msg.className='status ok';
      msg.textContent = '✓ Flash complete! Device restarting in 3 seconds...';
      setTimeout(()=>{ msg.textContent='Reloading page...'; setTimeout(()=>location.reload(),3000); }, 3000);
    } else {
      msg.className='status err';
      msg.textContent = '✗ Flash failed: ' + xhr.responseText;
      btn.disabled = false;
    }
  };

  xhr.onerror = () => {
    msg.className='status err';
    msg.textContent = '✗ Connection error — check device is on same network';
    btn.disabled = false;
  };

  xhr.send(fd);
});
</script>
</body>
</html>
)rawliteral");
  });

  // ── /ota/upload — receives the .bin and flashes it ──
  server.on("/ota/upload", HTTP_POST,
    // Response handler (after upload finishes)
    []() {
      if (!server.authenticate(OTA_WEB_USER, OTA_WEB_PASSWORD)) {
        return server.requestAuthentication();
      }
      if (Update.hasError()) {
        String err = Update.errorString();
        server.send(500, "text/plain", "Flash FAILED: " + err);
        Serial.println("[OTA-Web] FAILED: " + err);
        otaBusy = false;
        digitalWrite(LIGHT_PIN, lightState ? RELAY_ON : RELAY_OFF);
        if (mqtt.connected())
          mqtt.publish(TOPIC_TELE, "{\"ota_status\":\"FAILED\"}");
      } else {
        server.send(200, "text/plain", "OK");
        Serial.println("[OTA-Web] Flash complete — restarting...");
        if (mqtt.connected()) {
          mqtt.publish(TOPIC_TELE, "{\"ota_status\":\"COMPLETE\",\"ota_progress\":100}");
          mqtt.loop();
        }
        delay(1000);
        ESP.restart();
      }
    },
    // Upload handler (chunk by chunk)
    []() {
      if (!server.authenticate(OTA_WEB_USER, OTA_WEB_PASSWORD)) {
        return server.requestAuthentication();
      }
      HTTPUpload& upload = server.upload();

      if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA-Web] Start: %s\n", upload.filename.c_str());
        otaBusy = true;
        // Safety: turn relay OFF before flash
        digitalWrite(LIGHT_PIN, RELAY_OFF);
        if (mqtt.connected())
          mqtt.publish(TOPIC_TELE, "{\"ota_status\":\"WEB_UPLOADING\",\"ota_progress\":0}");
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
        }
      }
      else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
          Update.printError(Serial);
        }
        // Progress
        if (upload.totalSize > 0) {
          int pct = (upload.totalSize * 100) / (upload.totalSize + 1);
          static int lastPct2 = -1;
          if (pct != lastPct2 && pct % 20 == 0) {
            lastPct2 = pct;
            if (mqtt.connected()) {
              String prog = "{\"ota_status\":\"WEB_UPLOADING\",\"ota_progress\":" + String(pct) + "}";
              mqtt.publish(TOPIC_TELE, prog.c_str());
              mqtt.loop();
            }
          }
        }
        esp_task_wdt_reset();
      }
      else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
          Serial.printf("[OTA-Web] Uploaded %u bytes\n", upload.totalSize);
        } else {
          Update.printError(Serial);
        }
      }
    }
  );
}

// ============================================================
//  WEB SERVER — all routes
// ============================================================
void setupWebServer() {

  // AP config page
  server.on("/", HTTP_GET, []() {
    if (apMode) {
      server.send(200, "text/html",
        "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>*{box-sizing:border-box}body{font-family:sans-serif;background:#f5f7fa;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
        ".c{background:#fff;border-radius:16px;padding:32px;max-width:380px;width:100%;box-shadow:0 4px 24px rgba(0,0,0,.1)}"
        "h2{margin-bottom:20px;color:#0d1117}label{font-size:12px;font-weight:600;color:#374151;display:block;margin-bottom:4px;margin-top:12px}"
        "input[type=text],input[type=password]{width:100%;padding:10px;border:1.5px solid #d1d5db;border-radius:8px;font-size:14px}"
        "button{width:100%;margin-top:20px;padding:12px;background:#1a6bff;color:#fff;border:none;border-radius:8px;font-size:14px;font-weight:700;cursor:pointer}"
        "</style></head><body><div class='c'>"
        "<h2>&#128246; AIPL WiFi Setup</h2>"
        "<form action='/save' method='POST'>"
        "<label>WiFi SSID</label><input type='text' name='ssid' required placeholder='Network name'/>"
        "<label>Password</label><input type='password' name='password' placeholder='WiFi password'/>"
        "<button type='submit'>Save &amp; Connect</button>"
        "</form></div></body></html>");
    } else {
      server.send(200, "application/json", getStatusJson());
    }
  });

  // Save WiFi
  server.on("/save", HTTP_POST, []() {
    String s = server.arg("ssid");
    String p = server.arg("password");
    prefs.begin("wifi", false);
    prefs.putString("ssid", s);
    prefs.putString("password", p);
    prefs.end();
    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;text-align:center;padding:40px'>"
      "<h2 style='color:#10b981'>&#10003; Saved!</h2><p>Device restarting...</p></body></html>");
    delay(2000);
    ESP.restart();
  });

  // /api/set?state=1 or 0
  server.on("/api/set", HTTP_POST, []() {
    if (apMode) { server.send(403, "application/json", "{\"error\":\"AP mode\"}"); return; }
    if (otaBusy) { server.send(503, "application/json", "{\"error\":\"OTA in progress\"}"); return; }
    setLightState(server.arg("state") == "1");
    server.send(200, "application/json", getStatusJson());
  });

  // /api/status
  server.on("/api/status", HTTP_GET, []() {
    if (apMode) { server.send(403, "application/json", "{\"error\":\"AP mode\"}"); return; }
    server.send(200, "application/json", getStatusJson());
  });

  // /api/ota-url — trigger URL OTA via REST (alternative to MQTT)
  // POST body: {"url":"http://your-server/firmware.bin"}
  server.on("/api/ota-url", HTTP_POST, []() {
    if (apMode) { server.send(403, "application/json", "{\"error\":\"AP mode\"}"); return; }
    if (!server.authenticate(OTA_WEB_USER, OTA_WEB_PASSWORD)) {
      return server.requestAuthentication();
    }
    String body = server.arg("plain");
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"error\":\"invalid json\"}"); return;
    }
    String url = doc["url"] | "";
    if (url.length() == 0) {
      server.send(400, "application/json", "{\"error\":\"url required\"}"); return;
    }
    server.send(200, "application/json", "{\"ota\":\"starting\",\"url\":\"" + url + "\"}");
    delay(100);
    doURLOTA(url, "");
  });

  // /reset WiFi
  server.on("/reset", HTTP_GET, []() {
    prefs.begin("wifi", false); prefs.clear(); prefs.end();
    server.send(200, "text/plain", "WiFi config cleared. Restarting...");
    delay(1000); ESP.restart();
  });

  // /restart — soft reboot via browser
  server.on("/restart", HTTP_GET, []() {
    server.send(200, "text/plain", "Restarting...");
    delay(500); ESP.restart();
  });

  // Register OTA web upload routes
  setupOTA_Web();
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║  AIPL High Bay Controller v7.0        ║");
  Serial.println("║  3x Wireless OTA — No USB ever needed ║");
  Serial.println("╚═══════════════════════════════════════╝\n");

  // ── Watchdog — resets ESP if loop hangs > 30s ──
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  sessionStartMs = millis();

  // GPIO
  pinMode(LIGHT_PIN, OUTPUT);
  lightState = loadLightState();
  digitalWrite(LIGHT_PIN, lightState ? RELAY_ON : RELAY_OFF);
  Serial.printf("[GPIO] Pin %d = %s\n", LIGHT_PIN, lightState ? "ON" : "OFF");

  // Restore ON time
  totalOnSeconds = loadOnTime();
  if (lightState) lightOnStart = millis();

  // Load WiFi credentials
  prefs.begin("wifi", false);
  savedSSID = prefs.getString("ssid", WIFI_SSID);
  savedPass = prefs.getString("password", WIFI_PASSWORD);
  prefs.end();

  if (savedSSID.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    Serial.print("[WiFi] Connecting to " + savedSSID);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
      delay(500); Serial.print("."); tries++;
      esp_task_wdt_reset();
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
      apMode = false;
      Serial.println("[WiFi] Connected! IP: " + WiFi.localIP().toString());
      Serial.println("\n┌─ OTA METHODS ─────────────────────────────┐");
      Serial.println("│ 1. IDE/PlatformIO → " + String(OTA_HOSTNAME) + ".local");
      Serial.println("│    Password: " + String(OTA_IDE_PASSWORD));
      Serial.println("│ 2. Browser → http://" + WiFi.localIP().toString() + "/ota");
      Serial.println("│    Login: " + String(OTA_WEB_USER) + " / " + String(OTA_WEB_PASSWORD));
      Serial.println("│ 3. MQTT RPC → method: otaUpdate, params: {url:...}");
      Serial.println("└───────────────────────────────────────────┘\n");
      setupOTA_IDE();
      setupMQTT();
    } else {
      Serial.println("[WiFi] Failed — starting AP mode");
      startAPMode();
    }
  } else {
    Serial.println("[WiFi] No credentials — AP mode");
    startAPMode();
  }

  setupWebServer();
  server.begin();
  Serial.println("[HTTP] Server on port 80");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  esp_task_wdt_reset();   // pet the watchdog every loop

  server.handleClient();
  checkWiFiHealth();

  if (!apMode) {
    ArduinoOTA.handle();   // handles Method 1 IDE OTA

    if (!mqtt.connected()) mqttReconnect();
    mqtt.loop();

    if (millis() - lastTelemetryMs >= TELE_INTERVAL) {
      lastTelemetryMs = millis();
      publishTelemetry();
    }
  }
}