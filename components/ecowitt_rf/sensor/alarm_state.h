// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace esphome { namespace ecowitt_rf {
// The caller supplies a LOCAL calendar-minute key. That makes the clock alarm one
// event per local date, including the repeated hour at the end of daylight saving.
struct AlarmState {
    static constexpr uint32_t RING_MS = 120000, SNOOZE_MS = 600000;
    bool ringing = false, snoozing = false;
    uint32_t ring_started_ms = 0, snooze_started_ms = 0;
    int64_t last_regular_minute = -1;

    void cancel() { ringing = snoozing = false; }
    void start(uint32_t now) { ringing = true; snoozing = false; ring_started_ms = now; }
    void tick(uint32_t now, bool enabled, bool time_valid, int64_t wall_minute_key, bool matches) {
        if (!enabled) { cancel(); return; }
        if (ringing && now - ring_started_ms >= RING_MS) ringing = false;
        if (snoozing) {
            if (now - snooze_started_ms >= SNOOZE_MS) start(now);
            return;
        }
        if (!ringing && time_valid && matches && last_regular_minute != wall_minute_key) {
            last_regular_minute = wall_minute_key;
            start(now);
        }
    }
    // Ringing key presses are consumed. LIGHT snoozes, any other key stops the ring.
    bool press(uint32_t now, bool light) {
        if (!ringing) return false;
        ringing = false;
        snoozing = light;
        if (light) snooze_started_ms = now;
        return true;
    }
    bool cancel_snooze() {
        if (!snoozing) return false;
        snoozing = false;
        return true;
    }
    bool buzzer(uint32_t now) const {
        if (!ringing) return false;
        const uint32_t elapsed = now - ring_started_ms;
        if (elapsed >= RING_MS) return false;
        if (elapsed >= 90000) return true;
        const uint32_t period = elapsed < 30000 ? 1000 : elapsed < 60000 ? 500 : 250;
        return elapsed % period < 100;
    }
    uint32_t snooze_seconds(uint32_t now) const {
        if (!snoozing || now - snooze_started_ms >= SNOOZE_MS) return 0;
        return (SNOOZE_MS - (now - snooze_started_ms) + 999) / 1000;
    }
};
}}  // namespace esphome::ecowitt_rf
