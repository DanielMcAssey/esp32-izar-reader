#pragma once

#include <Arduino.h>
#include <vector>

// ---------------------------------------------------------------------------
//  Diehl IZAR (RC 868) wM-Bus payload decoder.
//
//  wMbus-lib already does the CC1101 reception, 3-out-of-6 decoding, CRC check
//  and strips the wM-Bus block CRCs, handing us the raw link-layer telegram:
//      [0]     L   (length)
//      [1]     C   (control)
//      [2..3]  M   (manufacturer)
//      [4..7]  A   (address / meter id, little-endian)
//      [8]     version
//      [9]     type
//      [10]    CI
//      [11..14] header (status / config word - carries battery, period, alarms)
//      [15..]  scrambled payload (descrambles to total / last-month volumes)
//
//  IZAR uses a proprietary descrambling algorithm (no user AES key). Field
//  offsets are ported from wmbusmeters' izar driver.
// ---------------------------------------------------------------------------

struct IzarReading {
  bool     valid           = false;  // descrambled to a valid IZAR frame
  uint32_t meterId         = 0;      // little-endian A field
  uint32_t liters          = 0;      // total consumption (L)
  float    m3              = 0.0f;   // total consumption (m3)
  uint32_t lastMonthLiters = 0;      // total around end of last month (L)
  float    lastMonthM3     = 0.0f;   // total around end of last month (m3)
  float    batteryYears    = 0.0f;   // estimated remaining battery life (years)
  uint16_t transmitPeriodS = 0;      // meter transmit period (seconds)
  bool     generalAlarm    = false;  // device-level general alarm flag
  char     currentAlarms[96] = "";   // comma-separated, or "no_alarm"
  char     previousAlarms[48] = "";  // comma-separated, or "no_alarm"
  char     lastMonthDate[12] = "";   // "YYYY-MM-DD" of last-month reading, or ""
};

// Decode an already-CRC-stripped wM-Bus telegram (as returned by wMbus-lib's
// WMbusFrame.frame). Returns a reading with valid=false if the frame is too
// short or does not descramble to a valid IZAR payload.
IzarReading izarDecode(const std::vector<uint8_t> &frame);
