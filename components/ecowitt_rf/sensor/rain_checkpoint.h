// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstring>
#include "fineoffset.h"

namespace esphome::ecowitt_rf {

// Separate, versioned NVS key: existing config/calendar preference layouts are unchanged.
// These windows use cal_total, NOT the raw or gain-scaled sensor lifetime counter.
struct RainCheckpoint {
    static constexpr uint32_t VERSION = 0x57535201;
    uint32_t version = VERSION, key = 0, unix_time = 0;
    float last_counter = -1, last_raw = -1, cal_total = 0, hour_mm = 0;
    float event_mm = 0, day_mm = 0, week_mm = 0, month_mm = 0, year_mm = 0;
    uint32_t last_rain_unix = 0;
    uint16_t mark_doy = 0, mark_year = 0;
    uint8_t mark_hour = 0xFF, mark_week = 0xFF, mark_month = 0xFF;
    RainWindow24h::State rolling;
    RainWindow1h::State hourly;
    RainWindow10m::State recent;
    uint32_t checksum = 0;

    static void hash_word(uint32_t &h, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) { h = (h ^ (value & 255)) * 16777619UL; value >>= 8; }
    }
    static void hash_float(uint32_t &h, float value) {
        uint32_t bits; std::memcpy(&bits, &value, sizeof(bits)); hash_word(h, bits);
    }
    template<typename State> static void hash_window(uint32_t &h, const State &s) {
        for (auto amount : s.amounts) hash_word(h, amount);
        hash_word(h, s.previous_counter); hash_word(h, s.phase_ms);
        hash_word(h, s.head); hash_word(h, s.initialized);
    }
    uint32_t digest() const {
        uint32_t h = 2166136261UL;
        hash_word(h, version); hash_word(h, key); hash_word(h, unix_time);
        hash_float(h, last_counter); hash_float(h, last_raw);
        hash_float(h, cal_total); hash_float(h, hour_mm);
        hash_float(h, event_mm); hash_float(h, day_mm); hash_float(h, week_mm);
        hash_float(h, month_mm); hash_float(h, year_mm); hash_word(h, last_rain_unix);
        hash_word(h, mark_doy); hash_word(h, mark_year); hash_word(h, mark_hour);
        hash_word(h, mark_week); hash_word(h, mark_month);
        hash_window(h, rolling); hash_window(h, hourly); hash_window(h, recent);
        return h;
    }
    bool valid() const {
        return version == VERSION && key && unix_time >= 1704067200UL && checksum == digest() &&
            std::isfinite(last_counter) && last_counter >= 0 && std::isfinite(last_raw) && last_raw >= 0 &&
            std::isfinite(cal_total) && cal_total >= 0 && std::isfinite(hour_mm) && hour_mm >= 0 &&
            std::isfinite(event_mm) && event_mm >= 0 && std::isfinite(day_mm) && day_mm >= 0 &&
            std::isfinite(week_mm) && week_mm >= 0 && std::isfinite(month_mm) && month_mm >= 0 &&
            std::isfinite(year_mm) && year_mm >= 0 &&
            rolling.initialized == 1 && rolling.head < RainWindow24h::SLOTS && rolling.phase_ms < RainWindow24h::SLOT_MS &&
            hourly.initialized == 1 && hourly.head < RainWindow1h::SLOTS && hourly.phase_ms < RainWindow1h::SLOT_MS &&
            recent.initialized == 1 && recent.head < RainWindow10m::SLOTS && recent.phase_ms < RainWindow10m::SLOT_MS;
    }
    static RainCheckpoint capture(const RainAggregator::Bucket &b, uint32_t now_unix, uint32_t now_ms) {
        RainCheckpoint c;
        c.key = b.key; c.last_counter = b.last_counter; c.last_raw = b.last_raw;
        c.cal_total = b.cal_total; c.hour_mm = b.hour_mm;
        c.event_mm = b.event_mm; c.day_mm = b.day_mm; c.week_mm = b.week_mm;
        c.month_mm = b.month_mm; c.year_mm = b.year_mm; c.last_rain_unix = b.last_rain_unix;
        c.mark_doy = b.mark_doy; c.mark_year = b.mark_year; c.mark_hour = b.mark_hour;
        c.mark_week = b.mark_week; c.mark_month = b.mark_month;
        uint32_t age = (now_ms - b.last_sample_ms) / 1000;
        c.unix_time = now_unix >= age ? now_unix - age : 0;
        c.rolling = b.rolling.state(); c.hourly = b.hourly.state(); c.recent = b.recent.state();
        c.checksum = c.digest();
        return c;
    }
    template<typename Legacy> bool matches_calendar(const Legacy &old) const {
        // An older firmware can keep updating the legacy key after rollback. Also
        // reject an interrupted multi-key save; never overwrite newer calendar data
        // with a stale rolling-window baseline on the next public reinstall.
        return key == old.key && last_counter == old.last_counter &&
            event_mm == old.event_mm && day_mm == old.day_mm && week_mm == old.week_mm &&
            month_mm == old.month_mm && year_mm == old.year_mm && mark_doy == old.mark_doy &&
            mark_month == old.mark_month && mark_year == old.mark_year;
    }
    void seed(RainAggregator::Bucket &b, float current_gain) const {
        // One atomic NVS blob owns the complete matching baseline and calendar.
        // Never combine a newer legacy key with older window counters after power loss.
        b = RainAggregator::Bucket{};
        b.key = key; b.last_raw = last_raw; b.last_counter = last_raw * current_gain;
        b.cal_total = cal_total; b.event_mm = event_mm; b.hour_mm = hour_mm;
        b.day_mm = day_mm; b.week_mm = week_mm; b.month_mm = month_mm; b.year_mm = year_mm;
        b.last_rain_unix = last_rain_unix; b.mark_doy = mark_doy; b.mark_year = mark_year;
        b.mark_hour = mark_hour; b.mark_week = mark_week; b.mark_month = mark_month;
    }
    bool restore(RainAggregator::Bucket &b, uint32_t now_unix, uint32_t now_ms) const {
        if (!valid() || b.key != key || now_unix < unix_time) return false;
        uint32_t seconds = now_unix - unix_time;
        uint32_t elapsed = seconds >= 90000UL ? 90000000UL : seconds * 1000UL;
        if (!b.rolling.restore(rolling, elapsed, now_ms) ||
            !b.hourly.restore(hourly, elapsed, now_ms) || !b.recent.restore(recent, elapsed, now_ms)) return false;
        b.last_raw = last_raw; b.cal_total = cal_total; b.last_sample_ms = now_ms;
        // Runtime coverage is carried by the separate continuity checkpoint when present.
        // Even an older partial checkpoint gains known observation time during downtime;
        // a changed/reset counter on the next RF packet invalidates this proof again.
        b.add_event_coverage(elapsed,now_ms);
        return true;
    }
};
static_assert(sizeof(RainCheckpoint) < 256, "bounded primary rain NVS snapshot");

}  // namespace esphome::ecowitt_rf
