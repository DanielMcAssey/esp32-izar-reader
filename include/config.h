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
#define CC1101_CS   5
#define CC1101_GDO0 16
#define CC1101_GDO2 17

// --- Radio ---
//  868.95 MHz, wM-Bus T1 mode (Diehl IZAR RC 868).
#define WMBUS_FREQ 868.950f

// --- MQTT topics (Home Assistant) ---
#define MQTT_CLIENT_ID   "izar-watermeter"
#define MQTT_STATE_TOPIC "watermeter/izar/state"
#define MQTT_AVAIL_TOPIC "watermeter/izar/status"

//  Home Assistant publishes "online" here on (re)start; we answer by resending
//  discovery + the last reading, since HA forgets non-retained state.
#define HA_STATUS_TOPIC "homeassistant/status"
//  PubSubClient packet buffer. publish() fails silently if a packet exceeds it,
//  so the discovery payload buffer below must (and is static_assert'ed to) fit.
#define MQTT_BUFFER_SIZE       4096
#define MQTT_DISCOVERY_MAX_LEN 3584
#define MQTT_STATE_MAX_LEN     512

// --- Firmware version (shown as sw_version in HA; changes on every build) ---
#ifndef FW_VERSION  // CI release builds override this with the git tag
#define FW_VERSION __DATE__ " " __TIME__
#endif

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
//  WiFi forced re-begin backs off exponentially from WIFI_REBEGIN_MS up to this
//  cap, so a slow association/DHCP is not repeatedly interrupted.
#define WIFI_REBEGIN_MAX_MS 300000UL

// --- Reading sanity / radio watchdog ---
//  Reject readings whose total rises by more than this many litres relative to
//  the last accepted one (or that go backwards) until 3 in a row agree.
#define IZAR_MAX_JUMP_L 10000UL
//  No valid reading for this long (ms): mark the device "offline" in HA so
//  stale data is visible. The next reading brings it back online.
#define STALE_OFFLINE_MS 1800000UL
//  No valid reading for this long (ms): assume the CC1101 wedged and reboot.
//  Only active when METER_ID != 0.
#define RADIO_STALL_RESTART_MS 21600000UL
//  Retry interval (ms) for a failed CC1101 init.
#define RADIO_RETRY_MS 30000UL

// --- Publishing ---
//  The meter transmits every few seconds, but only the total (and rssi) move.
//  Publish when the total changes, otherwise at most this often (ms), so the HA
//  recorder isn't flooded with an rssi row per telegram.
#define PUBLISH_MIN_INTERVAL_MS 60000UL
