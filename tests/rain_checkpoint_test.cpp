// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "rain_checkpoint.h"
using namespace esphome::ecowitt_rf;
static void eq(float a, float b) { assert(std::fabs(a - b) < 0.02f); }
static constexpr uint32_t epoch = 1792141200;
static constexpr uint8_t fam = (uint8_t)Family::WS90;
static void sample(RainAggregator &rain, RainAggregate &a, float raw, uint32_t ms, uint32_t unix_time) {
    rain.sample(fam, 1, raw, 289, 9, rain_week_key(2026, 289), 10, 2026, ms, a, unix_time);
}
static RainAggregator reboot(const RainCheckpoint &c, uint32_t when, float gain = 1) {
    RainAggregator r; r.set_gain(gain); r.set_count(1); c.seed(r.buckets()[0], gain);
    assert(c.restore(r.buckets()[0], when, 1000));
    return r;
}
int main() {
    RainAggregator r; RainAggregate a;
    sample(r, a, 139, 1000, epoch);
    sample(r, a, 140.2f, 10000, epoch + 9);
    eq(a.last24h_mm, 1.2f); eq(a.last1h_mm, 1.2f); eq(a.rate_mmh, 7.2f);
    auto c = RainCheckpoint::capture(r.buckets()[0], epoch + 14, 15000);
    assert(c.valid() && c.unix_time == epoch + 9); // anchor to LAST RF sample, not flash-write time
    // Round trip the complete blob as NVS does. No Arduino/clock state survives reboot.
    unsigned char flash[sizeof(c)]; std::memcpy(flash, &c, sizeof(c));
    RainCheckpoint loaded; std::memcpy(&loaded, flash, sizeof(loaded)); assert(loaded.valid());
    struct Legacy {
        uint32_t key; float last_counter, event_mm, day_mm, week_mm, month_mm, year_mm;
        uint16_t mark_doy, mark_year; uint8_t mark_month;
    } old{c.key,c.last_counter,c.event_mm,c.day_mm,c.week_mm,c.month_mm,c.year_mm,
          c.mark_doy,c.mark_year,c.mark_month};
    assert(c.matches_calendar(old));
    old.day_mm += 0.1f; assert(!c.matches_calendar(old)); // rollback firmware has newer history
    old.day_mm = c.day_mm; old.key++; assert(!c.matches_calendar(old));
    auto recovered = reboot(loaded, epoch + 30);
    sample(recovered, a, 140.2f, 2000, epoch + 31);
    eq(a.last24h_mm, 1.2f); eq(a.last1h_mm, 1.2f); eq(a.rate_mmh, 7.2f);
    eq(a.year_mm, 1.2f); // lifetime 140.2 must not seed the current year
    sample(recovered, a, 140.5f, 11000, epoch + 40);
    eq(a.last24h_mm, 1.5f); eq(a.day_mm, 1.5f);
    auto ten_minutes = reboot(c, epoch + 620);
    sample(ten_minutes, a, 140.2f, 2000, epoch + 621);
    eq(a.rate_mmh, 0); eq(a.last1h_mm, 1.2f); eq(a.last24h_mm, 1.2f);
    auto two_hours = reboot(c, epoch + 7200);
    sample(two_hours, a, 140.2f, 2000, epoch + 7201);
    eq(a.rate_mmh, 0); eq(a.last1h_mm, 0); eq(a.last24h_mm, 1.2f);
    auto two_days = reboot(c, epoch + 172800);
    sample(two_days, a, 140.2f, 2000, epoch + 172801);
    eq(a.last24h_mm, 0); eq(a.rate_mmh, 0); eq(a.event_mm, 0);
    auto gain = reboot(c, epoch + 30, 2);
    sample(gain, a, 140.2f, 2000, epoch + 31);
    eq(a.last24h_mm, 1.2f); eq(a.year_mm, 1.2f);
    sample(gain, a, 140.5f, 11000, epoch + 40);
    eq(a.last24h_mm, 1.8f); eq(a.year_mm, 1.8f);
    sample(gain, a, 0, 12000, epoch + 41); // sensor counter reset, no loss/negative rain
    eq(a.last24h_mm, 1.8f);
    sample(gain, a, 0.2f, 13000, epoch + 42); eq(a.last24h_mm, 2.2f);
    RainAggregator pending; pending.set_count(1); c.seed(pending.buckets()[0], 1);
    assert(!c.restore(pending.buckets()[0], 0, 100)); // NTP not ready: no mutation
    eq(pending.buckets()[0].last_raw, 140.2f);
    assert(!c.restore(pending.buckets()[0], epoch - 1, 100));
    pending.buckets()[0].key++; assert(!c.restore(pending.buckets()[0], epoch + 10, 100));
    auto corrupt = c; corrupt.cal_total += 1; assert(!corrupt.valid());
    corrupt = c; corrupt.version++; corrupt.checksum = corrupt.digest(); assert(!corrupt.valid());
    corrupt = c; corrupt.recent.head = 255; corrupt.checksum = corrupt.digest(); assert(!corrupt.valid());
    corrupt = c; corrupt.last_raw = NAN; corrupt.checksum = corrupt.digest(); assert(!corrupt.valid());
    // Another sensor must retain an independent raw/calibrated window baseline.
    auto multiple = reboot(c, epoch + 30);
    multiple.sample(fam, 2, 999, 289, 9, rain_week_key(2026, 289), 10, 2026, 2000, a, epoch + 31);
    eq(a.last24h_mm, 0); sample(multiple, a, 140.2f, 3000, epoch + 32); eq(a.last24h_mm, 1.2f);
    std::puts("Rain NVS checkpoint, elapsed-time expiry, gain, reset, corruption and source isolation passed");
}
