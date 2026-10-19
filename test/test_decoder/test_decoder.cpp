#include <unity.h>

#include <cstring>
#include <vector>

#include "izar_decoder.h"

// ---------------------------------------------------------------------------
//  Host-side tests for the pure decoder. Frames are synthesised: the header and
//  payload are laid out per izar_decoder.h, then scrambled with a keystream
//  written independently of the production code. That exercises the field
//  offsets, bit twiddling and bounds handling; to also pin the algorithm
//  against real hardware, add a telegram captured in METER_ID=0 bring-up mode
//  (the "[rx] frame" hex dump) together with the values the meter displays.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kMeterId = 0x11223344;

uint32_t be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

void put32le(std::vector<uint8_t> &v, size_t at, uint32_t value) {
  for (int i = 0; i < 4; i++) v[at + i] = (value >> (8 * i)) & 0xFF;
}

// XOR the payload (bytes 15..) with the IZAR keystream derived from the header.
void scramble(std::vector<uint8_t> &f) {
  uint32_t key = 0xdfd109e8u ^ be32(&f[2]) ^ be32(&f[6]) ^ be32(&f[10]);
  for (size_t i = 15; i < f.size(); i++) {
    for (int step = 0; step < 8; step++) {
      uint32_t feedback = ((key >> 1) ^ (key >> 2) ^ (key >> 11) ^ (key >> 31)) & 1u;
      key = (key << 1) | feedback;
    }
    f[i] ^= (uint8_t)key;
  }
}

struct FrameSpec {
  uint32_t liters      = 123456;
  uint32_t lastMonthL  = 120000;
  uint8_t  dateLo      = 0x2F;  // 2025-06-15: year low 3 bits (001) | day 15
  uint8_t  dateHi      = 0x36;  //             year high (0011)      | month 6
  uint8_t  periodByte  = 0x03;  // period nibble 3 -> 32 s, no general alarm
  uint8_t  batteryByte = 0x14;  // 20 / 2 = 10.0 years, no leakage/blocked
  uint8_t  alarmByte   = 0x00;
  uint8_t  marker      = 0x4B;
  size_t   payloadLen  = 12;
};

std::vector<uint8_t> buildFrame(const FrameSpec &s) {
  std::vector<uint8_t> f(15 + s.payloadLen, 0);
  f[0] = (uint8_t)(f.size() - 1);
  put32le(f, 4, kMeterId);
  f[11] = s.periodByte;
  f[12] = s.batteryByte;
  f[13] = s.alarmByte;

  f[15] = s.marker;
  put32le(f, 16, s.liters);
  if (s.payloadLen >= 9) put32le(f, 20, s.lastMonthL);
  if (s.payloadLen > 10) {
    f[24] = s.dateLo;
    f[25] = s.dateHi;
  }
  scramble(f);
  return f;
}

}  // namespace

void setUp() {}
void tearDown() {}

// --- frame validation --------------------------------------------------------

void test_too_short_frame_is_invalid() {
  std::vector<uint8_t> f(19, 0);
  TEST_ASSERT_FALSE(izarDecode(f).valid);
}

void test_oversized_frame_is_invalid() {
  std::vector<uint8_t> f(256, 0);
  TEST_ASSERT_FALSE(izarDecode(f).valid);
}

void test_bad_marker_is_invalid() {
  FrameSpec s;
  s.marker = 0x00;
  TEST_ASSERT_FALSE(izarDecode(buildFrame(s)).valid);
}

// --- decoded fields ------------------------------------------------------------

void test_decodes_meter_id_and_volumes() {
  IzarReading r = izarDecode(buildFrame(FrameSpec{}));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(kMeterId, r.meterId);
  TEST_ASSERT_EQUAL_UINT32(123456, r.liters);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 123.456, r.m3);
  TEST_ASSERT_TRUE(r.hasLastMonth);
  TEST_ASSERT_EQUAL_UINT32(120000, r.lastMonthLiters);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 120.0, r.lastMonthM3);
}

void test_large_total_keeps_litre_resolution() {
  FrameSpec s;
  s.liters = 12345678;  // 12345.678 m3: a float would round this
  IzarReading r = izarDecode(buildFrame(s));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_DOUBLE_WITHIN(1e-9, 12345.678, r.m3);
}

void test_decodes_last_month_date() {
  IzarReading r = izarDecode(buildFrame(FrameSpec{}));
  TEST_ASSERT_EQUAL_STRING("2025-06-15", r.lastMonthDate);
}

void test_invalid_date_is_left_empty() {
  FrameSpec s;
  s.dateHi = 0x30;  // month 0
  IzarReading r = izarDecode(buildFrame(s));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_STRING("", r.lastMonthDate);

  s.dateHi = 0x36;
  s.dateLo = 0x20;  // day 0
  TEST_ASSERT_EQUAL_STRING("", izarDecode(buildFrame(s)).lastMonthDate);
}

void test_short_payload_has_no_date_or_last_month() {
  FrameSpec s;
  s.payloadLen = 5;  // marker + total only
  IzarReading r = izarDecode(buildFrame(s));
  TEST_ASSERT_TRUE(r.valid);
  TEST_ASSERT_EQUAL_UINT32(123456, r.liters);
  TEST_ASSERT_FALSE(r.hasLastMonth);
  TEST_ASSERT_EQUAL_UINT32(0, r.lastMonthLiters);
  TEST_ASSERT_EQUAL_STRING("", r.lastMonthDate);
}

