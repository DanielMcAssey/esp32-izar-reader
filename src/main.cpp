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
//  Two cores, one job each:
//    - Arduino loop() (core 1) only polls the CC1101 and decodes. It never
//      blocks; accepted readings are handed to the network task via a queue.
//    - netTask (core 0) owns WiFi, MQTT and OTA. It may block (broker connect,
//      DNS) without ever stalling reception. PubSubClient is not thread-safe,
//      so *only* netTask touches `mqtt` / `wifiClient`.
//
//  Networking is self-healing:
//    - WiFi auto-reconnects (core auto-reconnect + a periodic forced re-begin,
//      with exponential backoff, if the core gave up after a long AP outage).
//    - MQTT reconnects on a bounded backoff with a short socket timeout.
//    - No WiFi simply means MQTT is skipped; readings are still logged.
//    - HA restarts are handled: discovery + last reading are resent when HA
//      announces "online" on homeassistant/status.
//    - Radio watchdog: CC1101 init is retried; a long silence marks the device
//      offline in HA and, eventually, reboots the board.
// ---------------------------------------------------------------------------

static_assert(MQTT_DISCOVERY_MAX_LEN + 128 <= MQTT_BUFFER_SIZE,
              "discovery payload (+ MQTT header/topic) must fit the PubSubClient buffer");

struct QueuedReading {
  IzarReading reading;
  int8_t      rssi;
};

static rf_mbus     mbus;
static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);

static QueueHandle_t readingQueue = nullptr;

// Written by loop() (core 1), read by netTask (core 0). Aligned 32-bit access.
static volatile uint32_t lastValidRxMs = 0;

// netTask-only state.
static unsigned long lastNetCheck    = 0;
static unsigned long lastWifiBegin   = 0;
static unsigned long wifiRebeginMs   = WIFI_REBEGIN_MS;
static unsigned long lastMqttAttempt = 0;
static bool          wifiWasConnected = false;
static bool          otaStarted      = false;
static bool          availOnline     = false;
static volatile bool haBirthSeen     = false;  // set from the MQTT callback
static QueuedReading latest;                 // newest queued reading
static bool          latestPending   = false;  // latest not yet published/discarded
static bool          havePublished   = false;
static uint32_t      lastPublishedL  = 0;
static unsigned long lastPublishMs   = 0;
static char          lastPayload[MQTT_STATE_MAX_LEN] = "";
static size_t        lastPayloadLen  = 0;

// loop()-only state.
static bool             radioOk          = false;
static unsigned long    lastRadioAttempt = 0;
static IzarPlausibility plausibility(IZAR_MAX_JUMP_L);


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

