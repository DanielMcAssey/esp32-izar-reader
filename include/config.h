#pragma once

// ---------------------------------------------------------------------------
//  Hardware configuration - ESP32 + CC1101 wM-Bus Izar water meter reader
// ---------------------------------------------------------------------------
//  Credentials (WiFi / MQTT) and the meter id live in secrets.h so they are
//  kept out of version control. Copy secrets.h.example to secrets.h and fill
//  in your values.
// ---------------------------------------------------------------------------

#include "secrets.h"

// --- CC1101 wiring (matches the zibous/ha-watermeter ESP32 pinout) ---
//  CC1101 is a 3.3V device - do NOT power it from 5V.
#define CC1101_MOSI 23
#define CC1101_MISO 19
#define CC1101_SCK  18
#define CC1101_CS   21
#define CC1101_GDO0 16
#define CC1101_GDO2 17

// --- Radio ---
//  868.95 MHz, wM-Bus T1 mode (Diehl IZAR RC 868).
#define WMBUS_FREQ 868.950f

// --- MQTT topics (Home Assistant) ---
#define MQTT_CLIENT_ID   "izar-watermeter"
#define MQTT_STATE_TOPIC "watermeter/izar/state"
#define MQTT_AVAIL_TOPIC "watermeter/izar/status"

// --- Home Assistant device (groups all entities under one device) ---
#define HA_DEVICE_ID    "izar_watermeter"
#define HA_DEVICE_NAME  "Izar Water Meter"
#define HA_MANUFACTURER "Diehl"
#define HA_MODEL        "IZAR RC 868"

// --- OTA (over-the-air firmware updates) ---
//  Hostname the device advertises for OTA / mDNS. OTA_PASSWORD lives in
//  secrets.h. Flash wirelessly with:  pio run -t upload --upload-port <ip|host>
#define OTA_HOSTNAME "izar-watermeter"

// --- Resilience / reconnection ---
//  How often (ms) to run the connectivity maintenance check.
#define NET_CHECK_INTERVAL_MS 2000UL
//  If WiFi has been down this long (ms), force a fresh WiFi.begin() in case the
//  core's auto-reconnect gave up (e.g. AP was offline for a while).
#define WIFI_REBEGIN_MS 20000UL
//  Minimum gap (ms) between MQTT (re)connection attempts, so a down broker
//  doesn't get hammered and the RX loop isn't stalled every iteration.
#define MQTT_RETRY_INTERVAL_MS 10000UL
//  Max seconds a single MQTT socket operation may block. Kept short so the
//  CC1101 receive loop is never stalled for long when the broker is unreachable.
#define MQTT_SOCKET_TIMEOUT_S 2
//  MQTT keepalive (seconds).
#define MQTT_KEEPALIVE_S 15
