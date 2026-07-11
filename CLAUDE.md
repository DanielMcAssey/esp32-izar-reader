# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware (PlatformIO / Arduino / ESP32) that receives a **Diehl IZAR RC 868** water meter's wireless M-Bus (wM-Bus) **T1** telegrams (~868.95 MHz) via a CC1101 transceiver, descrambles the proprietary IZAR payload (no AES key needed), and publishes readings to **Home Assistant over MQTT** with auto-discovery. It is a native port of the ESPHome project `zibous/ha-watermeter`.

## Commands

```bash
pio run                      # build (default env: usb)
pio run -t upload            # build + flash over USB
pio device monitor           # serial @115200 (esp32_exception_decoder + timestamps)
pio run -e ota -t upload     # flash wirelessly (needs IZAR_OTA_PASSWORD env var)
pio check                    # cppcheck static analysis (warning/style/performance/portability)
clang-format -i src/*.cpp src/*.h include/*.h   # format to .clang-format
```

There is **no test suite** — verification is done on-device via the serial monitor. To exercise a change, flash it and watch `pio device monitor`.

OTA target host is `upload_port` in `platformio.ini` (default `izar-watermeter.local`); override per-run with `--upload-port <ip>`. `IZAR_OTA_PASSWORD` must match `OTA_PASSWORD` in `include/secrets.h`.

## Setup prerequisite

`include/secrets.h` is gitignored and required to build. Copy `include/secrets.h.example` → `include/secrets.h` and set WiFi, MQTT, OTA credentials, and `METER_ID`.

## Architecture

Three source areas, each with a distinct responsibility:

- **`include/config.h`** — all hardware pins, radio frequency, MQTT/HA topics, and resilience-tuning constants. It `#include`s `secrets.h` (credentials + `METER_ID`). Change wiring, topics, and timing here, not in `main.cpp`.

- **`src/izar_decoder.{h,cpp}`** — pure decode logic, no I/O. `izarDecode()` takes an **already-CRC-stripped** link-layer telegram (as handed over by wMbus-lib) and returns an `IzarReading`. The wM-Bus header layout it assumes is documented at the top of `izar_decoder.h` (meter id at bytes 4–7 little-endian, config word at 11–14, scrambled payload from byte 15). Descrambling ports wmbusmeters' IZAR driver (`hashShiftKey` → `izarDescramble`); battery/period/alarms come from the header, total/last-month volumes from the descrambled payload. If a frame decodes wrong, offsets here are the place to look.

- **`src/main.cpp`** — everything runtime: the CC1101 RX loop plus WiFi/MQTT/OTA. `mbus` (from wMbus-lib) drives the radio; `loop()` polls `mbus.task()`, decodes T1 frames, and publishes.

### Reception → publish flow

wMbus-lib owns the CC1101 (SPI + GDO0/GDO2), does 3-out-of-6 decoding, CRC checks, and strips block CRCs. `loop()` → `mbus.task()` → `izarDecode(frame.frame)` → `publishReading()`.

### Two operating modes gated by `METER_ID`

- `METER_ID == 0` (**bring-up**): logs *every* T1 telegram seen (meter id, RSSI, raw hex dump) and **MQTT publishing is disabled**. Used to discover your meter id. Extra debug code (`dumpHex`) is `#if METER_ID == 0` compiled out otherwise.
- `METER_ID == <your id>`: filters to that meter and enables publishing.

### Networking is deliberately non-blocking (do not break this)

The CC1101 RX loop is timing-sensitive, so nothing in `loop()` may block for long. `maintainWifi()` / `maintainMqtt()` run on a periodic non-blocking cadence (`NET_CHECK_INTERVAL_MS`) with bounded MQTT retry backoff and a short socket timeout. **Never add `delay()` or other blocking calls to the loop** — it causes watchdog resets. No WiFi simply skips MQTT (still decodes/logs over serial).

### Home Assistant MQTT discovery

On each MQTT (re)connect, `publishDiscovery()` sends **one retained** device-based discovery config to `homeassistant/device/izar_watermeter/config` — a `device` block, mandatory `origin`, shared `state_topic`/`availability_topic`, and a `components` map (each entity carries its own `platform` + `unique_id`). Readings go to `watermeter/izar/state` as JSON; availability/LWT to `watermeter/izar/status`.

The combined discovery payload is ~2.8 KB, so `mqtt.setBufferSize()` in `setup()` **must** stay large enough (currently 4096) — `PubSubClient::publish` fails silently if a packet exceeds the buffer. If you add/rename `value_json` keys in `publishReading()`, keep the matching `value_template` in `publishDiscovery()` in sync.

## Conventions

- Pins, topics, and tuning constants are `#define`s in `config.h` — reuse them rather than hard-coding.
- Libraries are pinned in `platformio.ini` (`platform = espressif32@7.0.1`, exact lib versions). Bump deliberately.
