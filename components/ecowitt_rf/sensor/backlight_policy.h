// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace esphome { namespace ecowitt_rf {
struct BacklightSchedule {
    uint16_t on_minute = 7 * 60, off_minute = 22 * 60;
    uint8_t enabled = 0;
    bool valid() const { return enabled <= 1 && on_minute < 1440 && off_minute < 1440; }
    bool active(uint16_t minute) const {
        if (!enabled || !valid() || on_minute == off_minute) return true;
        return on_minute < off_minute ? minute >= on_minute && minute < off_minute
                                    : minute >= on_minute || minute < off_minute;
    }
};
struct BacklightPolicy {
    static constexpr uint32_t WAKE_MS = 15000;
    bool initialized = false, dc_power = true, awake = true;
    bool schedule_override = false, schedule_known = false, previous_schedule_on = true;
    uint32_t activity_ms = 0;
    uint8_t effective_level = 0, restore_level = 2;

    // Call on every genuine key activity (including holds). On battery it wakes
    // medium light for 15 s. A normal LIGHT effect calls manual_level afterwards.
    void activity(uint32_t now) { awake = true; activity_ms = now; }
    void manual_level(uint32_t now, uint8_t selected) {
        awake = selected != 0;
        if (selected) restore_level = selected;
        activity_ms = now;
        if (dc_power) schedule_override = true; // lasts until next schedule boundary
    }
    uint8_t tick(uint32_t now, bool dc, uint8_t &selected, const BacklightSchedule &schedule,
                 bool time_valid = false, uint16_t minute = 0) {
        if (selected > 4) selected = 4;
        if (!initialized) {
            initialized = true; dc_power = dc; activity_ms = now; awake = true;
            if (!dc) selected = 2;
            if (selected) restore_level = selected;
        } else if (dc_power != dc) {
            dc_power = dc; selected = restore_level = 2; activity_ms = now; awake = true;
            schedule_override = false;
        }
        const bool scheduled = schedule.enabled && schedule.valid() && time_valid && minute < 1440;
        const bool on = scheduled ? schedule.active(minute) : true;
        if (!scheduled) {
            schedule_known = false;
            schedule_override = false;
        } else {
            if (schedule_known && on != previous_schedule_on) {
                schedule_override = false;
                if (on && selected == 0) selected = restore_level;
            }
            previous_schedule_on = on; schedule_known = true;
        }
        if (dc_power) effective_level = (!on && !schedule_override) ? 0 : selected;
        else {
            if (awake && now - activity_ms >= WAKE_MS) awake = false;
            effective_level = awake ? 2 : 0;
        }
        return effective_level;
    }
};
}}  // namespace esphome::ecowitt_rf
