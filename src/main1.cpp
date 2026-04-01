

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

// ============================================================
//  USER CONFIG — edit before flashing each device
// ============================================================
#define WIFI_SSID        "AIPL-IOT"
#define WIFI_PASSWORD    "@ipl2027"

// ── HiveMQ Cloud ─────────────────────────────────────────────
#define HIVEMQ_HOST      "a0d0d0e9332a4d3db7516f6125f6e677.s1.eu.hivemq.cloud"
#define HIVEMQ_PORT      8883
#define HIVEMQ_USERNAME  "Highbaylight"
#define HIVEMQ_PASSWORD  "Naveen235623@@"

// ══════════════════════════════════════════════════════════════
//  DEVICE IDENTITY — uncomment ONLY ONE line before flashing
//  Format: AIPL/HighBay/Row_<R>/Light_<L>
// ══════════════════════════════════════════════════════════════

// ── ROW 1 ──────────────────────────────────────────────────
constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_1/Light_1";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_1/Light_2";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_1/Light_3";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_1/Light_4";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_1/Light_5";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_1/Light_6";

// ── ROW 2 ──────────────────────────────────────────────────
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_2/Light_1";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_2/Light_2";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_2/Light_3";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_2/Light_4";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_2/Light_5";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_2/Light_6";

// ── ROW 3 ──────────────────────────────────────────────────
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_3/Light_1";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_3/Light_2";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_3/Light_3";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_3/Light_4";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_3/Light_5";  // completed
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_3/Light_6";

// ── ROW 4 ──────────────────────────────────────────────────
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_4/Light_1";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_4/Light_2";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_4/Light_3";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_4/Light_4";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_4/Light_5";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_4/Light_6";

// ── ROW 5 ──────────────────────────────────────────────────
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_5/Light_1";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_5/Light_2";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_5/Light_3";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_5/Light_4";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_5/Light_5";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_5/Light_6";

// ── ROW 6 ──────────────────────────────────────────────────
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_6/Light_1";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_6/Light_2";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_6/Light_3";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_6/Light_4";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_6/Light_5";
// constexpr const char* DEVICE_ID = "AIPL/HighBay/Row_6/Light_6";

// ── Parse ROW_INDEX and LIGHT_INDEX from DEVICE_ID at compile time ──
#define ROW_INDEX   (DEVICE_ID[17] - '1')
#define LIGHT_INDEX (DEVICE_ID[25] - '1')

#define FIRMWARE_VERSION "v9.2"

// ============================================================
//  AP MODE
// ============================================================
const char*     AP_SSID     = "AIPL-Light-Setup";
const char*     AP_PASSWORD = "12345678";
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GW(192, 168, 4, 1);
const IPAddress AP_SUB(255, 255, 255, 0);

// ============================================================
//  HARDWARE — JQC3F-05VDC-C is ACTIVE LOW
// ============================================================
const int LIGHT_PIN = 26;
const int RELAY_ON  = LOW;
const int RELAY_OFF = HIGH;

const float WATTAGE = 150.0f;
const float VOLTAGE = 120.0f;

// ============================================================
//  MQTT TOPICS
// ============================================================
char TOPIC_CMD_SINGLE[64];
char TOPIC_CMD_ROW[48];
char TOPIC_CMD_ALL[]  = "aipl/all/command";
char TOPIC_STATE[64];
char TOPIC_TELE[64];

// ============================================================
//  INTERVALS
// ============================================================
const unsigned long TELE_INTERVAL = 5000;
const unsigned long WIFI_CHECK_MS = 10000;
const unsigned long WDT_TIMEOUT_S = 30;

// ============================================================
//  STATE
// ============================================================
bool          lightState      = true;
bool          userForcedOff   = false;  // FIX v9.2: true when user explicitly commanded OFF
bool          apMode          = true;

Preferences   prefs;
String        savedSSID       = "";
String        savedPass       = "";

unsigned long lightOnStart    = 0;
unsigned long totalOnSeconds  = 0;
unsigned long sessionStartMs  = 0;
unsigned long lastTelemetryMs = 0;
unsigned long lastWiFiCheckMs = 0;

