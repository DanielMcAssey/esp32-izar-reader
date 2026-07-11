#include "izar_decoder.h"

// ---------------------------------------------------------------------------
//  Frame layout constants (see izar_decoder.h). "Frame" offsets index the raw
//  CRC-stripped telegram; "payload" offsets index the descrambled payload.
// ---------------------------------------------------------------------------
namespace {
constexpr uint8_t HEADER_LEN    = 15;    // scrambled payload starts here
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

#define GET_BIT(var, pos) (((var) >> (pos)) & 0x01)

// Big-endian 32-bit read.
static uint32_t uintFromBytes(const uint8_t *data) {
  uint32_t result = (uint32_t)data[0] << 24;
  result += (uint32_t)data[1] << 16;
  result += (uint32_t)data[2] << 8;
  result += data[3];
  return result;
}

// Little-endian 32-bit read.
static uint32_t uintFromBytesLittleEndian(const uint8_t *data) {
  uint32_t result = (uint32_t)data[3] << 24;
  result += (uint32_t)data[2] << 16;
  result += (uint32_t)data[1] << 8;
  result += data[0];
  return result;
}

// LFSR-style key mangling used by the IZAR descrambler.
static uint32_t hashShiftKey(uint32_t key) {
  for (uint8_t i = 0; i < 8; i++) {
    uint8_t bit = GET_BIT(key, 1) ^ GET_BIT(key, 2) ^ GET_BIT(key, 11) ^
                  GET_BIT(key, 31);
    key <<= 1;
    key |= bit;
  }
  return key;
}

// IZAR descrambler. `encoded` is the CRC-stripped telegram; writes the
// descrambled payload into `decoded` (at most `outCap` bytes). Returns the
// payload length, or 0 if the frame is too short or the descrambled result is
// not a valid IZAR frame (first byte must be VALID_MARKER).
static uint8_t izarDescramble(const uint8_t *encoded, uint8_t len,
                              uint8_t *decoded, uint8_t outCap) {
  if (len < HEADER_LEN) {
    return 0;
  }

  uint32_t key = 0xdfd109e8;
  key ^= uintFromBytes(encoded + 2);
  key ^= uintFromBytes(encoded + 6);
  key ^= uintFromBytes(encoded + 10);

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

  r.generalAlarm = (frame[FRAME_PERIOD] >> 7) & 0x1;

  size_t cur = 0, prev = 0;
  r.currentAlarms[0] = '\0';
  r.previousAlarms[0] = '\0';

  // current
  if (b12 >> 7)            appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "leakage");
  if ((b12 >> 5) & 0x1)    appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "meter_blocked");
  if (b13 >> 7)            appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "back_flow");
  if ((b13 >> 6) & 0x1)    appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "underflow");
  if ((b13 >> 5) & 0x1)    appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "overflow");
  if ((b13 >> 4) & 0x1)    appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "submarine");
  if ((b13 >> 3) & 0x1)    appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "sensor_fraud");
  if ((b13 >> 1) & 0x1)    appendAlarm(r.currentAlarms, sizeof(r.currentAlarms), cur, "mechanical_fraud");

  // previous
  if ((b12 >> 6) & 0x1)    appendAlarm(r.previousAlarms, sizeof(r.previousAlarms), prev, "leakage");
  if ((b13 >> 2) & 0x1)    appendAlarm(r.previousAlarms, sizeof(r.previousAlarms), prev, "sensor_fraud");
  if (b13 & 0x1)           appendAlarm(r.previousAlarms, sizeof(r.previousAlarms), prev, "mechanical_fraud");

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
  if (frame.size() < 20 || frame.size() > 255) {  // need header (15) + payload
    return r;
  }
  const uint8_t  len     = (uint8_t)frame.size();
  const uint8_t *encoded = frame.data();

  // Meter id is the little-endian A field, available before descrambling.
  r.meterId = uintFromBytesLittleEndian(encoded + OFF_METER_ID);

  uint8_t decrypted[64] = {0};
  uint8_t decryptedLen = izarDescramble(encoded, len, decrypted, sizeof(decrypted));
  if (decryptedLen < 5) {  // need at least the 4-byte volume after byte 0
    return r;
  }

  // Total consumption (liters) at payload[1..4], little-endian.
  r.liters = uintFromBytesLittleEndian(decrypted + PAY_TOTAL);
  r.m3     = r.liters / 1000.0f;

  // Last-month total at payload[5..8] when present.
  if (decryptedLen >= PAY_LASTMONTH + 4) {
    r.lastMonthLiters = uintFromBytesLittleEndian(decrypted + PAY_LASTMONTH);
    r.lastMonthM3     = r.lastMonthLiters / 1000.0f;
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
    snprintf(r.lastMonthDate, sizeof(r.lastMonthDate), "%04d-%02d-%02d", year, month, day);
  }

  // Battery / transmit period from the raw header (frame >= 14 bytes: guaranteed).
  r.batteryYears    = (encoded[FRAME_BATTERY] & 0x1F) / 2.0f;
  r.transmitPeriodS = (uint16_t)1 << ((encoded[FRAME_PERIOD] & 0x0F) + 2);

  decodeAlarms(encoded, r);

  r.valid = true;
  return r;
}
