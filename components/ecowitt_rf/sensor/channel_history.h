// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cstdint>
namespace esphome { namespace ecowitt_rf {
struct ChannelHistory {
    struct Record {
        uint32_t key = 0;
        uint8_t valid = 0;
        float t_min = 0, t_max = 0, h_min = 0, h_max = 0;
        void sample(float t, float h) {
            if (std::isfinite(t)) {
                if (!(valid & 1) || t < t_min) t_min = t;
                if (!(valid & 2) || t > t_max) t_max = t;
                valid |= 3;
            }
            if (std::isfinite(h)) {
                if (!(valid & 4) || h < h_min) h_min = h;
                if (!(valid & 8) || h > h_max) h_max = h;
                valid |= 12;
            }
        }
        void reset(bool maximum) { valid &= maximum ? uint8_t(~10u) : uint8_t(~5u); }
    } channel[8];
    uint32_t magic = 0x43484931;
    bool valid() const {
        if (magic != 0x43484931) return false;
        for (const auto &r : channel) {
            if (r.valid & ~15u) return false;
            if ((r.valid & 1) && !std::isfinite(r.t_min)) return false;
            if ((r.valid & 2) && !std::isfinite(r.t_max)) return false;
            if ((r.valid & 4) && (!std::isfinite(r.h_min) || r.h_min < 0 || r.h_min > 100)) return false;
            if ((r.valid & 8) && (!std::isfinite(r.h_max) || r.h_max < 0 || r.h_max > 100)) return false;
        }
        return true;
    }
    bool sample(uint8_t ch, uint32_t key, float t, float h) {
        if (ch < 1 || ch > 8 || key == 0) return false;
        auto &r = channel[ch - 1];
        if (r.key != key) { r = Record{}; r.key = key; }
        const Record previous = r;
        r.sample(t, h);
        return previous.valid != r.valid || previous.t_min != r.t_min || previous.t_max != r.t_max ||
               previous.h_min != r.h_min || previous.h_max != r.h_max;
    }
    void forget(uint8_t ch) { if (ch >= 1 && ch <= 8) channel[ch - 1] = Record{}; }
    void reset(bool maximum) { for (auto &r : channel) r.reset(maximum); }
};
}}  // namespace esphome::ecowitt_rf