WiFiClientSecure tlsClient;
PubSubClient     mqtt(tlsClient);
WebServer        server(80);

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
void          setLightState(bool state, bool saveToFlash = true);
void          forceLight(bool state);
void          saveLightState(bool s);
bool          loadLightState();
void          saveOnTime(unsigned long s);
unsigned long loadOnTime();
unsigned long getOnSeconds();
unsigned long getOffSeconds();
float         getKwh();
void          publishTelemetry();
void          publishState();
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
  prefs.begin("ls", true); bool s = prefs.getBool("l1", true); prefs.end(); return s;
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
//  STATUS JSON
// ============================================================
String getStatusJson() {
  return String("{") +
    "\"state\":"           + (lightState    ? "true" : "false") +
    ",\"userForcedOff\":"  + (userForcedOff ? "true" : "false") +
    ",\"row\":"            + String(ROW_INDEX)    +
    ",\"light\":"          + String(LIGHT_INDEX)  +
    ",\"on_seconds\":"     + String(getOnSeconds())   +
    ",\"off_seconds\":"    + String(getOffSeconds())  +
    ",\"kwh\":"            + String(getKwh(), 4)  +
    ",\"rssi\":"           + String(WiFi.RSSI())  +
    ",\"ip\":\""           + WiFi.localIP().toString() + "\"" +
    ",\"mqtt\":"           + (mqtt.connected() ? "true" : "false") +
    ",\"firmware\":\""     + FIRMWARE_VERSION + "\"}";
}

// ============================================================
//  FORCE LIGHT — raw GPIO only, no MQTT publish
//  FIX v9.2: skips if user deliberately commanded OFF
// ============================================================
void forceLight(bool state) {
  // FIX v9.2: Do NOT force ON if user explicitly turned OFF
  if (state && userForcedOff) {
    Serial.println("[FAIL-SAFE] Skipped — user commanded OFF, respecting intent");
    return;
  }

  if (!lightState && state) lightOnStart = millis();
  if (lightState && !state && lightOnStart > 0) {
    totalOnSeconds += (millis() - lightOnStart) / 1000;
    saveOnTime(totalOnSeconds);
    lightOnStart = 0;
  }
  lightState = state;
  digitalWrite(LIGHT_PIN, state ? RELAY_ON : RELAY_OFF);
  Serial.printf("[FORCE] Light %s (fail-safe)\n", state ? "ON" : "OFF");
}

// ============================================================
//  SET LIGHT — normal controlled state change + MQTT publish
//  FIX v9.2: saves userForcedOff so power cycle knows intent
// ============================================================
void setLightState(bool state, bool saveToFlash) {
  // FIX v9.2: always record what the user explicitly wants
  userForcedOff = !state;

  if (lightState == state) return;

  if (lightState && !state && lightOnStart > 0) {
    totalOnSeconds += (millis() - lightOnStart) / 1000;
    saveOnTime(totalOnSeconds);
    lightOnStart = 0;
  }
  if (!lightState && state) lightOnStart = millis();

  lightState = state;

  digitalWrite(LIGHT_PIN, state ? RELAY_ON : RELAY_OFF);
  Serial.printf("[RELAY] %s  pin%d=%s\n",
                state ? "ON" : "OFF",
                LIGHT_PIN,
                state ? "LOW" : "HIGH");

  if (saveToFlash) saveLightState(state);

  if (!apMode && mqtt.connected()) {
    publishState();
    publishTelemetry();
  }
}

// ============================================================
//  MQTT PUBLISH
// ============================================================
void publishState() {
  if (!mqtt.connected()) return;
  mqtt.publish(TOPIC_STATE, lightState ? "ON" : "OFF", true);
}

void publishTelemetry() {
  if (!mqtt.connected()) return;
  StaticJsonDocument<300> doc;
  doc["light_state"]  = lightState;
  doc["row"]          = ROW_INDEX;
  doc["light"]        = LIGHT_INDEX;
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

// ============================================================
//  MQTT CALLBACK
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String topicStr = String(topic);
  String msg = "";
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();

  bool desired = (msg == "ON" || msg == "on" || msg == "1" || msg == "true");
  Serial.printf("[MQTT RX] %s → %s\n", topic, desired ? "ON" : "OFF");

  if (topicStr == String(TOPIC_CMD_SINGLE) ||
      topicStr == String(TOPIC_CMD_ROW)    ||
      topicStr == String(TOPIC_CMD_ALL)) {
    setLightState(desired);
  }
}

