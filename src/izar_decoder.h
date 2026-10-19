#pragma once

#include <cstdint>
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
  double   m3              = 0.0;    // total consumption (m3); double keeps 0.001 m3 resolution
  bool     hasLastMonth    = false;  // payload carried the last-month volume
  uint32_t lastMonthLiters = 0;      // total around end of last month (L)
  double   lastMonthM3     = 0.0;    // total around end of last month (m3)
  float    batteryYears    = 0.0f;   // estimated remaining battery life (years)
  uint32_t transmitPeriodS = 0;      // meter transmit period (seconds, up to 2^17)
  bool     generalAlarm    = false;  // device-level general alarm flag
  char     currentAlarms[96] = "";   // comma-separated, or "no_alarm"
  char     previousAlarms[48] = "";  // comma-separated, or "no_alarm"
  char     lastMonthDate[12] = "";   // "YYYY-MM-DD" of last-month reading, or "" if absent/invalid
};

// Decode an already-CRC-stripped wM-Bus telegram (as returned by wMbus-lib's
// WMbusFrame.frame). Returns a reading with valid=false if the frame is too
// short or does not descramble to a valid IZAR payload.
IzarReading izarDecode(const std::vector<uint8_t> &frame);

// Guards against implausible readings (noise that slipped past CRC but still
// descrambled to the 0x4B marker). A reading is accepted when the total is
// monotonic and within `maxJumpL` litres of the last accepted one. If the
// meter really did jump (or the very first reading was garbage), the new value
// is adopted once `confirmCount` consecutive rejected readings agree with each
// other, so the filter can never latch onto a bad baseline.
class IzarPlausibility {
 public:
  explicit IzarPlausibility(uint32_t maxJumpL, uint8_t confirmCount = 3)
      : maxJumpL_(maxJumpL), confirmCount_(confirmCount) {}

  // Returns true if `liters` should be published; updates internal state.
  bool accept(uint32_t liters);

 private:
  bool within(uint32_t base, uint32_t value) const {
    return value >= base && value - base <= maxJumpL_;
  }

  uint32_t maxJumpL_;
  uint8_t  confirmCount_;
  bool     haveBaseline_ = false;
  uint32_t baseline_     = 0;
  uint32_t candidate_    = 0;
  uint8_t  agreeing_     = 0;
};
