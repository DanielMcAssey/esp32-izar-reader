#include "izar_decoder.h"

#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
//  Frame layout constants (see izar_decoder.h). "Frame" offsets index the raw
//  CRC-stripped telegram; "payload" offsets index the descrambled payload.
// ---------------------------------------------------------------------------
namespace {
constexpr uint8_t HEADER_LEN    = 15;    // scrambled payload starts here
constexpr size_t  MIN_FRAME_LEN = 20;    // header + at least the 4-byte volume + marker
constexpr size_t  MAX_FRAME_LEN = 255;   // wM-Bus L-field is one byte
constexpr uint32_t KEY_SEED     = 0xdfd109e8;  // IZAR descrambler initial key
constexpr uint8_t VALID_MARKER  = 0x4B;  // descrambled payload[0] must equal this
constexpr uint8_t OFF_METER_ID  = 4;     // frame: A field (LE)
constexpr uint8_t FRAME_PERIOD  = 11;    // frame: transmit-period / general alarm
constexpr uint8_t FRAME_BATTERY = 12;    // frame: battery / leakage / blocked
constexpr uint8_t FRAME_ALARMS  = 13;    // frame: flow / fraud alarms
constexpr uint8_t PAY_TOTAL     = 1;     // payload: total volume (LE, L)
constexpr uint8_t PAY_LASTMONTH = 5;     // payload: last-month volume (LE, L)
constexpr uint8_t PAY_DATE_LO   = 9;     // payload: date low byte (day + year low)
constexpr uint8_t PAY_DATE_HI   = 10;    // payload: date high byte (month + year high)
}  // namespace

// ---------------------------------------------------------------------------
//  Byte helpers (ported from maciekn/izar-wmbus-esp izar_utils.cpp)
// ---------------------------------------------------------------------------

namespace {
enum class Endian { Big, Little };

constexpr uint8_t bit(uint32_t value, uint8_t pos) { return (value >> pos) & 0x01; }

uint32_t readU32(const uint8_t *data, Endian order) {
  uint32_t result = 0;
  for (uint8_t i = 0; i < 4; i++) {
    const uint8_t b = (order == Endian::Big) ? data[i] : data[3 - i];
    result = (result << 8) | b;
  }
  return result;
}

// LFSR-style key mangling used by the IZAR descrambler.
uint32_t hashShiftKey(uint32_t key) {
  for (uint8_t i = 0; i < 8; i++) {
    const uint8_t feedback = bit(key, 1) ^ bit(key, 2) ^ bit(key, 11) ^ bit(key, 31);
    key = (key << 1) | feedback;
  }
  return key;
}
}  // namespace

// IZAR descrambler. `encoded` is the CRC-stripped telegram; writes the
// descrambled payload into `decoded` (at most `outCap` bytes). Returns the
// payload length, or 0 if the frame is too short or the descrambled result is
// not a valid IZAR frame (first byte must be VALID_MARKER).
static uint8_t izarDescramble(const uint8_t *encoded, uint8_t len,
                              uint8_t *decoded, uint8_t outCap) {
  if (len < HEADER_LEN) {
    return 0;
  }

  uint32_t key = KEY_SEED;
  key ^= readU32(encoded + 2, Endian::Big);
  key ^= readU32(encoded + 6, Endian::Big);
  key ^= readU32(encoded + 10, Endian::Big);

  uint8_t size = len - HEADER_LEN;
  if (size > outCap) {  // never write past the caller's buffer
    size = outCap;
  }
  for (uint8_t i = 0; i < size; i++) {
    key = hashShiftKey(key);
    decoded[i] = encoded[i + HEADER_LEN] ^ (key & 0xFF);
  }

  if (decoded[0] != VALID_MARKER) {
    return 0;
  }

  return size;
}

// Append "token," to buf (bounded). `used` tracks current length.
static void appendAlarm(char *buf, size_t cap, size_t &used, const char *token) {
  size_t n = strlen(token);
  if (used + n + 2 >= cap) {  // +1 comma, +1 NUL
    return;
  }
  memcpy(buf + used, token, n);
  used += n;
  buf[used++] = ',';
  buf[used] = '\0';
}

