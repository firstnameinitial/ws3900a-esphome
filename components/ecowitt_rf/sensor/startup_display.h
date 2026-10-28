// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
namespace esphome { namespace ecowitt_rf {
// Manual 3.3.14: DATE and sunrise/set during the initial twenty-second search;
// then the first available AQ field and UV/solar. Explicit controls take precedence.
struct StartupDisplay {
    uint32_t began = 0;
    bool done = false, aq_manual = false, sun_manual = false;
    void tick(uint32_t now, uint8_t aq_count, uint8_t &aq_sel, bool &sun) {
        if (done) return;
        if (now - began < 20000UL) {
            if (!aq_manual) aq_sel = 0;
            if (!sun_manual) sun = true;
        } else {
            if (!aq_manual) aq_sel = aq_count ? 1 : 0;
            if (!sun_manual) sun = false;
            done = true;
        }
    }
};
}}  // namespace esphome::ecowitt_rf