// WiFi maintenance. Relies on the core's auto-reconnect, and as a fallback
// forces a fresh WiFi.begin() if the link has been down too long.
static void maintainWifi() {
  wl_status_t st = WiFi.status();

  if (st == WL_CONNECTED) {
    if (!wifiWasConnected) {
      Serial.printf("[wifi] connected, ip=%s rssi=%d\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      wifiWasConnected = true;
      wifiRebeginMs    = WIFI_REBEGIN_MS;
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
  // auto-reconnect has stopped retrying). The interval doubles up to
  // WIFI_REBEGIN_MAX_MS so a slow association/DHCP isn't interrupted.
  unsigned long now = millis();
  if (now - lastWifiBegin >= wifiRebeginMs) {
    wifiRebeginMs = min(wifiRebeginMs * 2, (unsigned long)WIFI_REBEGIN_MAX_MS);
    Serial.printf("[wifi] forcing reconnect (next in %lus)...\n", wifiRebeginMs / 1000);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWifiBegin = now;
  }
}

// --- Home Assistant MQTT device discovery ----------------------------------
// A single retained config on homeassistant/device/<id>/config describes the
// whole device and every entity ("component") at once, instead of one config
// topic per entity. Shared keys (state/availability topics) live at the top
// level; each component carries its own platform + unique_id. See:
//   https://www.home-assistant.io/integrations/mqtt/#device-based-discovery

// Create the parts every platform shares: platform, ids, name and template.
// The caller adds the platform-specific keys.
static JsonObject addComponent(JsonObject cmps, const char *platform, const char *objectId,
                               const char *name, const char *valueTemplate,
                               const char *deviceClass, bool diagnostic) {
  char uid[64];
  snprintf(uid, sizeof(uid), "%s_%s", HA_DEVICE_ID, objectId);
  JsonObject c = cmps[objectId].to<JsonObject>();
  c["platform"]       = platform;  // required for each device-discovery component
  c["unique_id"]      = uid;       // char[] -> ArduinoJson copies into the doc
  c["object_id"]      = uid;       // predictable entity_id
  c["name"]           = name;
  c["value_template"] = valueTemplate;
  if (deviceClass) c["device_class"]    = deviceClass;
  if (diagnostic)  c["entity_category"] = "diagnostic";
  return c;
}

// Pass nullptr for unit/deviceClass/stateClass to omit them.
static void addSensor(JsonObject cmps, const char *objectId, const char *name,
                      const char *valueTemplate, const char *unit,
                      const char *deviceClass, const char *stateClass,
                      bool diagnostic) {
  JsonObject c = addComponent(cmps, "sensor", objectId, name, valueTemplate,
                              deviceClass, diagnostic);
  if (unit)       c["unit_of_measurement"] = unit;
  if (stateClass) c["state_class"]         = stateClass;
}

static void addBinarySensor(JsonObject cmps, const char *objectId, const char *name,
                            const char *valueTemplate, const char *deviceClass,
                            bool diagnostic) {
  JsonObject c = addComponent(cmps, "binary_sensor", objectId, name, valueTemplate,
                              deviceClass, diagnostic);
  c["payload_on"]  = "ON";
  c["payload_off"] = "OFF";
}

static void publishDiscovery() {
  JsonDocument doc;

  // Device block: groups all entities under one HA device.
  JsonObject dev = doc["device"].to<JsonObject>();
  dev["identifiers"].to<JsonArray>().add(HA_DEVICE_ID);
  dev["name"]         = HA_DEVICE_NAME;
  dev["manufacturer"] = HA_MANUFACTURER;
  dev["model"]        = HA_MODEL;
  dev["sw_version"]   = FW_VERSION;

  // Origin block is mandatory for device-based discovery.
  doc["origin"]["name"]       = MQTT_CLIENT_ID;
  doc["origin"]["sw_version"] = FW_VERSION;

  // Shared by every component below.
  doc["state_topic"]        = MQTT_STATE_TOPIC;
  doc["availability_topic"] = MQTT_AVAIL_TOPIC;

  JsonObject cmps = doc["components"].to<JsonObject>();
  addSensor(cmps, "total", "Total",
            "{{ value_json.total_m3 }}", "m³", "water", "total_increasing", false);
  // A monthly snapshot, not a running total: no state_class, or HA's long-term
  // statistics would count each monthly change as consumption.
  addSensor(cmps, "last_month_total", "Last month total",
            "{{ value_json.last_month_m3 }}", "m³", "water", nullptr, false);
  addSensor(cmps, "last_month_date", "Last month date",
            "{{ value_json.last_month_date }}", nullptr, "date", nullptr, true);
  addSensor(cmps, "battery_life", "Battery life",
            "{{ value_json.battery_years }}", "y", nullptr, "measurement", true);
  addSensor(cmps, "transmit_period", "Transmit period",
            "{{ value_json.transmit_period_s }}", "s", "duration", "measurement", true);
  addSensor(cmps, "current_alarms", "Current alarms",
            "{{ value_json.current_alarms }}", nullptr, nullptr, nullptr, true);
  addSensor(cmps, "previous_alarms", "Previous alarms",
            "{{ value_json.previous_alarms }}", nullptr, nullptr, nullptr, true);
  addBinarySensor(cmps, "general_alarm", "General alarm",
                  "{{ value_json.general_alarm }}", "problem", true);
  addSensor(cmps, "rssi", "RSSI",
            "{{ value_json.rssi }}", "dBm", "signal_strength", "measurement", true);

  char topic[96];
  snprintf(topic, sizeof(topic), "homeassistant/device/%s/config", HA_DEVICE_ID);

  static char payload[MQTT_DISCOVERY_MAX_LEN];
  const size_t needed = measureJson(doc);
  if (needed >= sizeof(payload)) {  // serializeJson would silently truncate
    Serial.printf("[mqtt] discovery too large (%u >= %u) - raise MQTT_DISCOVERY_MAX_LEN\n",
                  (unsigned)needed, (unsigned)sizeof(payload));
    return;
  }
  size_t n = serializeJson(doc, payload, sizeof(payload));
  bool ok = mqtt.publish(topic, (const uint8_t *)payload, n, true);
  Serial.printf("[mqtt] discovery %s (%u bytes)\n",
                ok ? "published" : "FAILED (raise MQTT_BUFFER_SIZE?)", (unsigned)n);
}

// --- MQTT connection / publishing ------------------------------------------

static void publishAvailability(bool online) {
  if (mqtt.publish(MQTT_AVAIL_TOPIC, online ? "online" : "offline", true)) {
    availOnline = online;
  }
}

// HA announces "online" when it (re)starts; it has forgotten our non-retained
// state by then, so resend discovery, availability and the last reading.
static void onMqttMessage(char *topic, uint8_t *payload, unsigned int len) {
  (void)topic;
  if (len == 6 && memcmp(payload, "online", 6) == 0) {
    haBirthSeen = true;  // handled in netTask, not inside the PubSubClient callback
  }
}

static void resendAfterHaRestart() {
  Serial.println("[mqtt] HA restarted - resending discovery");
  publishAvailability(availOnline);
  publishDiscovery();
  if (lastPayloadLen > 0) {
    mqtt.publish(MQTT_STATE_TOPIC, (const uint8_t *)lastPayload, lastPayloadLen, false);
  }
}

// MQTT maintenance with bounded retry backoff. May block briefly (connect);
// that is fine here because it runs on netTask, not the RX loop.
static void maintainMqtt() {
  if (mqtt.connected()) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;  // no point trying without WiFi
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
    mqtt.subscribe(HA_STATUS_TOPIC);
    publishAvailability(true);
    publishDiscovery();
  } else {
    availOnline = false;
    Serial.printf("[mqtt] connect failed, rc=%d (retry in %lus)\n",
                  mqtt.state(), MQTT_RETRY_INTERVAL_MS / 1000);
  }
}

// No valid reading for a long time: flag the device offline so HA shows the
// data as stale instead of presenting the last value as current.
static void maintainAvailability() {
  if (METER_ID == 0 || !mqtt.connected() || !availOnline) {
    return;
  }
  if (millis() - lastValidRxMs > STALE_OFFLINE_MS) {
    Serial.println("[mqtt] no valid reading for a long time - marking offline");
    publishAvailability(false);
  }
}

// Publish one reading. Returns true if it went out.
static bool publishReading(const IzarReading &r, int8_t rssi) {
  if (!mqtt.connected()) {
    return false;
  }

  JsonDocument doc;
  doc["meter_id"] = r.meterId;
  doc["total_m3"] = r.m3;
  doc["total_l"]  = r.liters;
  if (r.hasLastMonth) {
    doc["last_month_m3"] = r.lastMonthM3;
    doc["last_month_l"]  = r.lastMonthLiters;
  } else {
    doc["last_month_m3"] = nullptr;  // renders as "None" -> unknown in HA, not a bogus 0
    doc["last_month_l"]  = nullptr;
  }
  if (r.lastMonthDate[0]) {
    doc["last_month_date"] = r.lastMonthDate;
  } else {
    doc["last_month_date"] = nullptr;
  }
  doc["battery_years"]     = r.batteryYears;
  doc["transmit_period_s"] = r.transmitPeriodS;
  doc["current_alarms"]    = r.currentAlarms;
  doc["previous_alarms"]   = r.previousAlarms;
  doc["general_alarm"]     = r.generalAlarm ? "ON" : "OFF";
  doc["rssi"]              = rssi;

  if (measureJson(doc) >= sizeof(lastPayload)) {  // serializeJson would silently truncate
    Serial.println("[mqtt] state payload too large - raise MQTT_STATE_MAX_LEN");
    return false;
  }
  lastPayloadLen = serializeJson(doc, lastPayload, sizeof(lastPayload));

  if (!availOnline) {
    publishAvailability(true);  // recover from a stale-offline period
  }
  if (mqtt.publish(MQTT_STATE_TOPIC, (const uint8_t *)lastPayload, lastPayloadLen, false)) {
    Serial.printf("[mqtt] published: %s\n", lastPayload);
    return true;
  }
  Serial.println("[mqtt] publish failed");
  return false;
}

// Publish the newest reading when the total changed or PUBLISH_MIN_INTERVAL_MS
// has passed; otherwise discard it. While MQTT is down the reading is kept, so
// it goes out right after (re)connect. Only a successful publish updates the
// rate-limit state, so a dropped reading never delays the next one.
static void publishLatestIfDue() {
  if (!latestPending || !mqtt.connected()) {
    return;
  }
  latestPending = false;

  const bool due = !havePublished || latest.reading.liters != lastPublishedL ||
                   millis() - lastPublishMs >= PUBLISH_MIN_INTERVAL_MS;
  if (due && publishReading(latest.reading, latest.rssi)) {
    havePublished  = true;
    lastPublishedL = latest.reading.liters;
    lastPublishMs  = millis();
  }
}

// --- Network task (core 0) ---------------------------------------------------

static void netTask(void *) {
  // WiFi: station mode, no flash wear, and let the core auto-reconnect.
  WiFi.persistent(false);
  WiFi.setHostname(OTA_HOSTNAME);  // shows up by name in the router's DHCP list
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);  // keep the radio responsive
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWifiBegin = millis();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);          // room for the combined HA discovery payload
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);
  mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);  // bound any blocking op
  mqtt.setCallback(onMqttMessage);

  for (;;) {
    unsigned long now = millis();
    if (now - lastNetCheck >= NET_CHECK_INTERVAL_MS) {
      lastNetCheck = now;
      maintainWifi();
      maintainMqtt();
      maintainAvailability();
    }
    if (otaStarted) {
      ArduinoOTA.handle();
    }
    mqtt.loop();

    if (haBirthSeen && mqtt.connected()) {
      haBirthSeen = false;
      resendAfterHaRestart();
    }

    QueuedReading item;
    while (xQueueReceive(readingQueue, &item, 0) == pdTRUE) {
      latest        = item;
      latestPending = true;
    }
    publishLatestIfDue();

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// --- Radio -------------------------------------------------------------------

static bool initRadio() {
  Serial.printf("[cc1101] init (MOSI=%d MISO=%d SCK=%d CS=%d GDO0=%d GDO2=%d @ %.3f MHz)\n",
                CC1101_MOSI, CC1101_MISO, CC1101_SCK, CC1101_CS,
                CC1101_GDO0, CC1101_GDO2, WMBUS_FREQ);
  if (mbus.init(CC1101_MOSI, CC1101_MISO, CC1101_SCK, CC1101_CS,
                CC1101_GDO0, CC1101_GDO2, WMBUS_FREQ)) {
    Serial.println("[cc1101] init OK, listening for T1 telegrams");
    return true;
  }
  Serial.printf("[cc1101] init FAILED - check wiring (CS/GDO0/GDO2) and 3.3V power; retry in %lus\n",
                RADIO_RETRY_MS / 1000);
  return false;
}

// --- Arduino entry points --------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[izar] ESP32 + CC1101 wM-Bus water meter reader (%s)\n", FW_VERSION);

  // wMbus-lib busy-waits on the CC1101 state with no timeout; if the radio ever
  // stops answering, loop() would hang forever. Let the task watchdog reboot us.
  enableLoopWDT();

  lastValidRxMs = millis();
  readingQueue  = xQueueCreate(4, sizeof(QueuedReading));

  radioOk          = initRadio();
  lastRadioAttempt = millis();

  // Networking runs on core 0 so a slow broker/DNS never stalls the RX loop.
  xTaskCreatePinnedToCore(netTask, "net", 10240, nullptr, 1, nullptr, 0);

#if METER_ID == 0
  Serial.println("[izar] METER_ID=0: logging every meter id seen (bring-up mode)");
#endif
}

