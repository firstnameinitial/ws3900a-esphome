// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace esphome { namespace ecowitt_rf {
struct RainCalendarPolicy {
    uint8_t day_reset_hour = 0;
    uint8_t week_start = 0; // 0 Sunday, 1 Monday
    uint8_t season_start_month = 1;
    bool valid() const { return day_reset_hour < 24 && week_start < 2 && season_start_month >= 1 && season_start_month <= 12; }
    bool operator==(const RainCalendarPolicy &p) const {
        return day_reset_hour == p.day_reset_hour && week_start == p.week_start && season_start_month == p.season_start_month;
    }
    bool operator!=(const RainCalendarPolicy &p) const { return !(*this == p); }
};
struct RainCalendarKeys {
    uint32_t day = 0, hour = 0, week = 0, month = 0, season = 0;
    bool valid = false;
};
inline bool rain_leap_year(uint16_t year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
inline uint8_t rain_month_days(uint16_t year, uint8_t month) {
    static constexpr uint8_t days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return month >= 1 && month <= 12 ? static_cast<uint8_t>(days[month-1] + (month == 2 && rain_leap_year(year))) : 0;
}
inline uint16_t rain_day_of_year(uint16_t year, uint8_t month, uint8_t day) {
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > rain_month_days(year,month)) return 0;
    uint16_t doy = day;
    for (uint8_t m = 1; m < month; ++m) doy += rain_month_days(year,m);
    return doy;
}
inline int32_t rain_days_since_epoch(uint16_t year, uint16_t doy) {
    if (year < 1970 || doy < 1 || doy > (rain_leap_year(year) ? 366 : 365)) return -1;
    const int32_t y = static_cast<int32_t>(year) - 1;
    return 365 * (y-1969) + (y/4-1969/4) - (y/100-1969/100) + (y/400-1969/400) + doy-1;
}
// Daily reset hour affects DAILY only. Week/month/season boundaries remain their
// independently configured local-calendar boundaries, not a shifted fiscal day.
inline RainCalendarKeys rain_calendar_keys(uint16_t year, uint16_t doy, uint8_t hour,
                                           const RainCalendarPolicy &policy = {}) {
    RainCalendarKeys k;
    const int32_t days = rain_days_since_epoch(year,doy);
    if (days < 0 || hour > 23 || !policy.valid()) return k;
    uint16_t remaining = doy;
    uint8_t month = 1;
    while (remaining > rain_month_days(year,month)) { remaining -= rain_month_days(year,month); ++month; }
    k.day = static_cast<uint32_t>(days + 1 - (hour < policy.day_reset_hour));
    k.hour = static_cast<uint32_t>(days) * 24u + hour;
    k.week = static_cast<uint32_t>((days + 4 - policy.week_start) / 7);
    k.month = static_cast<uint32_t>(year) * 12u + month;
    k.season = year - (month < policy.season_start_month);
    k.valid = true;
    return k;
}
}}  // namespace esphome::ecowitt_rf
