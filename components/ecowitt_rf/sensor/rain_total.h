// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "rain_checkpoint.h"

namespace esphome::ecowitt_rf {

// Separate preference key: never extend the original calendar/window blobs.
struct RainTotalCheckpoint {
    static constexpr uint32_t VERSION = 0x57535401;
    enum : uint8_t { INITIALIZED=1, MIGRATED=2, HISTORY_INCOMPLETE=4, CALENDAR_INITIALIZED=8, CALENDAR_REBASED=16 };
    uint32_t version = VERSION, key = 0;
    float total_mm = 0, cal_total = 0, last_raw = -1;
    uint32_t legacy_digest = 0, event_coverage_ms = 0;
    struct PeriodState {
        uint32_t day = 0, hour = 0, week = 0, month = 0, season = 0;
        uint8_t valid = 0; // Serialized bytes must never be read as an unchecked bool.
    } periods;
    RainCalendarPolicy policy;
    uint8_t flags = 0;
    uint32_t checksum = 0;

    static uint32_t calendar_digest(const RainAggregator::Bucket &b) {
        uint32_t h = 2166136261UL;
        RainCheckpoint::hash_word(h,b.key); RainCheckpoint::hash_float(h,b.last_counter);
        RainCheckpoint::hash_float(h,b.event_mm); RainCheckpoint::hash_float(h,b.day_mm);
        RainCheckpoint::hash_float(h,b.week_mm); RainCheckpoint::hash_float(h,b.month_mm);
        RainCheckpoint::hash_float(h,b.year_mm); RainCheckpoint::hash_word(h,b.mark_doy);
        RainCheckpoint::hash_word(h,b.mark_month); RainCheckpoint::hash_word(h,b.mark_year);
        return h;
    }
    uint32_t digest() const {
        uint32_t h = 2166136261UL;
        RainCheckpoint::hash_word(h,version); RainCheckpoint::hash_word(h,key);
        RainCheckpoint::hash_float(h,total_mm); RainCheckpoint::hash_float(h,cal_total);
        RainCheckpoint::hash_float(h,last_raw); RainCheckpoint::hash_word(h,legacy_digest);
        RainCheckpoint::hash_word(h,event_coverage_ms);
        RainCheckpoint::hash_word(h,periods.day); RainCheckpoint::hash_word(h,periods.hour);
        RainCheckpoint::hash_word(h,periods.week); RainCheckpoint::hash_word(h,periods.month);
        RainCheckpoint::hash_word(h,periods.season); RainCheckpoint::hash_word(h,periods.valid);
        RainCheckpoint::hash_word(h,policy.day_reset_hour); RainCheckpoint::hash_word(h,policy.week_start);
        RainCheckpoint::hash_word(h,policy.season_start_month); RainCheckpoint::hash_word(h,flags);
        return h;
    }
    bool valid() const {
        return version == VERSION && key && (flags & INITIALIZED) && !(flags & ~31u) &&
            checksum == digest() && policy.valid() && std::isfinite(total_mm) && total_mm >= 0 &&
            std::isfinite(cal_total) && cal_total >= 0 && std::isfinite(last_raw) && last_raw >= 0 &&
            event_coverage_ms <= RainAggregator::Bucket::EVENT_WINDOW_MS && periods.valid <= 1 &&
            (!(flags & CALENDAR_INITIALIZED) || periods.valid);
    }
};
static_assert(sizeof(RainTotalCheckpoint) <= 96, "bounded console-total continuity checkpoint");

class RainConsoleTotal {
 public:
    bool initialized() const { return initialized_; }
    float value() const { return initialized_ ? total_mm_ : NAN; }
    bool migrated() const { return migrated_; }
    bool history_incomplete() const { return history_incomplete_; }

