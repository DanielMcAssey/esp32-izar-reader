# ESP32 + CC1101 Izar Water Meter Reader

Reads a **Diehl IZAR RC 868** water meter over **wireless M-Bus (wM-Bus) T1 mode
(~868.95 MHz)** using an ESP32 and a CC1101 transceiver, and publishes the total
consumption to **Home Assistant via MQTT**.

This is a native [PlatformIO](https://platformio.org/) port of the ESPHome
project [zibous/ha-watermeter](https://github.com/zibous/ha-watermeter). It reuses
the same radio/link-layer stack ([SzczepanLeon/wMbus-lib](https://github.com/SzczepanLeon/wMbus-lib)
+ [SmartRC-CC1101-Driver-Lib](https://github.com/LSatan/SmartRC-CC1101-Driver-Lib))
and adds an IZAR payload descrambler ported from
[maciekn/izar-wmbus-esp](https://github.com/maciekn/izar-wmbus-esp) /
[wmbusmeters](https://github.com/wmbusmeters/wmbusmeters). IZAR uses a proprietary
descrambling algorithm, so **no encryption key is required**.

## Hardware

| ESP32 (DOIT DevKit v1) | CC1101 | Notes |
|---|---|---|
| GPIO23 | MOSI (SI) | |
| GPIO19 | MISO (SO) | |
| GPIO18 | SCK (CLK) | |
| GPIO5 | CS (CSN) | |
| GPIO16 | GDO0 | |
| GPIO17 | GDO2 | |
| 3V3 | VCC | **3.3 V only — 5 V will destroy the CC1101** |
| GND | GND | |

Add an **868 MHz antenna** (~8.6 cm of wire on the ANT pad works). Pins are
defined in [`include/config.h`](include/config.h).

## Libraries

Declared in [`platformio.ini`](platformio.ini) and installed automatically on
first build:

| Library | Purpose |
|---|---|
| `SzczepanLeon/wMbus-lib` | CC1101 + wM-Bus T1 reception / link layer |
| `lsatan/SmartRC-CC1101-Driver-Lib@^2.5.7` | CC1101 SPI driver |
| `knolleary/PubSubClient@^2.8` | MQTT client |
| `bblanchon/ArduinoJson@^7` | MQTT state + HA discovery payloads |

`WiFi` and `SPI` ship with the ESP32 Arduino core.

## Setup

1. **Credentials.** Copy the example secrets file and fill it in:
   ```bash
   cp include/secrets.h.example include/secrets.h
   ```
   Set your WiFi SSID/password, MQTT broker host/port/user/pass, and `METER_ID`.
   `include/secrets.h` is gitignored so your credentials are never committed.

2. **Bring-up (find your meter id).** Leave `METER_ID 0`, then build, flash and
   watch the serial monitor:
   ```bash
   pio run -t upload && pio device monitor
   ```
   With `METER_ID 0` the firmware logs **every** T1 telegram it hears, printing
   the meter id, RSSI and a raw hex dump, e.g.:
   ```
   [rx] mode=T1 rssi=-71 lqi=128 meter_id=21202341
   [rx] frame (39 bytes): 26 44 ...
   [izar] meter=21202341 total=123.456 m3 (123456 L) rssi=-71
   ```
   IZAR meters transmit roughly every 8–16 s. Confirm the `total` matches your
   physical meter dial, then note your `meter_id`.

3. **Lock to your meter.** Set `METER_ID` in `include/secrets.h` to your meter's
   id (decimal). Now the firmware filters to just your meter **and enables MQTT
   publishing** (publishing is intentionally disabled while `METER_ID == 0`).

4. **Flash the final build:**
   ```bash
   pio run -t upload
   ```

## Home Assistant

On MQTT connect the firmware publishes a single
[MQTT device-discovery](https://www.home-assistant.io/integrations/mqtt/#device-based-discovery)
config, so a `sensor.water_meter_total` (device_class `water`, state_class
`total_increasing`, unit `m³`) appears automatically — no YAML needed. It is
suitable for the HA **Energy dashboard** (water).

All entities are declared together under a single **Izar Water Meter** device
(Diehl / IZAR RC 868) in one retained `homeassistant/device/.../config` message
(each entity is a `component` with its own `platform`). Published entities:

| Entity | Source | Notes |
|---|---|---|
| Total (`m³`) | descrambled payload | `total_increasing`, Energy-dashboard ready |
| Last month total (`m³`) | descrambled payload | value around end of previous month |
| Last month date | descrambled payload | `date` device_class (`YYYY-MM-DD`), diagnostic |
| Battery life (`y`) | frame header | diagnostic |
| Transmit period (`s`) | frame header | diagnostic |
| Current alarms | frame header | `no_alarm` or e.g. `leakage,underflow` |
| Previous alarms | frame header | diagnostic |
| General alarm | frame header | `binary_sensor`, device_class `problem`, diagnostic |
| RSSI (`dBm`) | radio | diagnostic |

MQTT topics (configurable in `include/config.h`):

| Topic | Payload |
|---|---|
| `watermeter/izar/state` | JSON: `total_m3`, `total_l`, `last_month_m3`, `last_month_date`, `battery_years`, `transmit_period_s`, `current_alarms`, `previous_alarms`, `general_alarm`, `rssi`, `meter_id` |
| `watermeter/izar/status` | `online` / `offline` (retained availability / LWT) |
| `homeassistant/device/izar_watermeter/config` | HA device discovery, all entities (retained) |

## Over-the-air (OTA) updates

Once the device is on WiFi it advertises itself for OTA (hostname
`OTA_HOSTNAME`, password `OTA_PASSWORD` in `secrets.h`). Flash wirelessly with
the dedicated `ota` environment — no USB cable needed.

First export the OTA password (must match `OTA_PASSWORD` in `secrets.h`):

```bash
export IZAR_OTA_PASSWORD=your-password          # macOS/Linux
$env:IZAR_OTA_PASSWORD = "your-password"        # Windows PowerShell
```

Then upload:

```bash
pio run -e ota -t upload
```

The target host/IP is `upload_port` in `platformio.ini` (default
`izar-watermeter.local`); override per-run with `--upload-port 192.168.1.42`.

## Resilience

The networking is non-blocking and self-healing so it can run unattended:

- **WiFi auto-reconnect.** The ESP32 core's auto-reconnect is enabled, and a
  fallback forces a fresh `WiFi.begin()` if the link stays down beyond
  `WIFI_REBEGIN_MS` (default 20 s) — this covers access-point restarts where the
  core stops retrying. Reconnection never blocks the receive loop.
- **No WiFi ≠ error.** If WiFi is down, MQTT is simply skipped — no exception,
  no crash. Telegrams are still decoded and logged over serial, and MQTT resumes
  automatically once WiFi returns.
- **MQTT reconnect with backoff.** Reconnection attempts are throttled to
  `MQTT_RETRY_INTERVAL_MS` (default 10 s) and use a short socket timeout
  (`MQTT_SOCKET_TIMEOUT_S`, default 2 s), so an unreachable broker never stalls
  the timing-sensitive CC1101 RX loop for long.
- **Availability / LWT.** A retained last-will (`offline`) marks the device
  unavailable in HA if it drops unexpectedly; `online` is published on connect.
- **Readings are re-sent.** Each telegram (every ~8–16 s) publishes the latest
  total, so a dropped message self-corrects on the next transmission.

Tuning constants live in [`include/config.h`](include/config.h).

## Project layout

```
platformio.ini            PlatformIO config + library deps + cppcheck setup
.clang-format             Formatting style (clang-format -i src/* include/*)
include/config.h          Pins, frequency, MQTT/HA topics, OTA, resilience tuning
include/secrets.h.example Template for WiFi/MQTT/OTA credentials + METER_ID
include/secrets.h         Your real credentials (gitignored)
src/main.cpp              Radio RX loop, WiFi/MQTT/OTA, HA discovery
src/izar_decoder.{h,cpp}  IZAR descrambler + field extraction
```

## Developer tooling

```bash
pio run                      # build
pio run -t upload            # flash over USB
pio device monitor           # serial (with exception decoder + timestamps)
pio check                    # cppcheck static analysis (warning/style/perf/portability)
clang-format -i src/*.cpp src/*.h include/*.h   # format to .clang-format
```

## Troubleshooting

- **`[cc1101] init FAILED`** — check CS/GDO0/GDO2 wiring and that the module is
  on 3.3 V. A wrong CS pin is the usual culprit.
- **No telegrams received** — verify the antenna, that the meter is an 868 MHz
  T1 IZAR, and proximity. Some meters only transmit periodically.
- **Meter id looks wrong / no `total`** — the decoder assumes the standard
  CRC-stripped frame layout returned by wMbus-lib (id at bytes 4–7, payload at
  byte 15). Compare the `[rx] frame` hex dump against your telegram; offsets may
  need a small adjustment for an unusual variant.
- **Watchdog resets** — usually caused by blocking in the loop; the networking
  here is deliberately non-blocking. Avoid adding long `delay()` calls.

## Credits

- [zibous/ha-watermeter](https://github.com/zibous/ha-watermeter) — original ESPHome project
- [SzczepanLeon/wMbus-lib](https://github.com/SzczepanLeon/wMbus-lib) — wM-Bus reception
- [maciekn/izar-wmbus-esp](https://github.com/maciekn/izar-wmbus-esp) & [wmbusmeters](https://github.com/wmbusmeters/wmbusmeters) — IZAR descrambling
