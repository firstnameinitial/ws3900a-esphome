// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cmath>
#include <cstdint>
#include "display_state.h"

namespace esphome { namespace ecowitt_rf {
// Pass a single selected WH45/WH46 roster entry, rather than mixing upload-only
// particle sensors into the combo sensor's console row. DATE is always reachable.
enum class AirQualityRow : uint8_t { DATE = 0, CO2, PM1, PM25, PM10, AQI, COUNT };
inline bool aq_supports_lcd(Family family) { return family == Family::WH45 || family == Family::WH46; }
inline uint8_t aq_row_slot(uint8_t row) {
    switch (static_cast<AirQualityRow>(row)) {
        case AirQualityRow::CO2: return UiState::CO2;
        case AirQualityRow::PM1: return UiState::PM1;
        case AirQualityRow::PM25: return UiState::PM25;
        case AirQualityRow::PM10: return UiState::PM10;
        case AirQualityRow::AQI: return UiState::AQI;
        default: return UiState::N;
    }
}
inline bool aq_row_value(uint8_t row, const UiState &u, float &value) {
    const uint8_t slot = aq_row_slot(row);
    if (slot == UiState::N || !u.has(slot) || !std::isfinite(u.v[slot]) || u.v[slot] < 0) return false;
    value = u.v[slot];
    return true;
}
inline bool aq_row_available(uint8_t row, const UiState &u) {
    if (row == static_cast<uint8_t>(AirQualityRow::DATE)) return true;
    float value;
    return aq_row_value(row, u, value);
}
inline uint8_t aq_normalize_row(uint8_t row, const UiState &u) {
    return aq_row_available(row, u) ? row : static_cast<uint8_t>(AirQualityRow::DATE);
}
inline uint8_t aq_next_row(uint8_t current, const UiState &u) {
    constexpr uint8_t count = static_cast<uint8_t>(AirQualityRow::COUNT);
    current = current < count ? current : 0;
    for (uint8_t n = 1; n <= count; ++n) {
        const uint8_t candidate = static_cast<uint8_t>((current + n) % count);
        if (aq_row_available(candidate, u)) return candidate;
    }
    return 0;
}
inline bool aq_date_show_weekday(uint32_t now_ms) { return ((now_ms / 5000) & 1u) != 0; }

// WS3900 v1.4.8 console convention, established from its PM2.5 index conversion:
// the US EPA 2012 breakpoints, not the revised 2024 scale or the WH45 app's
// separately documented China scale. Input is the sensor's original concentration
// in tenths of a microgram/m³. Preserve this precision before display rounding.
// 0xffff means missing; a known zero gives AQI 0. Result -1 means unavailable.
inline int16_t stock_us_epa_2012_pm25_aqi(uint16_t concentration_tenths) {
    if (concentration_tenths == 0xffffu) return -1;
    constexpr uint16_t concentration[] = {0,121,355,555,1505,2505,3505,5004};
    constexpr uint16_t index10[] = {0,510,1010,1510,2010,3010,4010,5000};
    uint8_t band = 1;
    while (band < 8 && concentration_tenths >= concentration[band]) ++band;
    if (band == 8) return 500;
    const int32_t c_low = concentration[band - 1], c_high = concentration[band] - 1;
    const int32_t i_low = index10[band - 1], i_high = index10[band] - 10;
    const int32_t value10 = i_low + (i_high - i_low) * (concentration_tenths - c_low) / (c_high - c_low);
    return value10 > 5004 ? 500 : static_cast<int16_t>((value10 + 5) / 10);
}
}}  // namespace esphome::ecowitt_rf
