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
   [izar] meter=21202341 total=123.456 m3 (123456 L) batt=10.0y period=32s alarms=no_alarm
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
config, so a `sensor.izar_watermeter_total` (device_class `water`, state_class
`total_increasing`, unit `m³`) appears automatically — no YAML needed. It is
suitable for the HA **Energy dashboard** (water).

All entities are declared together under a single **Izar Water Meter** device
(Diehl / IZAR RC 868) in one retained `homeassistant/device/.../config` message
(each entity is a `component` with its own `platform`). Entity names omit the
device name because HA prepends it; entity IDs are fixed by `object_id`
(`sensor.izar_watermeter_<key>`). The device also reports the firmware build
(`sw_version`). Published entities:

| Entity | Source | Notes |
|---|---|---|
| Total (`m³`) | descrambled payload | `total_increasing`, Energy-dashboard ready |
| Last month total (`m³`) | descrambled payload | value around end of previous month; no `state_class` (a snapshot, not a running total) |
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

## Releases

Pushing a tag such as `v1.2.0` runs `.github/workflows/release.yml`, which tests,
builds and publishes a GitHub release with `*-factory.bin` (flash at `0x0` on a
blank board), `*-app.bin` (application image only) and `SHA256SUMS.txt`.

WiFi/MQTT credentials and `METER_ID` are compiled in, so release binaries use the
placeholders from `secrets.h.example`. For a working device, build from source
with your own `include/secrets.h`.

## Resilience

The firmware is built to run unattended. Reception and networking are split
across the two ESP32 cores:

- **Receive loop (core 1)** only polls the CC1101, decodes and queues readings.
  It never blocks, so a slow network can't make it miss telegrams.
- **Network task (core 0)** owns WiFi, MQTT and OTA and may block freely.

Behaviour:

- **WiFi auto-reconnect.** The ESP32 core's auto-reconnect is enabled, and a
  fallback forces a fresh `WiFi.begin()` if the link stays down beyond
  `WIFI_REBEGIN_MS` (default 20 s). The interval doubles up to
  `WIFI_REBEGIN_MAX_MS` so a slow association or DHCP lease isn't interrupted.
- **No WiFi ≠ error.** If WiFi is down, MQTT is simply skipped. Telegrams are
  still decoded and logged over serial, and MQTT resumes automatically.
- **MQTT reconnect with backoff.** Attempts are throttled to
  `MQTT_RETRY_INTERVAL_MS` (default 10 s) with a short socket timeout.
- **Availability / LWT.** A retained last-will (`offline`) marks the device
  unavailable in HA if it drops unexpectedly; `online` is published on connect.
  If no valid reading arrives for `STALE_OFFLINE_MS` (30 min) the device is also
  marked `offline`, so stale data is visible; it returns `online` with the next
  reading.
- **HA restarts.** The device listens on `homeassistant/status` and resends
  discovery and the last reading when HA comes back, so entities don't vanish.
- **Rate-limited publishing.** The meter transmits every ~8–16 s. A reading is
  published when the total changes, otherwise at most every
  `PUBLISH_MIN_INTERVAL_MS` (60 s), which keeps the HA recorder from logging an
  RSSI row per telegram. A reading received while MQTT is down is sent right
  after reconnect.
- **Plausibility filter.** Totals that go backwards or jump by more than
  `IZAR_MAX_JUMP_L` (10 m³) are ignored until three consecutive readings agree,
  so a corrupted telegram that slipped past CRC can't poison the sensor.
- **Radio watchdog.** A failed CC1101 init is retried every `RADIO_RETRY_MS`;
  the loop watchdog reboots the board if the radio stops answering mid-run; and
  with no valid reading for `RADIO_STALL_RESTART_MS` (6 h) the board restarts.
  The stale/restart checks are off in bring-up mode (`METER_ID 0`).

Tuning constants live in [`include/config.h`](include/config.h).

## Project layout

```
platformio.ini            PlatformIO config + library deps + cppcheck setup
.clang-format             Formatting style (clang-format -i src/* include/*)
include/config.h          Pins, frequency, MQTT/HA topics, OTA, resilience tuning
include/secrets.h.example Template for WiFi/MQTT/OTA credentials + METER_ID
include/secrets.h         Your real credentials (gitignored)
src/main.cpp              Radio RX loop (core 1); network task: WiFi/MQTT/OTA, HA discovery (core 0)
src/izar_decoder.{h,cpp}  IZAR descrambler, field extraction, plausibility filter (pure logic)
test/test_decoder/        Host-side Unity tests for the decoder (pio test -e native)
monitor/                  pio device monitor filter hiding wMbus-lib per-frame chatter
.github/workflows/ci.yml  CI: native tests, cppcheck, firmware build
.github/workflows/release.yml  Tag push (v*) -> GitHub release with firmware binaries
```

## Developer tooling

```bash
pio run                      # build
pio run -t upload            # flash over USB
pio device monitor           # serial (exception decoder, timestamps, wMbus-lib noise filter)
pio test -e native           # decoder unit tests on the host (needs gcc/clang)
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
- **Watchdog resets** — the receive loop is watched (5 s); a reset usually means
  something blocked in `loop()` or the CC1101 stopped answering. Keep network
  and other slow work in the network task and avoid long `delay()` calls in `loop()`.
- **Entities show unavailable** — no valid reading for 30 min marks the device
  offline; check the antenna/range and the `[izar]` serial lines.

## Credits

- [zibous/ha-watermeter](https://github.com/zibous/ha-watermeter) — original ESPHome project
- [SzczepanLeon/wMbus-lib](https://github.com/SzczepanLeon/wMbus-lib) — wM-Bus reception
- [maciekn/izar-wmbus-esp](https://github.com/maciekn/izar-wmbus-esp) & [wmbusmeters](https://github.com/wmbusmeters/wmbusmeters) — IZAR descrambling
