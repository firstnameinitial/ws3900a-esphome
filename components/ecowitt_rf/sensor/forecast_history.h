// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace esphome { namespace ecowitt_rf {

// Timestamped observations, not inferred samples for periods when the receiver was
// off. Completed-hour and completed-day means give every observed period equal
// weight. The current partial period is retained in the checkpoint as well.
struct PressureObservation {
    uint32_t at = 0;
    int16_t pressure10 = 0;
    uint16_t samples = 0;
};

template<size_t N> struct PressureHistoryRing {
    PressureObservation entries[N]{};
    uint16_t head = 0, size = 0;
    static constexpr int16_t MISSING = std::numeric_limits<int16_t>::min();

    void push(uint32_t at, int16_t pressure10, uint16_t samples) {
        entries[head] = {at, pressure10, samples};
        head = (head + 1) % N;
        if (size < N) ++size;
    }
    const PressureObservation &newest(size_t offset) const {
        return entries[(head + N - 1 - offset) % N];
    }
    void expire(uint32_t now, uint32_t horizon) {
        while (size && now >= newest(size - 1).at && now - newest(size - 1).at > horizon) --size;
    }
    uint16_t count(uint32_t now, uint32_t horizon, uint32_t older_than = 0) const {
        uint16_t count = 0;
        for (size_t i = 0; i < size; ++i) {
            const auto &e = newest(i);
            if (now >= e.at && now - e.at <= horizon && now - e.at >= older_than) ++count;
        }
        return count;
    }
    int16_t mean(uint32_t now, uint32_t horizon) const {
        int32_t total = 0;
        uint16_t count = 0;
        for (size_t i = 0; i < size; ++i) {
            const auto &e = newest(i);
            if (now >= e.at && now - e.at <= horizon) { total += e.pressure10; ++count; }
        }
        return count ? static_cast<int16_t>(total / count) : MISSING;
    }
    int16_t minimum(uint32_t now, uint32_t horizon, uint32_t older_than = 0) const {
        int16_t minimum = std::numeric_limits<int16_t>::max();
        for (size_t i = 0; i < size; ++i) {
            const auto &e = newest(i);
            if (now >= e.at && now - e.at <= horizon && now - e.at >= older_than &&
                e.pressure10 < minimum) minimum = e.pressure10;
        }
        return minimum == std::numeric_limits<int16_t>::max() ? MISSING : minimum;
    }
    bool valid(uint32_t min_time, uint32_t last_time, uint16_t max_samples) const {
        if (head >= N || size > N) return false;
        uint32_t previous = 0;
        for (size_t i = size; i > 0; --i) {
            const auto &e = newest(i - 1);
            if (e.at < min_time || e.at > last_time || (previous && e.at <= previous) ||
                e.pressure10 < 3000 || e.pressure10 > 12000 || !e.samples ||
                e.samples > max_samples) return false;
            previous = e.at;
        }
        return true;
    }
};

struct ForecastHistory {
    static constexpr uint32_t MAGIC = 0x57465831;
    static constexpr uint32_t MIN_TIME = 1577836800UL; // valid wall clock, 2020 onwards
    static constexpr uint32_t HOUR = 3600, DAY = 86400;
    static constexpr int16_t MISSING = PressureHistoryRing<6>::MISSING;
    uint32_t magic = MAGIC, last_unix = 0;
    PressureHistoryRing<6> ten;
    PressureHistoryRing<72> hourly;
    PressureHistoryRing<30> daily;
    uint32_t ten_bucket = 0, hour_bucket = 0, day_bucket = 0;
    int32_t hour_sum = 0, day_sum = 0;
    uint16_t hour_samples = 0, day_samples = 0;
    uint8_t base_state = 1; // stock cold-start state: partly cloudy; not a learned verdict
    uint8_t reserved[3]{};
    uint32_t checksum = 0;

