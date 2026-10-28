// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace esphome { namespace ecowitt_rf {
enum class ClockField : uint8_t { HOUR, MINUTE, YEAR, MONTH, DAY };
struct PanelClockValue {
    int year = 2026, month = 1, day = 1, hour = 0, minute = 0, second = 0;
};
inline bool panel_leap_year(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
inline int panel_month_days(int y, int m) {
    static constexpr uint8_t DAYS[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return m >= 1 && m <= 12 ? DAYS[m - 1] + (m == 2 && panel_leap_year(y)) : 0;
}
// ESPHome's generic validity threshold is 2019; the console editor supports 2000..2099.
// Validate the actual calendar fields instead, while the unset 1970 clock stays invalid.
template<class T> inline bool panel_clock_valid(const T &t) {
    return t.year >= 2000 && t.year <= 2099 && t.month >= 1 && t.month <= 12 &&
        t.day_of_month >= 1 && t.day_of_month <= panel_month_days(t.year,t.month) &&
        t.hour <= 23 && t.minute <= 59 && t.second <= 59;
}
inline int panel_wrap(int value, int lo, int hi) {
    const int range = hi - lo + 1;
    int n = (value - lo) % range;
    return lo + (n < 0 ? n + range : n);
}
// Edits are calendar-field changes, not elapsed-duration additions. Hour/minute wrap
// within their field; month/year edits clamp a formerly valid day (31 Jan -> 28 Feb).
inline bool apply_clock_delta(PanelClockValue &t, ClockField field, int delta) {
    if (t.year < 2000 || t.year > 2099 || t.month < 1 || t.month > 12 || t.day < 1 ||
        t.day > panel_month_days(t.year, t.month) || t.hour < 0 || t.hour > 23 ||
        t.minute < 0 || t.minute > 59 || t.second < 0 || t.second > 59) return false;
    switch (field) {
        case ClockField::HOUR: t.hour = panel_wrap(t.hour + delta, 0, 23); t.second = 0; break;
        case ClockField::MINUTE: t.minute = panel_wrap(t.minute + delta, 0, 59); t.second = 0; break;
        case ClockField::YEAR: t.year = panel_wrap(t.year + delta, 2000, 2099); break;
        case ClockField::MONTH: t.month = panel_wrap(t.month + delta, 1, 12); break;
        case ClockField::DAY: t.day = panel_wrap(t.day + delta, 1, panel_month_days(t.year, t.month)); break;
        default: return false;
    }
    const int max_day = panel_month_days(t.year, t.month);
    if (t.day > max_day) t.day = max_day;
    return true;
}
}}  // namespace esphome::ecowitt_rf
