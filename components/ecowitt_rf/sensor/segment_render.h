// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "display.h"

namespace esphome { namespace ecowitt_rf {

struct ForecastSegments {
    bool sun = false, cloud = false, rain = false, snow = false;
};

// The manual's storm diagrams retain the cloud bodies while rain/snow flashes.
// Stock v1.4.8 instead flashes the whole icon; follow the documented diagrams.
inline ForecastSegments forecast_segments(uint8_t state, bool blink) {
    if (state == 5 || state == 6) {
        return {false, true, state == 5 && blink, state == 6 && blink};
    }
    if (state > 4) return {};
    return {state < 2, state >= 1, state == 3, state == 4};
}

inline void draw_forecast_segments(uint8_t *frame, uint8_t state, bool blink) {
    const auto icons = forecast_segments(state, blink);
    seg(frame, SEG_FC_SUN, icons.sun);
    seg(frame, SEG_FC_CLOUD, icons.cloud);
    seg(frame, SEG_FC_RAIN, icons.rain);
    seg(frame, SEG_FC_SNOW, icons.snow);
}

// The physical table runs from the +5 hPa position (0) to -5 hPa (10).
inline int8_t pressure_trend_level(int32_t difference_tenths_hpa) {
    const int32_t d = difference_tenths_hpa;
    return d > 49 ? 0 : d > 39 ? 1 : d > 29 ? 2 : d > 19 ? 3 : d > 9 ? 4
           : d >= -9 ? 5 : d < -49 ? 10 : d < -39 ? 9 : d < -29 ? 8 : d < -19 ? 7 : 6;
}

inline void draw_trend_segments(uint8_t *frame, int8_t level) {
    seg(frame, SEG_TREND_ZONE, true);
    seg(frame, SEG_TREND_AXIS, true);
    for (uint8_t i = 0; i < PTREND_N; ++i) seg(frame, BAR22[i], i == level);
}

// Shared date/AQ row captions. Electrode roles are taken from the actual selector
// dispatch, not the early reverse-engineering notes' ambiguous label names.
constexpr uint16_t SEG_DATE_DOT = 0x2608;
constexpr uint16_t SEG_AQ_CO2 = 0x1e08;
constexpr uint16_t SEG_AQ_PM1 = 0x2602;
constexpr uint16_t SEG_AQ_PM25 = 0x2580;
constexpr uint16_t SEG_AQ_PM10 = 0x2510;
constexpr uint16_t SEG_AQ_AQI = 0x1c80;
constexpr uint16_t SEG_AQ_PPM = 0x0404;
constexpr uint16_t SEG_AQ_UGM3 = 0x0440;
constexpr uint16_t SEG_AQ_RF_ANT = 0x2504;
static const uint16_t AQ_RF_BARS[4] = {0x1d02,0x2501,0x2480,0x2c80};

// Row identifiers match AirQualityRow: DATE, CO2, PM1, PM25, PM10, AQI.
inline void draw_aq_captions(uint8_t *frame, uint8_t row) {
    seg(frame, SEG_ROW_DATE, row == 0);
    seg(frame, SEG_DATE_DOT, row == 0);
    seg(frame, SEG_AQ_CO2, row == 1);
    seg(frame, SEG_AQ_PM1, row == 2);
    seg(frame, SEG_AQ_PM25, row == 3);
    seg(frame, SEG_AQ_PM10, row == 4);
    seg(frame, SEG_AQ_AQI, row == 5);
    seg(frame, SEG_AQ_PPM, row == 1);
    seg(frame, SEG_AQ_UGM3, row >= 2 && row <= 4);
}

inline void draw_aq_link(uint8_t *frame, bool available, uint8_t bars, bool battery_low) {
    seg(frame, SEG_AQ_RF_ANT, available);
    for (uint8_t i = 0; i < 4; ++i) seg(frame, AQ_RF_BARS[i], available && i < bars);
    seg(frame, SEG_AQ_BATT, available && battery_low);
}

}}  // namespace esphome::ecowitt_rf