void test_battery_and_period() {
  IzarReading r = izarDecode(buildFrame(FrameSpec{}));
  TEST_ASSERT_EQUAL_FLOAT(10.0f, r.batteryYears);
  TEST_ASSERT_EQUAL_UINT32(32, r.transmitPeriodS);
  TEST_ASSERT_FALSE(r.generalAlarm);
}

void test_max_transmit_period_does_not_overflow() {
  FrameSpec s;
  s.periodByte = 0x8F;  // nibble 15 -> 1 << 17, plus general alarm bit
  IzarReading r = izarDecode(buildFrame(s));
  TEST_ASSERT_EQUAL_UINT32(131072, r.transmitPeriodS);
  TEST_ASSERT_TRUE(r.generalAlarm);
}

// --- alarms --------------------------------------------------------------------

void test_no_alarms() {
  IzarReading r = izarDecode(buildFrame(FrameSpec{}));
  TEST_ASSERT_EQUAL_STRING("no_alarm", r.currentAlarms);
  TEST_ASSERT_EQUAL_STRING("no_alarm", r.previousAlarms);
}

void test_current_and_previous_alarms() {
  FrameSpec s;
  s.batteryByte = 0xA0 | 0x14;  // leakage (b7) + meter_blocked (b5), battery 10 y
  s.alarmByte   = 0x80 | 0x01;  // back_flow (b7) + previous mechanical_fraud (b0)
  IzarReading r = izarDecode(buildFrame(s));
  TEST_ASSERT_EQUAL_STRING("leakage,meter_blocked,back_flow", r.currentAlarms);
  TEST_ASSERT_EQUAL_STRING("mechanical_fraud", r.previousAlarms);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, r.batteryYears);
}

void test_previous_leakage_and_fraud() {
  FrameSpec s;
  s.batteryByte = 0x40;         // previous leakage (b6)
  s.alarmByte   = 0x04 | 0x08;  // previous sensor_fraud (b2), current sensor_fraud (b3)
  IzarReading r = izarDecode(buildFrame(s));
  TEST_ASSERT_EQUAL_STRING("sensor_fraud", r.currentAlarms);
  TEST_ASSERT_EQUAL_STRING("leakage,sensor_fraud", r.previousAlarms);
}

// --- plausibility filter ---------------------------------------------------------

void test_first_reading_is_accepted() {
  IzarPlausibility p(10000);
  TEST_ASSERT_TRUE(p.accept(500000));
}

void test_small_increase_and_repeat_accepted() {
  IzarPlausibility p(10000);
  TEST_ASSERT_TRUE(p.accept(1000));
  TEST_ASSERT_TRUE(p.accept(1000));
  TEST_ASSERT_TRUE(p.accept(1500));
}

void test_decrease_is_rejected() {
  IzarPlausibility p(10000);
  p.accept(5000);
  TEST_ASSERT_FALSE(p.accept(4000));
}

void test_isolated_glitch_does_not_move_baseline() {
  IzarPlausibility p(10000);
  p.accept(100000);
  TEST_ASSERT_FALSE(p.accept(900000));
  TEST_ASSERT_TRUE(p.accept(100010));
  TEST_ASSERT_FALSE(p.accept(900000));
  TEST_ASSERT_FALSE(p.accept(5));
  TEST_ASSERT_TRUE(p.accept(100020));
}

void test_bad_first_baseline_recovers_after_three_agreeing_readings() {
  IzarPlausibility p(10000);
  p.accept(5000000);  // garbage latched as the first baseline
  TEST_ASSERT_FALSE(p.accept(1000));
  TEST_ASSERT_FALSE(p.accept(1001));
  TEST_ASSERT_TRUE(p.accept(1002));
  TEST_ASSERT_TRUE(p.accept(1010));
}

void test_real_jump_is_adopted_after_confirmation() {
  IzarPlausibility p(10000);
  p.accept(1000);
  TEST_ASSERT_FALSE(p.accept(500000));
  TEST_ASSERT_FALSE(p.accept(500005));
  TEST_ASSERT_TRUE(p.accept(500009));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_too_short_frame_is_invalid);
  RUN_TEST(test_oversized_frame_is_invalid);
  RUN_TEST(test_bad_marker_is_invalid);
  RUN_TEST(test_decodes_meter_id_and_volumes);
  RUN_TEST(test_large_total_keeps_litre_resolution);
  RUN_TEST(test_decodes_last_month_date);
  RUN_TEST(test_invalid_date_is_left_empty);
  RUN_TEST(test_short_payload_has_no_date_or_last_month);
  RUN_TEST(test_battery_and_period);
  RUN_TEST(test_max_transmit_period_does_not_overflow);
  RUN_TEST(test_no_alarms);
  RUN_TEST(test_current_and_previous_alarms);
  RUN_TEST(test_previous_leakage_and_fraud);
  RUN_TEST(test_first_reading_is_accepted);
  RUN_TEST(test_small_increase_and_repeat_accepted);
  RUN_TEST(test_decrease_is_rejected);
  RUN_TEST(test_isolated_glitch_does_not_move_baseline);
  RUN_TEST(test_bad_first_baseline_recovers_after_three_agreeing_readings);
  RUN_TEST(test_real_jump_is_adopted_after_confirmation);
  return UNITY_END();
}
