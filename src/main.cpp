#include <Arduino.h>
#include <vector>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include <rf_mbus.hpp>

#include "config.h"
#include "izar_decoder.h"

// ---------------------------------------------------------------------------
//  ESP32 + CC1101 wM-Bus reader for a Diehl IZAR RC 868 water meter.
//  Receives T1 telegrams, descrambles the IZAR payload, publishes to MQTT.
//
//  Networking is fully non-blocking and self-healing:
//    - WiFi auto-reconnects (core auto-reconnect + a periodic forced re-begin
//      as a fallback if the AP was down long enough for the core to give up).
//    - MQTT reconnects on a bounded backoff with a short socket timeout so a
//      missing broker never stalls the timing-sensitive CC1101 receive loop.
//    - No WiFi simply means MQTT is skipped (no error/crash); readings are
//      still decoded and logged over serial.
//    - ArduinoOTA lets you flash wirelessly once it is on the network.
// ---------------------------------------------------------------------------

static rf_mbus     mbus;
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static unsigned long lastNetCheck    = 0;
static unsigned long lastWifiBegin    = 0;
static unsigned long lastMqttAttempt  = 0;
static bool          wifiWasConnected = false;
static bool          otaStarted       = false;

// --- OTA --------------------------------------------------------------------

static void startOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("[ota] update starting"); });
  ArduinoOTA.onEnd([]() { Serial.println("\n[ota] update complete"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[ota] %u%%\r", (t ? (p * 100u / t) : 0));
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
  ArduinoOTA.begin();
  otaStarted = true;
  Serial.printf("[ota] ready as '%s'\n", OTA_HOSTNAME);
}

// --- helpers ---------------------------------------------------------------

#if METER_ID == 0
// Only used in bring-up mode; guarded to avoid an unused-function warning.
static void dumpHex(const char *label, const std::vector<uint8_t> &data) {
  Serial.printf("%s (%u bytes): ", label, (unsigned)data.size());
  for (uint8_t b : data) {
    Serial.printf("%02X ", b);
  }
  Serial.println();
}
#endif

// Non-blocking WiFi maintenance. Relies on the core's auto-reconnect, and as a
// fallback forces a fresh WiFi.begin() if the link has been down too long.
static void maintainWifi() {
  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    if (!wifiWasConnected) {
      Serial.printf("[wifi] connected, ip=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      wifiWasConnected = true;
      if (!otaStarted) {
        startOTA();  // OTA/mDNS needs an active connection
      }
    }
    return;
  }

  // Link is down.
  if (wifiWasConnected) {
    Serial.println("[wifi] connection lost");
    wifiWasConnected = false;
  }

  // If down for a while, force a re-begin (covers AP restarts the core's
  // auto-reconnect has stopped retrying).
  unsigned long now = millis();
  if (now - lastWifiBegin >= WIFI_REBEGIN_MS) {
    Serial.println("[wifi] forcing reconnect...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiBegin = now;
  }
}

// --- Home Assistant MQTT discovery -----------------------------------------

// Common fields shared by every entity: predictable ids, the state/availability
// topics, and the device block so all entities group under one HA device.
static void addHaCommon(JsonDocument &doc, const char *objectId) {
  char uid[64];
  snprintf(uid, sizeof(uid), "%s_%s", HA_DEVICE_ID, objectId);
  doc["unique_id"]          = uid;  // char[] -> ArduinoJson copies into the doc
  doc["object_id"]          = uid;  // predictable entity_id
  doc["state_topic"]        = MQTT_STATE_TOPIC;
  doc["availability_topic"] = MQTT_AVAIL_TOPIC;

  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"].to<JsonArray>().add(HA_DEVICE_ID);
  dev["name"]         = HA_DEVICE_NAME;
  dev["manufacturer"] = HA_MANUFACTURER;
  dev["model"]        = HA_MODEL;
}

// Serialize `doc` and publish it retained to the HA discovery config topic for
// the given platform ("sensor" / "binary_sensor").
static void publishConfig(const char *platform, const char *objectId, JsonDocument &doc) {
  char topic[128];
  snprintf(topic, sizeof(topic), "homeassistant/%s/%s/%s/config",
           platform, HA_DEVICE_ID, objectId);
  char payload[640];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  mqtt.publish(topic, (const uint8_t *)payload, n, true);
}

// Pass nullptr for unit/deviceClass/stateClass to omit them.
static void publishSensorDiscovery(const char *objectId, const char *name,
                                   const char *valueTemplate, const char *unit,
                                   const char *deviceClass, const char *stateClass,
                                   bool diagnostic) {
  JsonDocument doc;
  addHaCommon(doc, objectId);
  doc["name"]           = name;
  doc["value_template"] = valueTemplate;
  if (unit)        doc["unit_of_measurement"] = unit;
  if (deviceClass) doc["device_class"]        = deviceClass;
  if (stateClass)  doc["state_class"]         = stateClass;
  if (diagnostic)  doc["entity_category"]     = "diagnostic";
  publishConfig("sensor", objectId, doc);
}

static void publishBinarySensorDiscovery(const char *objectId, const char *name,
                                         const char *valueTemplate,
                                         const char *deviceClass, bool diagnostic) {
  JsonDocument doc;
  addHaCommon(doc, objectId);
  doc["name"]           = name;
  doc["value_template"] = valueTemplate;
  doc["payload_on"]     = "ON";
  doc["payload_off"]    = "OFF";
  if (deviceClass) doc["device_class"]    = deviceClass;
  if (diagnostic)  doc["entity_category"] = "diagnostic";
  publishConfig("binary_sensor", objectId, doc);
}

static void publishDiscovery() {
  publishSensorDiscovery("total", "Water meter total",
                         "{{ value_json.total_m3 }}", "m³", "water",
                         "total_increasing", false);
  publishSensorDiscovery("last_month_total", "Water meter last month total",
                         "{{ value_json.last_month_m3 }}", "m³", "water",
                         "total_increasing", false);
  publishSensorDiscovery("last_month_date", "Water meter last month date",
                         "{{ value_json.last_month_date }}", nullptr, "date",
                         nullptr, true);
  publishSensorDiscovery("battery_life", "Water meter battery life",
                         "{{ value_json.battery_years }}", "y", nullptr,
                         "measurement", true);
  publishSensorDiscovery("transmit_period", "Water meter transmit period",
                         "{{ value_json.transmit_period_s }}", "s", "duration",
                         "measurement", true);
  publishSensorDiscovery("current_alarms", "Water meter current alarms",
                         "{{ value_json.current_alarms }}", nullptr, nullptr,
                         nullptr, true);
  publishSensorDiscovery("previous_alarms", "Water meter previous alarms",
                         "{{ value_json.previous_alarms }}", nullptr, nullptr,
                         nullptr, true);
  publishBinarySensorDiscovery("general_alarm", "Water meter general alarm",
                               "{{ value_json.general_alarm }}", "problem", true);
  publishSensorDiscovery("rssi", "Water meter RSSI",
                         "{{ value_json.rssi }}", "dBm", "signal_strength",
                         "measurement", true);
}

// --- MQTT connection / publishing ------------------------------------------

// Non-blocking MQTT maintenance with bounded retry backoff.
static void maintainMqtt() {
  if (mqtt.connected()) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;  // no point trying without WiFi (and never blocks/errors)
  }

  unsigned long now = millis();
  if (now - lastMqttAttempt < MQTT_RETRY_INTERVAL_MS) {
    return;
  }
  lastMqttAttempt = now;

  Serial.printf("[mqtt] connecting to %s:%d ...\n", MQTT_HOST, (int)MQTT_PORT);
  // Last-will marks the device offline in HA if we drop unexpectedly.
  if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                   MQTT_AVAIL_TOPIC, 0, true, "offline")) {
    Serial.println("[mqtt] connected");
    mqtt.publish(MQTT_AVAIL_TOPIC, "online", true);
    publishDiscovery();
  } else {
    Serial.printf("[mqtt] connect failed, rc=%d (retry in %lus)\n",
                  mqtt.state(), MQTT_RETRY_INTERVAL_MS / 1000);
  }
}