    // Called once only when upgrading existing saved calendar data. Preserve exactly
    // the TOTAL previously visible on the console; YEARLY is not recoverable lifetime data.
    bool migrate_legacy(const RainAggregator::Bucket &b) {
        if (initialized_ || !usable(b) || !std::isfinite(b.last_counter) || b.last_counter < 0) return false;
        initialized_ = migrated_ = history_incomplete_ = true;
        total_mm_ = b.last_counter; bind(b);
        return true;
    }
    // Call only for the selected primary rain source, after aggregation. A new console
    // starts at zero, never at the sensor's unrelated pre-installation counter.
    bool observe(const RainAggregator::Bucket &b) {
        if (!usable(b)) return false;
        if (!initialized_) { initialized_ = true; total_mm_ = 0; bind(b); return true; }
        if (key_ != b.key || b.cal_total < baseline_) {
            history_incomplete_ = true; bind(b); return true;
        }
        const float delta = b.cal_total - baseline_;
        baseline_ = b.cal_total;
        if (delta > 0) { total_mm_ += delta; return true; }
        return false;
    }
    // Seed after the original RainCheckpoint.seed(), before its NTP-dependent restore().
    // A matching rolling checkpoint is mandatory before restoring coverage: empty windows
    // cannot prove past rain was below 1 mm. A stale blob preserves TOTAL but rebases the
    // baseline and marks unrecoverable history instead of guessing or double counting.
    bool restore(const RainTotalCheckpoint &c, RainAggregator::Bucket &b,
                 const RainCalendarPolicy &policy, bool matching_rolling_checkpoint = false) {
        if (!c.valid() || !usable(b)) return false;
        initialized_ = true; migrated_ = c.flags & RainTotalCheckpoint::MIGRATED;
        history_incomplete_ = c.flags & RainTotalCheckpoint::HISTORY_INCOMPLETE;
        total_mm_ = c.total_mm;
        const bool exact = c.key == b.key && c.cal_total == b.cal_total && c.last_raw == b.last_raw;
        const bool calendar_matches = c.legacy_digest == RainTotalCheckpoint::calendar_digest(b);
        const bool legacy = c.key == b.key && b.last_raw < 0 && calendar_matches;
        if (legacy) { b.last_raw = c.last_raw; b.cal_total = c.cal_total; }
        if (exact || legacy) {
            if (matching_rolling_checkpoint) b.event_coverage_ms = c.event_coverage_ms;
            if (c.policy == policy && calendar_matches) {
                b.period_keys = {c.periods.day,c.periods.hour,c.periods.week,c.periods.month,c.periods.season,c.periods.valid == 1};
                b.calendar_initialized = c.flags & RainTotalCheckpoint::CALENDAR_INITIALIZED;
                b.calendar_rebased = c.flags & RainTotalCheckpoint::CALENDAR_REBASED;
            } else if (c.policy == policy) {
                // A dry calendar rollover can change period totals without changing
                // raw/cal_total. Never attach newer period keys to older totals.
                b.period_keys = b.mark_hour < 24 ? rain_calendar_keys(b.mark_year,b.mark_doy,b.mark_hour,policy)
                                                : RainCalendarKeys{};
                b.calendar_initialized = b.period_keys.valid;
                history_incomplete_ = true;
            } else {
                b.calendar_initialized = false; b.calendar_rebased = true;
                history_incomplete_ = true;
            }
        } else history_incomplete_ = true;
        bind(b);
        return true;
    }
    RainTotalCheckpoint checkpoint(const RainAggregator::Bucket &b, const RainCalendarPolicy &policy) const {
        RainTotalCheckpoint c;
        if (!initialized_ || !usable(b) || b.key != key_ || b.cal_total != baseline_) return c;
        c.key = b.key; c.total_mm = total_mm_; c.cal_total = baseline_; c.last_raw = b.last_raw;
        // Legacy-only migration may precede the first RF sample; derive raw only in the
        // caller/aggregator where the saved gain is known. Such a blob stays invalid here.
        c.legacy_digest = RainTotalCheckpoint::calendar_digest(b);
        c.event_coverage_ms = b.event_coverage_ms;
        c.periods = {b.period_keys.day,b.period_keys.hour,b.period_keys.week,b.period_keys.month,b.period_keys.season,
                     static_cast<uint8_t>(b.period_keys.valid)};
        c.policy = policy;
        c.flags = RainTotalCheckpoint::INITIALIZED |
            (migrated_ ? RainTotalCheckpoint::MIGRATED : 0) |
            (history_incomplete_ ? RainTotalCheckpoint::HISTORY_INCOMPLETE : 0) |
            (b.calendar_initialized ? RainTotalCheckpoint::CALENDAR_INITIALIZED : 0) |
            (b.calendar_rebased ? RainTotalCheckpoint::CALENDAR_REBASED : 0);
        c.checksum = c.digest(); return c;
    }
 private:
    static bool usable(const RainAggregator::Bucket &b) {
        return b.key && std::isfinite(b.cal_total) && b.cal_total >= 0;
    }
    void bind(const RainAggregator::Bucket &b) { key_ = b.key; baseline_ = b.cal_total; }
    uint32_t key_ = 0;
    float total_mm_ = 0, baseline_ = 0;
    bool initialized_ = false, migrated_ = false, history_incomplete_ = false;
};
}  // namespace esphome::ecowitt_rf