void loop() {
  unsigned long now = millis();

  if (!radioOk) {
    // Retry a failed init without ever blocking for long.
    if (now - lastRadioAttempt >= RADIO_RETRY_MS) {
      lastRadioAttempt = now;
      radioOk = initRadio();
    }
    return;
  }

  // Radio watchdog: a wedged CC1101 looks like permanent silence.
  if (METER_ID != 0 && now - lastValidRxMs > RADIO_STALL_RESTART_MS) {
    Serial.println("[izar] no valid reading for a long time - restarting");
    delay(100);
    ESP.restart();
  }

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
        if (METER_ID != 0 && !plausibility.accept(r.liters)) {
          Serial.printf("[izar] implausible total %u L - ignored\n", (unsigned)r.liters);
          return;
        }
        lastValidRxMs = millis();
        Serial.printf("[izar] meter=%u total=%.3f m3 (%u L) batt=%.1fy period=%us alarms=%s\n",
                      (unsigned)r.meterId, r.m3, (unsigned)r.liters,
                      r.batteryYears, (unsigned)r.transmitPeriodS, r.currentAlarms);
        // netTask applies the publish rate limit (see config.h).
        if (METER_ID != 0) {
          QueuedReading item{r, frame.rssi};
          if (xQueueSend(readingQueue, &item, 0) != pdTRUE) {
            Serial.println("[izar] publish queue full - reading dropped");
          }
        }
      }
    }
  }
}