static void publishReading(const IzarReading &r, int8_t rssi) {
  if (!mqtt.connected()) {
    Serial.println("[mqtt] offline - reading not published (will resend on next telegram)");
    return;
  }

  JsonDocument doc;
  doc["meter_id"]          = r.meterId;
  doc["total_m3"]          = r.m3;
  doc["total_l"]           = r.liters;
  doc["last_month_m3"]     = r.lastMonthM3;
  doc["last_month_l"]      = r.lastMonthLiters;
  doc["last_month_date"]   = r.lastMonthDate;
  doc["battery_years"]     = r.batteryYears;
  doc["transmit_period_s"] = r.transmitPeriodS;
  doc["current_alarms"]    = r.currentAlarms;
  doc["previous_alarms"]   = r.previousAlarms;
  doc["general_alarm"]     = r.generalAlarm ? "ON" : "OFF";
  doc["rssi"]              = rssi;

  char payload[512];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (mqtt.publish(MQTT_STATE_TOPIC, (const uint8_t *)payload, n, false)) {
    Serial.printf("[mqtt] published: %s\n", payload);
  } else {
    Serial.println("[mqtt] publish failed");
  }
}

// --- Arduino entry points --------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[izar] ESP32 + CC1101 wM-Bus water meter reader");

  // WiFi: station mode, no flash wear, and let the core auto-reconnect.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // keep the radio responsive
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiBegin = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(768);                       // room for HA discovery payloads
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);  // bound any blocking op

  Serial.printf("[cc1101] init (MOSI=%d MISO=%d SCK=%d CS=%d GDO0=%d GDO2=%d @ %.3f MHz)\n",
                CC1101_MOSI, CC1101_MISO, CC1101_SCK, CC1101_CS,
                CC1101_GDO0, CC1101_GDO2, WMBUS_FREQ);
  if (mbus.init(CC1101_MOSI, CC1101_MISO, CC1101_SCK, CC1101_CS,
                CC1101_GDO0, CC1101_GDO2, WMBUS_FREQ)) {
    Serial.println("[cc1101] init OK, listening for T1 telegrams");
  } else {
    Serial.println("[cc1101] init FAILED - check wiring (CS/GDO0/GDO2) and 3.3V power");
  }

