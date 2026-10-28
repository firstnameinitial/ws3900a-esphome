// SPDX-License-Identifier: GPL-3.0-only
#include "forecast_history.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace esphome::ecowitt_rf;
constexpr uint32_t START = 1767225600; // exact UTC midnight, 1 Jan 2026

int main() {
    ForecastHistory h;
    assert(sizeof(h) < 2048 && h.forecast_state(10) == 1);
    assert(h.waiting_for_clock(0) && !h.sample(1000, 0));
    assert(!h.sample(NAN, START) && !h.sample(INFINITY, START));
    assert(!h.sample(299, START) && !h.sample(1201, START));
    assert(h.trend_level(1000, START) == -1);
    assert(h.sample(1000, START));
    assert(h.trend_level(1000, START) == 5 && !h.trained(START));
    assert(h.trend_level(1e30f, START)==-1 && h.trend_level(-1e30f, START)==-1);
    assert(!h.sample(1001, START + 10)); // one actual observation per ten-minute bucket
    assert(!h.sample(1001, START - 10)); // backward clock cannot rewrite history
    assert(h.waiting_for_clock(START - 1));
    h.seal(); assert(h.valid());
    ForecastHistory restored;
    std::memcpy(&restored, &h, sizeof(h)); assert(restored.valid());
    restored.ten.entries[0].pressure10++; assert(!restored.valid());
    restored = h; restored.hour_samples = 7; restored.seal(); assert(!restored.valid());
    restored = h; restored.hour_sum = 1; restored.seal(); assert(!restored.valid());
    restored = h; restored.base_state = 255; restored.seal(); assert(!restored.valid());
    ForecastHistory empty;
    empty.seal(); assert(empty.valid());
    empty.ten.head = 6; empty.seal(); assert(!empty.valid());
    restored = h; restored.ten.entries[0].at -= 600; restored.seal(); assert(!restored.valid());
    restored = h; restored.day_sum = 3000; restored.seal(); assert(!restored.valid());
    restored = h; restored.hour_samples = 0; restored.seal(); assert(!restored.valid());

    // Three observed days with gaps: no fake hour/day entries for the outage.
    ForecastHistory sparse;
    sparse.sample(1000, START);
    sparse.sample(1002, START + 600);
    sparse.sample(1004, START + 3 * ForecastHistory::HOUR);
    assert(sparse.hourly.size == 1 && sparse.hourly.newest(0).pressure10 == 10010);
    sparse.sample(1006, START + 2 * ForecastHistory::DAY);
    assert(sparse.daily.size == 1 && sparse.hourly.size == 2);
    assert(sparse.daily.newest(0).pressure10 == 10025);
    assert(sparse.ten.size == 1 && !sparse.trained(START + 2 * ForecastHistory::DAY));
    sparse.seal(); assert(sparse.valid());

    // Midnight/gaps: complete the old hour into its own UTC day before
    // rolling the day; average actual hourly means, with no absent-day fill.
    ForecastHistory calendar;
    calendar.sample(1000, START + 23*3600 + 40*60);
    calendar.sample(1002, START + 23*3600 + 50*60);
    calendar.sample(1010, START + ForecastHistory::DAY);
    assert(calendar.daily.size==1 && calendar.daily.newest(0).pressure10==10010);
    assert(calendar.daily.newest(0).samples==1);
    calendar.sample(1012, START + ForecastHistory::DAY + 600);
    calendar.sample(1020, START + ForecastHistory::DAY + 23*3600);
    calendar.sample(1030, START + 3*ForecastHistory::DAY + 3600);
    assert(calendar.daily.size==2 && calendar.daily.newest(0).pressure10==10155);
    assert(calendar.daily.newest(0).samples==2);
    assert(calendar.daily.newest(0).at==START+ForecastHistory::DAY+23*3600);
    assert(calendar.hour_samples==1 && calendar.hour_sum==10300 && calendar.day_samples==0);
    calendar.seal(); assert(calendar.valid());
    ForecastHistory calendar_copy = calendar;
    assert(calendar_copy.valid()); // value-copy CRC remains stable: no implicit padding

    // True 30-day daily mean, rather than the old three-hour trend reference.
    ForecastHistory full;
    for (uint32_t day = 0; day <= 30; ++day)
        for (uint32_t minute = 0; minute < 1440; minute += 10)
            full.sample(1000, START + day * ForecastHistory::DAY + minute * 60);
    const uint32_t now = START + 30 * ForecastHistory::DAY + 1430 * 60;
    assert(full.daily.size == 30 && full.trained(now));
    assert(full.baseline10(now, 30 * ForecastHistory::DAY) == 10000);
    assert(full.trend_level(1005, now) == 0 && full.trend_level(995, now) == 10);
    full.seal(); assert(full.valid());
    assert(full.sample(1001, now + 31 * ForecastHistory::DAY));
    assert(full.daily.size == 0 && full.hourly.size == 0 && full.ten.size == 1);
    assert(full.forecast_state(10) == 1 && !full.trained(now + 31 * ForecastHistory::DAY));

    // Synthetic classifier conditions use real timestamps; hysteresis preserves state.
    ForecastHistory classifier;
    const uint32_t t = START + 20 * ForecastHistory::DAY;
    classifier.daily.push(t - ForecastHistory::DAY, 10000, 24);
    classifier.hourly.push(t - 1800, 10050, 6);
    classifier.classify(t); assert(classifier.forecast_state(10) == 0);
    classifier.hourly.entries[0].pressure10 = 10047;
    classifier.classify(t); assert(classifier.forecast_state(10) == 0);
    classifier.hourly.entries[0].pressure10 = 10010;
    classifier.classify(t); assert(classifier.forecast_state(10) == 1);
    classifier.hourly.entries[0].pressure10 = 9980;
    classifier.classify(t); assert(classifier.forecast_state(10) == 2);
    classifier.hourly.entries[0].pressure10 = 9960;
    classifier.classify(t); assert(classifier.forecast_state(10) == 3);
    assert(classifier.forecast_state(0) == 4 && classifier.forecast_state(1) == 3);
    classifier.base_state = 5;
    assert(classifier.forecast_state(-1) == 6 && classifier.forecast_state(1) == 5);

    // Storm requires BOTH a six-hPa short/24h fall and a four-hPa lower minimum.
    ForecastHistory storm;
    storm.daily.push(t - ForecastHistory::DAY, 10000, 24);
    for (unsigned i = 0; i < 24; ++i) storm.hourly.push(t - (24 - i) * 3600, 10000, 6);
    for (unsigned i = 0; i < 6; ++i) storm.ten.push(t - (5 - i) * 600, 9920, 1);
    storm.classify(t); assert(storm.forecast_state(10) == 5);
    storm.ten.size = 5;
    storm.base_state = 1;
    storm.classify(t); assert(storm.forecast_state(10) != 5);
    puts("forecast: stock cold state, timestamped rings, gaps/aging/clock/corruption, 30-day trend and classifier passed");
}
