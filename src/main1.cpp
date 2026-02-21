#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

// ============================================================
//  USER CONFIG — EDIT BEFORE FIRST FLASH
// ============================================================
#define WIFI_SSID        "AIPL-IOT"
#define WIFI_PASSWORD    "@ipl2027"

// ThingsBoard
#define TB_HOST          "mqtt.thingsboard.cloud"
#define TB_PORT          1883
#define TB_ACCESS_TOKEN  "J1R7Lw0dNx17T6HVifjX"

// Firmware version — update this with every new flash so you can track it
#define FIRMWARE_VERSION   "v8.0"

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
const int   LIGHT_PIN  = 26;
const int   RELAY_ON   = HIGH;
const int   RELAY_OFF  = LOW;
const float WATTAGE    = 150.0f;
const float VOLTAGE    = 120.0f;

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
const unsigned long TELE_INTERVAL  = 5000;   // telemetry every 5s
const unsigned long WIFI_CHECK_MS  = 15000;  // WiFi health check every 15s
const unsigned long WDT_TIMEOUT_S  = 30;     // watchdog resets after 30s hang

// ============================================================
//  STATE
// ============================================================
bool          lightState      = false;
bool          apMode          = true;

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
         ",\"firmware\":\""  + String(FIRMWARE_VERSION) + "\"}";
}

// ============================================================
//  SET LIGHT — exact desired state
// ============================================================
void setLightState(bool state) {
  if (lightState == state) return;

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
  doc["light_state"]  = lightState;
  doc["on_seconds"]   = getOnSeconds();
  doc["off_seconds"]  = getOffSeconds();
  doc["kwh_used"]     = getKwh();
  doc["rssi"]         = WiFi.RSSI();
  doc["uptime_s"]     = (millis() - sessionStartMs) / 1000;
  doc["wattage"]      = WATTAGE;
  doc["voltage"]      = VOLTAGE;
  doc["current_amps"] = WATTAGE / VOLTAGE;
  doc["firmware"]     = FIRMWARE_VERSION;
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
//  MQTT CALLBACK — handles RPC from ThingsBoard
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
    mqtt.publish(
      (String(TOPIC_RPC_RES) + reqId).c_str(),
      ("{\"state\":" + String(lightState ? "true" : "false") + "}").c_str()
    );
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
    publishAttr("firmware", "\"" + String(FIRMWARE_VERSION) + "\"");
    publishAttr("ip",       "\"" + WiFi.localIP().toString() + "\"");
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
//  WEB SERVER — all routes
// ============================================================
void setupWebServer() {

  // AP config page / status JSON
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

  // Save WiFi credentials and restart
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

  // POST /api/set?state=1 or 0
  server.on("/api/set", HTTP_POST, []() {
    if (apMode) { server.send(403, "application/json", "{\"error\":\"AP mode\"}"); return; }
    setLightState(server.arg("state") == "1");
    server.send(200, "application/json", getStatusJson());
  });

  // GET /api/status
  server.on("/api/status", HTTP_GET, []() {
    if (apMode) { server.send(403, "application/json", "{\"error\":\"AP mode\"}"); return; }
    server.send(200, "application/json", getStatusJson());
  });

  // GET /reset — clear WiFi config and restart
  server.on("/reset", HTTP_GET, []() {
    prefs.begin("wifi", false); prefs.clear(); prefs.end();
    server.send(200, "text/plain", "WiFi config cleared. Restarting...");
    delay(1000); ESP.restart();
  });

  // GET /restart — soft reboot
  server.on("/restart", HTTP_GET, []() {
    server.send(200, "text/plain", "Restarting...");
    delay(500); ESP.restart();
  });
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n╔═══════════════════════════════════════╗");
  Serial.println("║  AIPL High Bay Controller " FIRMWARE_VERSION "         ║");
  Serial.println("║  MQTT + WiFi AP Setup                 ║");
  Serial.println("╚═══════════════════════════════════════╝\n");

  // Watchdog — resets ESP if loop hangs > 30s
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  sessionStartMs = millis();

  // GPIO
  pinMode(LIGHT_PIN, OUTPUT);
  lightState = loadLightState();
  digitalWrite(LIGHT_PIN, lightState ? RELAY_ON : RELAY_OFF);
  Serial.printf("[GPIO] Pin %d = %s\n", LIGHT_PIN, lightState ? "ON" : "OFF");

  // Restore ON time accumulator
  totalOnSeconds = loadOnTime();
  if (lightState) lightOnStart = millis();

  // Load WiFi credentials (fallback to compile-time defines)
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
  esp_task_wdt_reset();

  server.handleClient();
  checkWiFiHealth();

  if (!apMode) {
    if (!mqtt.connected()) mqttReconnect();
    mqtt.loop();

    if (millis() - lastTelemetryMs >= TELE_INTERVAL) {
      lastTelemetryMs = millis();
      publishTelemetry();
    }
  }
}