#if METER_ID == 0
  Serial.println("[izar] METER_ID=0: logging every meter id seen (bring-up mode)");
#endif
}

void loop() {
  // Keep the receive loop tight - CC1101 RX is timing sensitive.
  if (mbus.task()) {
    WMbusFrame frame = mbus.get_frame();

    if (frame.framemode == WMBUS_T1_MODE) {
      IzarReading r = izarDecode(frame.frame);

#if METER_ID == 0
      Serial.printf("[rx] mode=T1 rssi=%d lqi=%u meter_id=%u\n",
                    (int)frame.rssi, (unsigned)frame.lqi, (unsigned)r.meterId);
      dumpHex("[rx] frame", frame.frame);
#endif

      if (r.valid && (METER_ID == 0 || r.meterId == (uint32_t)METER_ID)) {
        Serial.printf("[izar] meter=%u total=%.3f m3 (%u L) batt=%.1fy period=%us alarms=%s\n",
                      (unsigned)r.meterId, r.m3, (unsigned)r.liters,
                      r.batteryYears, (unsigned)r.transmitPeriodS, r.currentAlarms);
        if (METER_ID != 0) {
          publishReading(r, frame.rssi);
        }
      }
    }
  }

  // Periodic, non-blocking connectivity maintenance (kept off the hot RX path).
  unsigned long now = millis();
  if (now - lastNetCheck >= NET_CHECK_INTERVAL_MS) {
    lastNetCheck = now;
    maintainWifi();
    maintainMqtt();
  }
  if (otaStarted) {
    ArduinoOTA.handle();
  }
  mqtt.loop();
}
