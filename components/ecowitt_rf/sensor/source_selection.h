// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fineoffset.h"

namespace esphome { namespace ecowitt_rf {

// The caller derives these facts from the accepted-packet roster. Persisted
// values alone are not observations: seen and fresh must both be true. Rain
// has no Field bit in the decoder, so its validated counter has a separate flag.
struct SourceCandidate {
    uint32_t key = 0;
    Family family = Family::Unknown;
    uint8_t model = 0;
    bool seen = false, fresh = false, enabled = false;
    uint32_t fields = 0;
    bool rain_present = false;
};
enum class RainSourcePreference : uint8_t { Traditional, Piezo };

inline bool source_has(const SourceCandidate &s, uint32_t required) {
    return s.key && s.seen && s.fresh && s.enabled &&
           required && (s.fields & required) == required;
}
inline int outdoor_source_rank(const SourceCandidate &s) {
    if (s.family == Family::WH25 && s.model == 0xD && !(s.fields & F_PRESS)) return 0;
    switch (s.family) {
        case Family::WS90: return 1;
        case Family::WS80: return 2;
        case Family::WS69: case Family::WH65: case Family::WH24: return 3;
        default: return -1;
    }
}
inline int wind_source_rank(const SourceCandidate &s) {
    switch (s.family) {
        case Family::WS85: return 0;
        case Family::WS90: return 1;
        case Family::WS80: return 2;
        case Family::WS68: return 3;
        case Family::WS69: case Family::WH65: case Family::WH24: return 4;
        default: return -1;
    }
}
inline int traditional_rain_source_rank(const SourceCandidate &s) {
    switch (s.family) {
        case Family::WH40: return 0;
        case Family::WN20: return 1; // supported standalone tipping gauge
        case Family::WS69: case Family::WH65: case Family::WH24: return 2;
        default: return -1;
    }
}
inline int piezo_rain_source_rank(const SourceCandidate &s) {
    return s.family == Family::WS85 ? 0 : s.family == Family::WS90 ? 1 : -1;
}

// Keep an eligible current source when ranks tie. Otherwise the lowest key
// wins, independent of roster/packet arrival order. A better rank preempts.
template<typename Rank>
inline uint32_t select_ranked_source(const SourceCandidate *sources, size_t count,
                                     uint32_t current, Rank rank) {
    if (!sources) return 0;
    uint32_t best = 0; int best_rank = -1;
    for (size_t i = 0; i < count; ++i) {
        const auto &s = sources[i];
        const int r = rank(s);
        if (r < 0) continue;
        if (best_rank < 0 || r < best_rank ||
            (r == best_rank && (s.key == current || (best != current && s.key < best)))) {
            best = s.key; best_rank = r;
        }
    }
    return best;
}
inline uint32_t select_outdoor_source(const SourceCandidate *s, size_t n, uint32_t current = 0,
                                      uint32_t required = F_TEMP | F_HUM) {
    if (!required || (required & ~(F_TEMP | F_HUM))) return 0;
    return select_ranked_source(s,n,current,[=](const SourceCandidate &c) {
        return source_has(c,required) ? outdoor_source_rank(c) : -1;
    });
}
inline uint32_t select_wind_source(const SourceCandidate *s, size_t n, uint32_t current = 0,
                                   uint32_t required = F_WIND | F_WDIR | F_GUST) {
    if (!required || (required & ~(F_WIND | F_WDIR | F_GUST))) return 0;
    return select_ranked_source(s,n,current,[=](const SourceCandidate &c) {
        return source_has(c,required) ? wind_source_rank(c) : -1;
    });
}
// Select UVI and light separately: a wind-only WS85 must never hide WS90 solar.
inline uint32_t select_solar_source(const SourceCandidate *s, size_t n, uint32_t field,
                                    uint32_t current = 0) {
    if (field != F_UV && field != F_LIGHT) return 0;
    return select_ranked_source(s,n,current,[=](const SourceCandidate &c) {
        return source_has(c,field) ? wind_source_rank(c) : -1;
    });
}
inline uint32_t select_rain_source(const SourceCandidate *s, size_t n, RainSourcePreference preference,
                                   uint32_t current = 0) {
    return select_ranked_source(s,n,current,[=](const SourceCandidate &c) {
        if (!c.key || !c.seen || !c.fresh || !c.enabled || !c.rain_present) return -1;
        const int t = traditional_rain_source_rank(c), p = piezo_rain_source_rank(c);
        if (preference == RainSourcePreference::Piezo) return p >= 0 ? p : t >= 0 ? t + 10 : -1;
        return t >= 0 ? t : p >= 0 ? p + 10 : -1;
    });
}
// This selects only the RF override. Caller retains the built-in I2C source
// when zero is returned. Never use a model0xD outdoor packet or an RF packet
// with a missing/faulted pressure value as the indoor override.
inline uint32_t select_indoor_override(const SourceCandidate *s, size_t n, uint32_t current = 0,
                                       uint32_t required = F_TEMP | F_HUM | F_PRESS) {
    if (!(required & F_PRESS) || (required & ~(F_TEMP | F_HUM | F_PRESS))) return 0;
    return select_ranked_source(s,n,current,[=](const SourceCandidate &c) {
        return c.family == Family::WH25 && c.model == 0xE && source_has(c,required) ? 0 : -1;
    });
}
}} // namespace esphome::ecowitt_rf