// Build the current/previous alarm strings from the raw frame status bytes,
// mirroring wmbusmeters' izar driver. Trailing comma is trimmed; empty -> value.
static void decodeAlarms(const uint8_t *frame, IzarReading &r) {
  const uint8_t b12 = frame[FRAME_BATTERY];
  const uint8_t b13 = frame[FRAME_ALARMS];

  r.generalAlarm = bit(frame[FRAME_PERIOD], 7);

  size_t cur = 0, prev = 0;
  r.currentAlarms[0] = '\0';
  r.previousAlarms[0] = '\0';

  // current
  if (bit(b12, 7)) appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "leakage");
  if (bit(b12, 5)) appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "meter_blocked");
  if (bit(b13, 7)) appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "back_flow");
  if (bit(b13, 6)) appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "underflow");
  if (bit(b13, 5)) appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "overflow");
  if (bit(b13, 4)) appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "submarine");
  if (bit(b13, 3)) appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "sensor_fraud");
  if (bit(b13, 1)) appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "mechanical_fraud");

  // previous
  if (bit(b12, 6)) appendAlarm(r.previousAlarms, sizeof(r.previousAlarms), prev, "leakage");
  if (bit(b13, 2)) appendAlarm(r.previousAlarms, sizeof(r.previousAlarms), prev, "sensor_fraud");
  if (bit(b13, 0)) appendAlarm(r.previousAlarms, sizeof(r.previousAlarms), prev, "mechanical_fraud");

  if (cur == 0) {
    strcpy(r.currentAlarms, "no_alarm");
  } else {
    r.currentAlarms[cur - 1] = '\0';  // trim trailing comma
  }
  if (prev == 0) {
    strcpy(r.previousAlarms, "no_alarm");
  } else {
    r.previousAlarms[prev - 1] = '\0';
  }
}

// ---------------------------------------------------------------------------

IzarReading izarDecode(const std::vector<uint8_t> &frame) {
  IzarReading r;

  // Guard both ends before narrowing to uint8_t: a wM-Bus L-field is one byte,
  // so anything above 255 is a malformed/garbage frame from the radio.
  if (frame.size() < MIN_FRAME_LEN || frame.size() > MAX_FRAME_LEN) {
    return r;
  }
  const uint8_t  len     = (uint8_t)frame.size();
  const uint8_t *encoded = frame.data();

  // Meter id is the little-endian A field, available before descrambling.
  r.meterId = readU32(encoded + OFF_METER_ID, Endian::Little);

  uint8_t decrypted[64] = {0};
  uint8_t decryptedLen = izarDescramble(encoded, len, decrypted, sizeof(decrypted));
  if (decryptedLen < 5) {  // need at least the 4-byte volume after byte 0
    return r;
  }

  // Total consumption (liters) at payload[1..4], little-endian.
  r.liters = readU32(decrypted + PAY_TOTAL, Endian::Little);
  r.m3     = r.liters / 1000.0;

  // Last-month total at payload[5..8] when present.
  if (decryptedLen >= PAY_LASTMONTH + 4) {
    r.hasLastMonth    = true;
    r.lastMonthLiters = readU32(decrypted + PAY_LASTMONTH, Endian::Little);
    r.lastMonthM3     = r.lastMonthLiters / 1000.0;
  }

  // Last-month measure date packed across payload[9] and payload[10]:
  //   year  = high nibble of [10] (<<3) | top 3 bits of [9]   (2-digit, +1900/+2000)
  //   month = [10] & 0x0F,   day = [9] & 0x1F
  if (decryptedLen > PAY_DATE_HI) {
    int decodeYear = ((decrypted[PAY_DATE_HI] & 0xF0) >> 1) |
                     ((decrypted[PAY_DATE_LO] & 0xE0) >> 5);
    int year  = decodeYear > 80 ? decodeYear + 1900 : decodeYear + 2000;
    int month = decrypted[PAY_DATE_HI] & 0x0F;
    int day   = decrypted[PAY_DATE_LO] & 0x1F;
    // Leave the date empty rather than emit "2026-00-00" for a garbled field.
    if (month >= 1 && month <= 12 && day >= 1 && day <= 31) {
      snprintf(r.lastMonthDate, sizeof(r.lastMonthDate), "%04d-%02d-%02d", year, month, day);
    }
  }

  // Battery / transmit period from the raw header (frame >= 14 bytes: guaranteed).
  r.batteryYears    = (encoded[FRAME_BATTERY] & 0x1F) / 2.0f;
  r.transmitPeriodS = (uint32_t)1 << ((encoded[FRAME_PERIOD] & 0x0F) + 2);

  decodeAlarms(encoded, r);

  r.valid = true;
  return r;
}

bool IzarPlausibility::accept(uint32_t liters) {
  if (!haveBaseline_ || within(baseline_, liters)) {
    haveBaseline_ = true;
    baseline_     = liters;
    agreeing_     = 0;
    return true;
  }

  // Out of range: only adopt it once several consecutive readings agree.
  if (agreeing_ > 0 && within(candidate_, liters)) {
    agreeing_++;
  } else {
    agreeing_ = 1;
  }
  candidate_ = liters;

  if (agreeing_ >= confirmCount_) {
    baseline_ = liters;
    agreeing_ = 0;
    return true;
  }
  return false;
}
