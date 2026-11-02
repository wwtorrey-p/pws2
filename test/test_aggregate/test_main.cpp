#include <unity.h>

#include "aggregate.h"

namespace {
constexpr uint32_t kT0 = 1'800'000'100;  // multiple of 300 plus 100 s: inside a bucket

Obs obs(uint32_t epoch, float temp, float avg, float gust, float dir, float rain = 0) {
    Obs o;
    o.epoch = epoch;
    o.temp = temp;
    o.hum = 50;
    o.pressure = 1013.2f;
    o.avg = avg;
    o.gust = gust;
    o.dir = dir;
    o.rain = rain;
    o.battery = 2.6f;
    return o;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_bucket_flushes_on_boundary() {
    Aggregator a;
    Sample s;
    TEST_ASSERT_FALSE(a.add(obs(kT0, 10, 2, 3, 90), s));
    TEST_ASSERT_FALSE(a.add(obs(kT0 + 60, 20, 4, 5, 90), s));
    TEST_ASSERT_TRUE(a.add(obs(kT0 + 300, 30, 1, 1, 90), s));
    TEST_ASSERT_EQUAL_UINT32(kT0 / 300 * 300, s.epoch);
    TEST_ASSERT_EQUAL_INT16(1500, s.temp100);
    TEST_ASSERT_EQUAL_UINT16(300, s.wind100);
    TEST_ASSERT_EQUAL_UINT16(500, s.gust100);
    TEST_ASSERT_EQUAL_UINT16(10132, s.press10);
    TEST_ASSERT_EQUAL_UINT16(2600, s.batteryMv);
}

void test_rain_is_summed() {
    Aggregator a;
    Sample s;
    a.add(obs(kT0, 10, 1, 1, 0, 0.25f), s);
    a.add(obs(kT0 + 60, 10, 1, 1, 0, 0.50f), s);
    TEST_ASSERT_TRUE(a.add(obs(kT0 + 300, 10, 1, 1, 0), s));
    TEST_ASSERT_EQUAL_UINT16(75, s.rain100);
}

void test_direction_averages_across_north() {
    Aggregator a;
    Sample s;
    a.add(obs(kT0, 10, 2, 2, 350), s);
    a.add(obs(kT0 + 60, 10, 2, 2, 10), s);
    TEST_ASSERT_TRUE(a.add(obs(kT0 + 300, 10, 2, 2, 0), s));
    TEST_ASSERT_TRUE(s.dir <= 1 || s.dir >= 359);
}

void test_late_packet_is_ignored() {
    Aggregator a;
    Sample s;
    a.add(obs(kT0 + 300, 10, 1, 1, 0), s);
    TEST_ASSERT_FALSE(a.add(obs(kT0, 99, 1, 1, 0), s));
    TEST_ASSERT_TRUE(a.add(obs(kT0 + 600, 10, 1, 1, 0), s));
    TEST_ASSERT_EQUAL_INT16(1000, s.temp100);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_bucket_flushes_on_boundary);
    RUN_TEST(test_rain_is_summed);
    RUN_TEST(test_direction_averages_across_north);
    RUN_TEST(test_late_packet_is_ignored);
    return UNITY_END();
}
