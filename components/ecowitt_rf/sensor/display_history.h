// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace esphome { namespace ecowitt_rf {
// Separate, versioned preference: never change the legacy MAX/MIN blob.
struct DisplayHistory {
    enum Field : uint8_t { TEMP, HUM, FEELS, DEW, IN_TEMP, IN_HUM, PRESS,
                           WIND, GUST, RAIN_RATE, UV, SOLAR, COUNT };
    static constexpr uint32_t MAGIC = 0x48494c32;
    uint32_t magic = MAGIC;
    uint32_t valid_min = 0, valid_max = 0;
    uint32_t legacy_signature = 0;
    float low[COUNT] = {}, high[COUNT] = {};

    static uint32_t signature(const float (&values)[6], uint8_t flags) {
        uint32_t hash = 2166136261u;
        for (float value : values) {
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            for (unsigned n = 0; n < 4; ++n) {
                hash = (hash ^ (bits & 0xffu)) * 16777619u;
                bits >>= 8;
            }
        }
        return (hash ^ flags) * 16777619u;
    }
    // A legacy firmware may run between two upgrades. Only these original fields
    // can be reconciled; newer measurements were never recorded by that firmware.
    void import_legacy(const float (&v)[6], uint8_t flags) {
        for (Field f : {TEMP, HUM, GUST, RAIN_RATE}) {
            valid_min &= ~(1u << f); valid_max &= ~(1u << f);
        }
        if (flags & 1) { sample(TEMP, v[1]); sample(TEMP, v[0]); }
        if (flags & 2) { sample(HUM, v[3]); sample(HUM, v[2]); }
        if (flags & 4) sample(GUST, v[4]);
        if (flags & 8) sample(RAIN_RATE, v[5]);
        legacy_signature = signature(v, flags);
    }
    bool has(Field f, bool maximum) const {
        return ((maximum ? valid_max : valid_min) & (1u << f)) != 0;
    }
    float get(Field f, bool maximum) const { return maximum ? high[f] : low[f]; }
    bool sample(Field f, float v) {
        if (!std::isfinite(v)) return false;
        bool changed = false;
        const uint32_t bit = 1u << f;
        if (!(valid_min & bit) || v < low[f]) { low[f] = v; valid_min |= bit; changed = true; }
        if (!(valid_max & bit) || v > high[f]) { high[f] = v; valid_max |= bit; changed = true; }
        return changed;
    }
    void reset(bool maximum) { (maximum ? valid_max : valid_min) = 0; }
    bool valid() const {
        if (magic != MAGIC || ((valid_min | valid_max) >> COUNT)) return false;
        for (uint8_t i = 0; i < COUNT; ++i) {
            const auto f = static_cast<Field>(i);
            if ((has(f, false) && !std::isfinite(low[i])) ||
                (has(f, true) && !std::isfinite(high[i]))) return false;
        }
        return true;
    }
};
// Manual section 3.3.10: 0, (0,3], (3,6], (6,9], (9,12], >12.
inline uint8_t uvi_level(int32_t uv) {
    return uv > 12 ? 5 : uv > 9 ? 4 : uv > 6 ? 3 : uv > 3 ? 2 : uv > 0 ? 1 : 0;
}
}}  // namespace esphome::ecowitt_rf
