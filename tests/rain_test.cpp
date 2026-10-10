// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "../components/ecowitt_rf/sensor/fineoffset.h"

using namespace esphome::ecowitt_rf;
static void eq(float actual, float expected) {
    if (std::fabs(actual - expected) > 0.02f) {
        std::fprintf(stderr, "actual %.4f != expected %.4f\n", actual, expected);
        std::abort();
    }
}

int main() {
    // Continuous RF reports formerly overwrote the only baseline forever.
    RainWindow24h window;
    eq(window.sample(100, 0), 0);
    for (uint32_t t = 8500; t < 3600000; t += 8500)
        eq(window.sample(t < 60000 ? 100 : 100.3f, t), t < 60000 ? 0 : 0.3f);
    eq(window.sample(101.3f, 3600000), 1.3f);
    // Cross local midnight: the elapsed-time window does not reset.
    eq(window.sample(101.3f, 12 * 3600000UL), 1.3f);
    // First rain expires at its half-hour boundary, later rain stays.
    eq(window.sample(101.3f, 49 * RainWindow24h::SLOT_MS), 1.0f);
    eq(window.sample(101.3f, 51 * RainWindow24h::SLOT_MS), 0);

    RainWindow24h reset;
    reset.sample(250, 1000);
    eq(reset.sample(250.5f, 2000), 0.5f);
    eq(reset.sample(0, 3000), 0.5f);  // sensor resets, real recent rain remains
    eq(reset.sample(0.2f, 4000), 0.7f);
    eq(reset.sample(NAN, 5000), 0.7f);
    eq(reset.sample(-1, 6000), 0.7f);
    eq(reset.sample(0.2f, 3 * 86400000UL), 0);  // long dry/reporting gap
    eq(reset.sample(10.2f, 6 * 86400000UL), 0); // do not label old, undated rain as recent

    RainWindow24h wrap;
    wrap.sample(10, UINT32_MAX - 1000);
    eq(wrap.sample(10.3f, 2000), 0.3f);
    eq(wrap.sample(10.3f, 90000000UL), 0);

    RainWindow24h calibration;
    calibration.sample(100, 0);
    for (unsigned i = 1; i <= 8; ++i)
        calibration.sample(100 + i * 0.125f, i * 1000);
    eq(calibration.sample(101, 10000), 1);  // no per-delta rounding drift

    RainAggregator rain;
    RainAggregate a;
    auto sample = [&](uint32_t id, float counter, uint32_t t, uint16_t day = 249) {
        rain.sample((uint8_t)Family::WS90, id, counter, day, 6, rain_week_key(2026, day), 9, 2026, t, a);
    };
    sample(1, 100, 1000);
    sample(2, 500, 1500);  // independent sensor, not another 400mm of rainfall
    sample(1, 100.3f, 10000);
    eq(a.last24h_mm, 0.3f);
    assert(a.rate_mmh > 0);
    sample(2, 500, 11000);
    eq(a.last24h_mm, 0);
    sample(1, 100.3f, 19000);
    eq(a.rate_mmh, 1.8f);  // persists between RF reports and scheduled HTTP uploads
    sample(1, 100.3f, 610000);
    eq(a.rate_mmh, 0);  // clears after the short window, despite a wet-surface flag
    sample(1, 100.3f, 12 * 3600000UL, 250);
    eq(a.day_mm, 0);
    eq(a.last24h_mm, 0.3f);
    rain.reset();
    sample(3, 200, 13 * 3600000UL);
    eq(a.last24h_mm, 0);
    eq(a.day_mm, 0);

    // Restored totals survive RF reception before NTP, then same-day clock sync.
    RainAggregator resumed;
    resumed.set_count(1);
    auto &b = resumed.buckets()[0];
    b.key = ((uint32_t)Family::WS90 << 24) | 1;
    b.last_counter = 111.1f;
    b.event_mm = b.day_mm = b.week_mm = b.month_mm = 0.3f;
    b.year_mm = 111.1f;
    b.restore_calendar(249, 9, 2026);
    resumed.sample((uint8_t)Family::WS90, 1, 111.1f, 0, 0, 0, 0, 0, 1000, a);
    eq(a.day_mm, 0.3f); eq(a.month_mm, 0.3f); eq(a.year_mm, 111.1f);
    resumed.sample((uint8_t)Family::WS90, 1, 111.1f, 249, 14, rain_week_key(2026, 249), 9, 2026, 2000, a);
    eq(a.day_mm, 0.3f); eq(a.week_mm, 0.3f); eq(a.month_mm, 0.3f);
    resumed.sample((uint8_t)Family::WS90, 1, 111.1f, 250, 0, rain_week_key(2026, 250), 9, 2026, 3000, a);
    eq(a.day_mm, 0); eq(a.week_mm, 0.3f); eq(a.month_mm, 0.3f);

    // WS90's wet-surface flag is independent of measurable rainfall; preserve it.
    uint8_t packet[32] = {0x90, 0, 0x3f, 0x0c};
    packet[16] = 0x10;
    packet[19] = 0x04; packet[20] = 0x57;  // 111.1mm lifetime counter
    packet[30] = crc8(0x31, packet, 30);
    packet[31] = add_sum(packet, 31);
    Reading reading;
    assert(decode_ws90(packet, sizeof(packet), reading));
    assert(reading.rain_state);
    eq(reading.rain_mm, 111.1f);
    eq(reading.rain_rate_mmh, 0);
    // Weekly rain rolls on Sunday (stock behaviour), not on the weekday of 1 January.
    // 2026-09-21 (doy 264) and 2026-09-27 (doy 270) are Sundays; 2026-09-24 was a Thursday.
    assert(rain_week_key(2026, 264) == rain_week_key(2026, 269));
    assert(rain_week_key(2026, 269) != rain_week_key(2026, 270));
    assert(rain_week_key(2026, 267) == rain_week_key(2026, 266));  // Thu == Wed
    // The week spanning New Year keeps one key: Thu 2026-12-31 and Fri 2027-01-01.
    assert(rain_week_key(2026, 365) == rain_week_key(2027, 1));
    assert(rain_week_key(0, 10) == 0xFF);

    // A month boundary mid-week clears the month but keeps the week.
    RainAggregator periods;
    auto at = [&](float counter, uint16_t doy, uint8_t hour, uint8_t month, uint16_t year,
                  uint32_t ms, uint32_t unix_s) {
        periods.sample((uint8_t)Family::WS90, 7, counter, doy, hour, rain_week_key(year, doy),
                       month, year, ms, a, unix_s);
    };
    at(10.0f, 272, 10, 9, 2026, 1000, 1790000000);    // Tue 29 Sep
    at(10.5f, 272, 11, 9, 2026, 2000, 1790003600);    // 0.5 mm
    eq(a.week_mm, 0.5f); eq(a.month_mm, 0.5f);
    at(10.7f, 274, 9, 10, 2026, 3000, 1790170000);    // Thu 1 Oct, +0.2 mm
    eq(a.month_mm, 0.2f); eq(a.week_mm, 0.7f); eq(a.day_mm, 0.2f);
    at(10.7f, 277, 9, 10, 2026, 4000, 1790430000);    // Sun 4 Oct: new week
    eq(a.week_mm, 0); eq(a.month_mm, 0.2f);

    // Event rain clears after 24 h dry on the wall clock, even across reboots (millis reset).
    eq(a.event_mm, 0);  // > 24 h since the last increment at 1790170000
    RainAggregator evt;
    evt.set_count(1);
    auto &eb = evt.buckets()[0];
    eb.key = ((uint32_t)Family::WS90 << 24) | 9;
    eb.last_counter = 50.0f;
    eb.event_mm = 28.8f;
    eb.year_mm = 50.0f;
    eb.restore_calendar(267, 9, 2026);
    // Undated restored event (older flash): starts its dry clock on first valid time.
    evt.sample((uint8_t)Family::WS90, 9, 50.0f, 269, 17, rain_week_key(2026, 269), 9, 2026, 500, a, 1790437000);
    eq(a.event_mm, 28.8f);
    evt.sample((uint8_t)Family::WS90, 9, 50.0f, 270, 17, rain_week_key(2026, 270), 9, 2026, 900, a, 1790437000 + 86400);
    eq(a.event_mm, 0);
    // Dated event: rain at T, reboot (millis small), still cleared at T + 24 h.
    evt.sample((uint8_t)Family::WS90, 9, 50.4f, 270, 18, rain_week_key(2026, 270), 9, 2026, 1000, a, 1790530000);
    eq(a.event_mm, 0.4f);
    evt.sample((uint8_t)Family::WS90, 9, 50.4f, 271, 17, rain_week_key(2026, 271), 9, 2026, 50, a, 1790530000 + 80000);
    eq(a.event_mm, 0.4f);
    evt.sample((uint8_t)Family::WS90, 9, 50.4f, 271, 19, rain_week_key(2026, 271), 9, 2026, 60, a, 1790530000 + 86400);
    eq(a.event_mm, 0);

    // cal_rain regression: a gain change must never create rain. The gain used to multiply
    // the LIFETIME counter before aggregation, so 1.0 -> 1.1 on a 500 mm counter injected
    // +50 mm into every total and window.
    {
        RainAggregator g;
        auto s = [&](float raw, uint32_t t, uint16_t doy = 249) {
            g.sample((uint8_t)Family::WS90, 5, raw, doy, 10, rain_week_key(2026, doy), 9, 2026, t, a,
                     1790000000 + t / 1000);
        };
        s(500.0f, 1000);
        s(500.5f, 2000);
        eq(a.day_mm, 0.5f); eq(a.year_mm, 0.5f); eq(a.last24h_mm, 0.5f); eq(a.last1h_mm, 0.5f);
        g.set_gain(1.1f);
        assert(g.take_rolled());   // the re-labelled counter must be persisted at once
        eq(g.buckets()[0].last_counter, 550.55f);
        s(500.5f, 3000);           // same raw counter after the change: no phantom rain
        eq(a.day_mm, 0.5f); eq(a.week_mm, 0.5f); eq(a.month_mm, 0.5f); eq(a.year_mm, 0.5f);
        eq(a.event_mm, 0.5f); eq(a.last24h_mm, 0.5f); eq(a.last1h_mm, 0.5f);
        s(501.5f, 4000);           // new rain is scaled by the new gain
        eq(a.day_mm, 1.6f); eq(a.year_mm, 1.6f); eq(a.last24h_mm, 1.6f);
        g.set_gain(1.0f);
        s(501.5f, 5000);
        eq(a.day_mm, 1.6f); eq(a.last24h_mm, 1.6f);
        eq(g.buckets()[0].last_counter, 501.5f);
        // The rolling past hour (Wunderground rainin) drops rain older than an hour.
        s(501.5f, 2 * 3600000UL);
        eq(a.last1h_mm, 0); eq(a.last24h_mm, 1.6f);
        // An invalid counter keeps every baseline.
        s(NAN, 2 * 3600000UL + 1000);
        eq(a.day_mm, 1.6f);
        s(501.7f, 2 * 3600000UL + 2000);
        eq(a.day_mm, 1.8f);
    }
    // A bucket restored from flash under gain 1.1 (last_counter = raw * 1.1) resumes without
    // a delta when the sensor reports the same raw counter.
    {
        RainAggregator g;
        g.set_gain(1.1f);
        g.set_count(1);
        auto &rb = g.buckets()[0];
        rb.key = ((uint32_t)Family::WS90 << 24) | 6;
        rb.last_counter = 110.0f;   // raw 100.0
        rb.day_mm = 2.0f; rb.year_mm = 40.0f;
        rb.restore_calendar(249, 9, 2026);
        g.sample((uint8_t)Family::WS90, 6, 100.0f, 249, 10, rain_week_key(2026, 249), 9, 2026, 1000, a);
        eq(a.day_mm, 2.0f); eq(a.year_mm, 40.0f);
        g.sample((uint8_t)Family::WS90, 6, 101.0f, 249, 10, rain_week_key(2026, 249), 9, 2026, 2000, a);
        eq(a.day_mm, 3.1f); eq(a.year_mm, 41.1f);
    }

    // YEARLY must not be seeded to the lifetime counter on a fresh bucket (it read == TOTAL
    // until 1 January). Restored buckets keep their saved year_mm (tested above).
    {
        RainAggregator fresh;
        fresh.sample((uint8_t)Family::WS90, 8, 734.2f, 269, 12, rain_week_key(2026, 269), 9, 2026, 1000, a);
        eq(a.year_mm, 0); eq(a.month_mm, 0); eq(a.day_mm, 0);
        fresh.sample((uint8_t)Family::WS90, 8, 734.5f, 269, 12, rain_week_key(2026, 269), 9, 2026, 2000, a);
        eq(a.year_mm, 0.3f);
        eq(fresh.buckets()[0].last_counter, 734.5f);   // lifetime total is still the counter
        // Clock not yet synchronised on the first sample: still no seeding.
        RainAggregator nosync;
        nosync.sample((uint8_t)Family::WS90, 8, 734.2f, 0, 0, 0, 0, 0, 1000, a);
        eq(a.year_mm, 0);
        nosync.sample((uint8_t)Family::WS90, 8, 734.2f, 269, 12, rain_week_key(2026, 269), 9, 2026, 2000, a);
        eq(a.year_mm, 0);
    }

    // The primary sensor's bucket can be moved to slot 0 (the persisted / LCD slot).
    {
        RainAggregator p;
        p.sample((uint8_t)Family::WH40, 1, 10.0f, 269, 12, rain_week_key(2026, 269), 9, 2026, 1000, a);
        p.sample((uint8_t)Family::WS90, 2, 20.0f, 269, 12, rain_week_key(2026, 269), 9, 2026, 1000, a);
        p.sample((uint8_t)Family::WS90, 2, 20.4f, 269, 12, rain_week_key(2026, 269), 9, 2026, 2000, a);
        const uint32_t k2 = ((uint32_t)Family::WS90 << 24) | 2;
        assert(p.buckets()[0].key != k2);
        assert(p.promote(k2));
        assert(p.buckets()[0].key == k2);
        eq(p.buckets()[0].day_mm, 0.4f);
        assert(!p.promote(k2));
        p.sample((uint8_t)Family::WS90, 2, 20.6f, 269, 12, rain_week_key(2026, 269), 9, 2026, 3000, a);
        eq(a.day_mm, 0.6f);
        eq(p.buckets()[1].day_mm, 0);   // the other gauge is untouched
    }

    static_assert(sizeof(RainWindow24h) <= 116, "ESP8266 window memory budget");
    std::printf("Rain regressions passed; window=%zu bytes, aggregator=%zu bytes\n",
                sizeof(RainWindow24h), sizeof(RainAggregator));
}
