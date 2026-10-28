// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include "backlight_policy.h"

namespace esphome { namespace ecowitt_rf {
// New preference; the original fourteen-byte PanelSettings remains unchanged.
struct PanelOptions {
    static constexpr uint32_t MAGIC = 0x504f5031;
    uint32_t magic = MAGIC;
    BacklightSchedule schedule;
    uint8_t rain_day_hour = 0, rain_week_monday = 0, rain_season_month = 1, rain_prefer_piezo = 1;
    uint32_t hilo_date = 0;
    bool valid() const {
        return magic == MAGIC && schedule.valid() && rain_day_hour < 24 && rain_week_monday <= 1 &&
               rain_season_month >= 1 && rain_season_month <= 12 && rain_prefer_piezo <= 1;
    }
};
}}  // namespace esphome::ecowitt_rf