    static bool clock_valid(uint32_t now) { return now >= MIN_TIME; }
    bool waiting_for_clock(uint32_t now) const { return !clock_valid(now) || (last_unix && now < last_unix); }
    bool trained(uint32_t now) const { return !waiting_for_clock(now) && daily.count(now, 30 * DAY) == 30; }
    uint8_t forecast_state(float outdoor_c) const {
        return (base_state == 3 || base_state == 5) && std::isfinite(outdoor_c) && outdoor_c <= 0
                   ? base_state + 1 : base_state;
    }
    int16_t baseline10(uint32_t now, uint32_t horizon) const {
        int16_t value = daily.mean(now, horizon);
        if (value == MISSING) value = hourly.mean(now, 72 * HOUR);
        if (value == MISSING) value = ten.mean(now, HOUR);
        return value;
    }
    int8_t trend_level(float current_hpa, uint32_t now) const {
        if (waiting_for_clock(now) || !std::isfinite(current_hpa) || current_hpa < 300 || current_hpa > 1200)
            return -1;
        const int16_t baseline = baseline10(now, 30 * DAY);
        if (baseline == MISSING) return -1;
        const int32_t d = static_cast<int32_t>(std::lround(current_hpa * 10)) - baseline;
        return d > 49 ? 0 : d > 39 ? 1 : d > 29 ? 2 : d > 19 ? 3 : d > 9 ? 4
               : d >= -9 ? 5 : d < -49 ? 10 : d < -39 ? 9 : d < -29 ? 8 : d < -19 ? 7 : 6;
    }
    void classify(uint32_t now) {
        const int16_t recent = ten.mean(now, HOUR), long_mean = hourly.mean(now, 24 * HOUR);
        const int16_t recent_min = ten.minimum(now, HOUR);
        const int16_t earlier_min = hourly.minimum(now, 24 * HOUR, 12 * HOUR);
        if (ten.count(now, HOUR) == 6 && long_mean != MISSING && earlier_min != MISSING &&
            static_cast<int32_t>(recent) - long_mean < -59 &&
            static_cast<int32_t>(recent_min) - earlier_min < -39) { base_state = 5; return; }
        int16_t short_mean = hourly.mean(now, 12 * HOUR);
        if (short_mean == MISSING) short_mean = recent;
        const int16_t baseline = baseline10(now, 14 * DAY);
        if (short_mean == MISSING || baseline == MISSING) return;
        const int32_t d = static_cast<int32_t>(short_mean) - baseline;
        // Deliberate hysteresis gaps preserve the previous state, including the
        // stock initial partly-cloudy state while actual history is incomplete.
        if (d > 49) base_state = 0;
        else if (d > 45) return;
        else if (d > 4) base_state = 1;
        else if (d >= 0) return;
        else if (d >= -30) base_state = 2;
        else if (d >= -34) return;
        else base_state = 3;
    }
    bool sample(float pressure_hpa, uint32_t now) {
        if (waiting_for_clock(now) || !std::isfinite(pressure_hpa) || pressure_hpa < 300 || pressure_hpa > 1200)
            return false;
        if (last_unix && now - last_unix > 30 * DAY) *this = ForecastHistory{};
        const uint32_t tb = now / 600, hb = now / HOUR, db = now / DAY;
        if (last_unix && tb == ten_bucket) return false;
        if (hour_samples && hb != hour_bucket) {
            const int16_t mean = static_cast<int16_t>(hour_sum / hour_samples);
            hourly.push(last_unix, mean, hour_samples);
            day_sum += mean; ++day_samples;
            hour_sum = 0; hour_samples = 0;
        }
        if (day_samples && db != day_bucket) {
            daily.push(last_unix, static_cast<int16_t>(day_sum / day_samples), day_samples);
            day_sum = 0; day_samples = 0;
        }
        ten.expire(now, HOUR); hourly.expire(now, 72 * HOUR); daily.expire(now, 30 * DAY);
        const int16_t p10 = static_cast<int16_t>(std::lround(pressure_hpa * 10));
        ten.push(now, p10, 1);
        hour_sum += p10; ++hour_samples;
        ten_bucket = tb; hour_bucket = hb; day_bucket = db; last_unix = now;
        classify(now);
        return true;
    }
    uint32_t crc() const {
        const auto *bytes = reinterpret_cast<const uint8_t *>(this);
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < offsetof(ForecastHistory, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619u;
        return hash;
    }
    void seal() { checksum = crc(); }
    bool valid() const {
        if (magic != MAGIC || checksum != crc() || base_state > 5 || base_state == 4 ||
            hour_samples > 6 || day_samples > 24 || (last_unix && !clock_valid(last_unix))) return false;
        if (!ten.valid(MIN_TIME, last_unix, 1) || !hourly.valid(MIN_TIME, last_unix, 6) ||
            !daily.valid(MIN_TIME, last_unix, 24)) return false;
        if (!last_unix) return !ten.size && !hourly.size && !daily.size && !hour_samples && !day_samples &&
                              !ten_bucket && !hour_bucket && !day_bucket && !hour_sum && !day_sum;
        if (!ten.size || !hour_samples || ten.newest(0).at != last_unix || (!day_samples && day_sum))
            return false;
        if (ten_bucket != last_unix / 600 || hour_bucket != last_unix / HOUR || day_bucket != last_unix / DAY)
            return false;
        if ((hour_samples && (hour_sum < 3000 * hour_samples || hour_sum > 12000 * hour_samples)) ||
            (day_samples && (day_sum < 3000 * day_samples || day_sum > 12000 * day_samples))) return false;
        return true;
    }
};
// Persisted bytes have no implicit padding: each observation/ring is an exact
// multiple of four bytes and reserved explicitly fills the state-byte tail.
// Freeze the layout so a future edit must deliberately change MAGIC/version.
static_assert(sizeof(PressureObservation) == 8, "checkpoint observation layout");
static_assert(sizeof(PressureHistoryRing<6>) == 52 && sizeof(PressureHistoryRing<72>) == 580 &&
              sizeof(PressureHistoryRing<30>) == 244, "checkpoint ring layout");
static_assert(sizeof(ForecastHistory) == 916 && offsetof(ForecastHistory, checksum) == 912,
              "forecast checkpoint layout: change version when changing persisted fields");
static_assert(sizeof(ForecastHistory) < 2048, "bounded forecast checkpoint");

}}  // namespace esphome::ecowitt_rf