// ============================================================
//  MQTT RECONNECT
//  FIX v9.2: fail-safe only triggers if user did NOT force OFF
// ============================================================
void mqttReconnect() {
  if (mqtt.connected() || apMode) return;
  static unsigned long lastTry = 0;
  if (millis() - lastTry < 5000) return;
  lastTry = millis();

  // FIX v9.2: respect user OFF intent during MQTT outage
  if (!lightState && !userForcedOff) {
    Serial.println("[FAIL-SAFE] MQTT down → forcing light ON");
    forceLight(true);
  }

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "aipl-r%d-l%d-%04X",
           ROW_INDEX, LIGHT_INDEX, (uint16_t)(ESP.getEfuseMac() & 0xFFFF));

  Serial.printf("[MQTT] Connecting as %s ...", clientId);

  if (mqtt.connect(clientId,
                   HIVEMQ_USERNAME, HIVEMQ_PASSWORD,
                   TOPIC_STATE, 1, true, "ON")) {
    Serial.println(" OK");
    mqtt.subscribe(TOPIC_CMD_SINGLE, 1);
    mqtt.subscribe(TOPIC_CMD_ROW,    1);
    mqtt.subscribe(TOPIC_CMD_ALL,    1);
    Serial.println("[MQTT] Subscribed");
    publishState();
    publishTelemetry();
  } else {
    Serial.printf(" FAIL rc=%d\n", mqtt.state());
  }
}

void setupMQTT() {
  snprintf(TOPIC_CMD_SINGLE, sizeof(TOPIC_CMD_SINGLE),
           "aipl/row/%d/light/%d/command", ROW_INDEX, LIGHT_INDEX);
  snprintf(TOPIC_CMD_ROW,    sizeof(TOPIC_CMD_ROW),
           "aipl/row/%d/command", ROW_INDEX);
  snprintf(TOPIC_STATE,      sizeof(TOPIC_STATE),
           "aipl/row/%d/light/%d/state", ROW_INDEX, LIGHT_INDEX);
  snprintf(TOPIC_TELE,       sizeof(TOPIC_TELE),
           "aipl/row/%d/light/%d/telemetry", ROW_INDEX, LIGHT_INDEX);

  Serial.println("[MQTT] Topics:");
  Serial.printf("  CMD single : %s\n", TOPIC_CMD_SINGLE);
  Serial.printf("  CMD row    : %s\n", TOPIC_CMD_ROW);
  Serial.printf("  CMD all    : %s\n", TOPIC_CMD_ALL);
  Serial.printf("  STATE      : %s\n", TOPIC_STATE);
  Serial.printf("  TELE       : %s\n", TOPIC_TELE);

  tlsClient.setInsecure();
  mqtt.setServer(HIVEMQ_HOST, HIVEMQ_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(10);

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
  forceLight(true);
  Serial.println("[FAIL-SAFE] AP mode → light ON");
}

// ============================================================
//  WiFi HEALTH CHECK
//  FIX v9.2: fail-safe only triggers if user did NOT force OFF
// ============================================================
void checkWiFiHealth() {
  if (apMode) return;
  if (millis() - lastWiFiCheckMs < WIFI_CHECK_MS) return;
  lastWiFiCheckMs = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected!");

    // FIX v9.2: respect user OFF intent during WiFi outage
    if (!lightState && !userForcedOff) {
      Serial.println("[FAIL-SAFE] WiFi down → forcing light ON");
      forceLight(true);
    }

    WiFi.disconnect();
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) {
      delay(500); Serial.print(".");
      esp_task_wdt_reset();
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED)
      Serial.println("[WiFi] Reconnected: " + WiFi.localIP().toString());
    else
      Serial.println("[WiFi] Still down — will retry");
  }
}

