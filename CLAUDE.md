# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (PlatformIO / Arduino / ESP32) that receives a **Diehl IZAR RC 868** water meter's wireless M-Bus (wM-Bus) **T1** telegrams (~868.95 MHz) via a CC1101 transceiver, descrambles the proprietary IZAR payload (no AES key needed), and publishes readings to **Home Assistant over MQTT** with auto-discovery. It is a native port of the ESPHome project `zibous/ha-watermeter`.

## Commands

```bash
pio run                      # build (default env: usb)
pio run -t upload            # build + flash over USB
pio device monitor           # serial @115200 (exception decoder, timestamps, izar_quiet filter hides wMbus-lib per-frame chatter)
pio run -e ota -t upload     # flash wirelessly (needs IZAR_OTA_PASSWORD env var)
pio check                    # cppcheck static analysis (warning/style/performance/portability)
clang-format -i src/*.cpp src/*.h include/*.h   # format to .clang-format
```

`pio test -e native` runs host-side Unity tests for the pure decoder (`test/test_decoder/`); it needs a host gcc/clang on PATH (or run the same g++ command inside a `gcc` Docker image). Everything else (radio, WiFi, MQTT) is verified on-device: flash it and watch `pio device monitor`. Tests use synthesised frames; add a real captured telegram (METER_ID=0 hex dump) to pin the descrambler against hardware.

OTA target host is `upload_port` in `platformio.ini` (default `izar-watermeter.local`); override per-run with `--upload-port <ip>`. `IZAR_OTA_PASSWORD` must match `OTA_PASSWORD` in `include/secrets.h`.

## Setup prerequisite

`include/secrets.h` is gitignored and required to build. Copy `include/secrets.h.example` → `include/secrets.h` and set WiFi, MQTT, OTA credentials, and `METER_ID`.

## Architecture

Three source areas, each with a distinct responsibility:

- **`include/config.h`** — all hardware pins, radio frequency, MQTT/HA topics, and resilience-tuning constants. It `#include`s `secrets.h` (credentials + `METER_ID`). Change wiring, topics, and timing here, not in `main.cpp`.

- **`src/izar_decoder.{h,cpp}`** — pure decode logic, no I/O. `izarDecode()` takes an **already-CRC-stripped** link-layer telegram (as handed over by wMbus-lib) and returns an `IzarReading`. The wM-Bus header layout it assumes is documented at the top of `izar_decoder.h` (meter id at bytes 4–7 little-endian, config word at 11–14, scrambled payload from byte 15). Descrambling ports wmbusmeters' IZAR driver (`hashShiftKey` → `izarDescramble`); battery/period/alarms come from the header, total/last-month volumes from the descrambled payload. If a frame decodes wrong, offsets here are the place to look.

  It also holds `IzarPlausibility`, a pure filter that rejects non-monotonic / implausibly large jumps in the total until 3 consecutive readings agree.

- **`src/main.cpp`** — everything runtime: the CC1101 RX loop (core 1) plus a `netTask` (core 0) for WiFi/MQTT/OTA. `mbus` (from wMbus-lib) drives the radio; `loop()` polls `mbus.task()`, decodes T1 frames, plausibility-checks them and queues them for `netTask` to publish.

### Reception → publish flow

wMbus-lib owns the CC1101 (SPI + GDO0/GDO2), does 3-out-of-6 decoding, CRC checks, and strips block CRCs. `loop()` → `mbus.task()` → `izarDecode(frame.frame)` → `IzarPlausibility::accept()` → FreeRTOS queue → `netTask` → `publishReading()`.

### Two operating modes gated by `METER_ID`

- `METER_ID == 0` (**bring-up**): logs *every* T1 telegram seen (meter id, RSSI, raw hex dump) and **MQTT publishing is disabled**. Used to discover your meter id. Extra debug code (`dumpHex`) is `#if METER_ID == 0` compiled out otherwise.
- `METER_ID == <your id>`: filters to that meter and enables publishing.

### Networking lives on its own task (do not break this)

The CC1101 RX loop is timing-sensitive, so **`loop()` must never block** (no `delay()`, no network calls) — it causes missed telegrams and watchdog resets. All WiFi/MQTT/OTA work runs in `netTask` pinned to core 0; it may block (broker connect, DNS) freely. `PubSubClient`/`WiFiClient` are not thread-safe: **only `netTask` may touch `mqtt`**; `loop()` hands readings over via `readingQueue`. WiFi forced re-begin backs off exponentially; MQTT retries on `MQTT_RETRY_INTERVAL_MS`. No WiFi simply skips MQTT (still decodes/logs over serial).

`loop()` queues every accepted reading; `netTask` applies the rate limit (publish when the total changes, otherwise at most every `PUBLISH_MIN_INTERVAL_MS`) and only counts a *successful* publish, keeping the newest reading while MQTT is down so it goes out on reconnect. `setup()` calls `enableLoopWDT()` because wMbus-lib busy-waits on the CC1101 with no timeout; a wedged radio then reboots the board instead of hanging `loop()`.

Radio watchdog (`config.h`): a failed CC1101 init is retried every `RADIO_RETRY_MS`; with no valid reading for `STALE_OFFLINE_MS` the device is published `offline` (back to `online` on the next reading); after `RADIO_STALL_RESTART_MS` the board reboots. Stale/restart checks are disabled in bring-up mode (`METER_ID == 0`).

### Home Assistant MQTT discovery
Discovery carries `sw_version` (`FW_VERSION`, build date/time). Absent last-month volume/date publish as JSON `null` (-> "unknown" in HA), not 0. Entity `name`s omit the device name (HA prepends it); entity IDs come from `default_entity_id` (full `<platform>.<id>`; the old `object_id` is deprecated/removed in HA), so renaming a display name is safe.


On each MQTT (re)connect, `publishDiscovery()` sends **one retained** device-based discovery config to `homeassistant/device/izar_watermeter/config` — a `device` block, mandatory `origin`, shared `state_topic`/`availability_topic`, and a `components` map (each entity carries its own `platform` + `unique_id`). Readings go to `watermeter/izar/state` as JSON; availability/LWT to `watermeter/izar/status`.

The device also subscribes to `homeassistant/status` and resends discovery + the last reading when HA announces `online`.

The combined discovery payload is ~2.8 KB, so `MQTT_BUFFER_SIZE` (4096) must stay larger than `MQTT_DISCOVERY_MAX_LEN` (3584) — `PubSubClient::publish` fails silently if a packet exceeds the buffer. A `static_assert` enforces this and publishing checks `measureJson` before serializing, so an oversized payload is logged rather than truncated. If you add/rename `value_json` keys in `publishReading()`, keep the matching `value_template` in `publishDiscovery()` in sync.

## Conventions

- Pins, topics, and tuning constants are `#define`s in `config.h` — reuse them rather than hard-coding.
- `platformio.ini`: shared ESP32 settings live in `[esp32]` (inherited by `usb`/`ota`); `[env]` only holds check config so `native` tests don't pull in the ESP32 platform.
- Libraries are pinned in `platformio.ini` (`platform = espressif32@7.0.1`, exact lib versions). Bump deliberately.