// ============================================================
//  WEB SERVER
// ============================================================
void setupWebServer() {

  server.on("/", HTTP_GET, []() {
    if (apMode) {
      String page =
        "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'/>"
        "<style>"
        "*{box-sizing:border-box}"
        "body{font-family:sans-serif;background:#06090d;color:#dde4ee;"
        "display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0}"
        ".c{background:#0e151e;border:1px solid #182030;border-radius:16px;padding:32px;max-width:380px;width:100%}"
        "h2{margin-bottom:20px;color:#f5c800;letter-spacing:3px}"
        "label{font-size:12px;color:#4a6070;display:block;margin-bottom:4px;margin-top:12px}"
        "input{width:100%;padding:10px;background:#0b1018;border:1px solid #182030;"
        "border-radius:8px;font-size:14px;color:#dde4ee}"
        "button{width:100%;margin-top:20px;padding:12px;background:#f5c800;"
        "color:#06090d;border:none;border-radius:8px;font-size:14px;font-weight:700;cursor:pointer}"
        ".note{margin-top:14px;font-size:11px;color:#334455;text-align:center}"
        "</style></head><body><div class='c'>"
        "<h2>AIPL SETUP</h2>"
        "<form action='/save' method='POST'>"
        "<label>WiFi SSID</label>"
        "<input type='text' name='ssid' required placeholder='Network name'/>"
        "<label>Password</label>"
        "<input type='password' name='password' placeholder='WiFi password'/>"
        "<button type='submit'>Save &amp; Connect</button>"
        "</form>"
        "<p class='note'>Row " + String(ROW_INDEX+1) +
        " &middot; Light " + String(LIGHT_INDEX+1) +
        " &middot; " + String(FIRMWARE_VERSION) + "</p>"
        "</div></body></html>";
      server.send(200, "text/html", page);
    } else {
      server.send(200, "application/json", getStatusJson());
    }
  });

  server.on("/save", HTTP_POST, []() {
    prefs.begin("wifi", false);
    prefs.putString("ssid",     server.arg("ssid"));
    prefs.putString("password", server.arg("password"));
    prefs.end();
    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;text-align:center;"
      "padding:40px;background:#06090d;color:#f5c800'>"
      "<h2>&#10003; Saved! Restarting...</h2></body></html>");
    delay(2000);
    ESP.restart();
  });

  server.on("/api/status", HTTP_GET, []() {
    server.send(200, "application/json", getStatusJson());
  });

  server.on("/api/set", HTTP_POST, []() {
    if (apMode) { server.send(403, "application/json", "{\"error\":\"AP mode\"}"); return; }
    bool desired = (server.arg("state") == "1" || server.arg("state") == "true");
    setLightState(desired);
    server.send(200, "application/json", getStatusJson());
  });

  server.on("/on",  HTTP_GET, []() { setLightState(true);  server.send(200, "text/plain", "Light ON");  });
  server.on("/off", HTTP_GET, []() { setLightState(false); server.send(200, "text/plain", "Light OFF"); });

  server.on("/reset", HTTP_GET, []() {
    prefs.begin("wifi", false); prefs.clear(); prefs.end();
    server.send(200, "text/plain", "Cleared. Restarting...");
    delay(1000); ESP.restart();
  });
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

  Serial.println("\n╔═══════════════════════════════════════════════╗");
  Serial.printf ("║  AIPL High Bay %s  Row%d Light%d             ║\n",
                  FIRMWARE_VERSION, ROW_INDEX+1, LIGHT_INDEX+1);
  Serial.println("║  Relay : JQC3F-05VDC-C  (ACTIVE LOW)          ║");
  Serial.println("║  Fail-safe : ON when WiFi/MQTT drops           ║");
  Serial.println("║  FIX v9.2 : Restores last user state on boot   ║");
  Serial.println("╚═══════════════════════════════════════════════╝\n");

  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  sessionStartMs = millis();

  pinMode(LIGHT_PIN, OUTPUT);

  // ── FIX v9.2: Read last saved state from flash ──────────────
  // OLD (broken): always booted ON regardless of what user set
  //   digitalWrite(LIGHT_PIN, RELAY_ON);
  //   lightState = true;
  //
  // NEW (fixed): restore exactly what user last commanded
  lightState    = loadLightState();
  userForcedOff = !lightState;

  digitalWrite(LIGHT_PIN, lightState ? RELAY_ON : RELAY_OFF);
  if (lightState) lightOnStart = millis();

  Serial.printf("[BOOT] Restored from flash → Light %s  GPIO%d=%s\n",
                lightState ? "ON"  : "OFF",
                LIGHT_PIN,
                lightState ? "LOW(RELAY_ON)" : "HIGH(RELAY_OFF)");

  totalOnSeconds = loadOnTime();

  // ── Load WiFi credentials ───────────────────────────────────
  prefs.begin("wifi", false);
  savedSSID = prefs.getString("ssid", WIFI_SSID);
  savedPass = prefs.getString("password", WIFI_PASSWORD);
  prefs.end();

  // ── Connect WiFi ────────────────────────────────────────────
  if (savedSSID.length() > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.persistent(true);
    WiFi.setAutoReconnect(true);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    Serial.print("[WiFi] Connecting to: " + savedSSID);

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
      Serial.println("[WiFi] Failed — AP mode");
      startAPMode();
    }
  } else {
    Serial.println("[WiFi] No credentials — AP mode");
    startAPMode();
  }

  setupWebServer();
  server.begin();
  Serial.println("[HTTP] Server ready on port 80");
  Serial.println("[READY] Waiting for MQTT commands.\n");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  esp_task_wdt_reset();
  server.handleClient();
  checkWiFiHealth();

  if (!apMode) {
    if (!mqtt.connected()) {
      mqttReconnect();
    } else {
      mqtt.loop();
    }

    if (millis() - lastTelemetryMs >= TELE_INTERVAL) {
      lastTelemetryMs = millis();
      publishTelemetry();
    }
  }
}