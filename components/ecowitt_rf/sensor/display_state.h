// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fineoffset.h"
#include <initializer_list>
namespace esphome { namespace ecowitt_rf {
struct UiState {
    enum : uint8_t {
        T_IN = 0, H_IN, P, T_OUT, H_OUT, LEAF, WDIR, WSPD, WGUST, RAIN, RAIN_RATE,
        UV, SOLAR, SOIL_MOIST, SOIL_T, SOIL_EC, PM25, PM10, CO2, LIGHT_N, LIGHT_D, LEAK,
        BAT_V, CAP_V, MAX_GUST, WIND_AVG, RAIN_24H, RAIN_STATE, N
    };
    float v[N] = {0};
    uint32_t seen = 0;
    void set(uint8_t i, float x) { v[i] = x; seen |= (1u << i); }
    bool has(uint8_t i) const { return (seen & (1u << i)) != 0; }
};
inline void map_reading(const Reading &r, UiState &u) {
        switch (r.family) {
            case Family::WH25:
                if (r.has(F_TEMP)) u.set(UiState::T_IN, r.temp_c);
                if (r.has(F_HUM)) u.set(UiState::H_IN, r.humidity);
                if (r.has(F_PRESS)) u.set(UiState::P, r.pressure_hpa);
                break;
            case Family::WH31:
                if (r.has(F_TEMP)) u.set(UiState::T_OUT, r.temp_c);
                if (r.has(F_HUM)) u.set(UiState::H_OUT, r.humidity);
                break;
            case Family::WH24: case Family::WH65: case Family::WS69:
                if (r.has(F_WDIR)) u.set(UiState::WDIR, r.wind_dir_deg);
                if (r.has(F_WIND)) u.set(UiState::WSPD, r.wind_mps);
                if (r.has(F_GUST)) u.set(UiState::WGUST, r.gust_mps);
                if (r.rain_mm > 0) u.set(UiState::RAIN, r.rain_mm);
                if (std::isfinite(r.rain_rate_mmh)) u.set(UiState::RAIN_RATE, r.rain_rate_mmh);
                else u.seen &= ~(1u << UiState::RAIN_RATE);
                if (r.has(F_UV)) u.set(UiState::UV, r.uv);
                if (r.has(F_LIGHT)) u.set(UiState::SOLAR, r.light_wm2);
                if (r.has(F_TEMP)) u.set(UiState::T_OUT, r.temp_c);
                if (r.has(F_HUM)) u.set(UiState::H_OUT, r.humidity);
                // Pressure card is owned by the built-in barometer — see the
                // barometer-ownership note in ecowitt_upload.h.
                break;
            case Family::WS68: case Family::WS80: case Family::WS85: case Family::WS90:
                if (r.has(F_WDIR)) u.set(UiState::WDIR, r.wind_dir_deg);
                if (r.has(F_WIND)) u.set(UiState::WSPD, r.wind_mps);
                if (r.has(F_GUST)) u.set(UiState::WGUST, r.gust_mps);
                if (r.rain_mm > 0) u.set(UiState::RAIN, r.rain_mm);
                if (std::isfinite(r.rain_rate_mmh)) u.set(UiState::RAIN_RATE, r.rain_rate_mmh);
                else u.seen &= ~(1u << UiState::RAIN_RATE);
                if (r.has(F_UV)) u.set(UiState::UV, r.uv);
                if (r.has(F_LIGHT)) u.set(UiState::SOLAR, r.light_wm2);
                if (r.has(F_TEMP)) u.set(UiState::T_OUT, r.temp_c);
                if (r.has(F_HUM)) u.set(UiState::H_OUT, r.humidity);
                if (r.has(F_BATV)) u.set(UiState::BAT_V, r.battery_v);
                if (r.has(F_CAPV)) u.set(UiState::CAP_V, r.cap_v);
                if (r.daily_gust_mps > 0) u.set(UiState::MAX_GUST, r.daily_gust_mps);
                if (r.wind_dir_avg_deg > 0) u.set(UiState::WIND_AVG, r.wind_dir_avg_deg);
                if (r.last24h_rain_mm >= 0) u.set(UiState::RAIN_24H, r.last24h_rain_mm);
                u.set(UiState::RAIN_STATE, r.rain_state ? 1 : 0);
                break;
            case Family::WH40: case Family::WN20:
                if (r.rain_mm > 0) u.set(UiState::RAIN, r.rain_mm);
                break;
            case Family::WH51:
                if (r.moisture_pct) u.set(UiState::SOIL_MOIST, r.moisture_pct);
                break;
            case Family::WN34: case Family::WN38:
                if (r.soil_temp_c != 0) u.set(UiState::SOIL_T, r.soil_temp_c);
                break;
            case Family::WH52:
                if (r.moisture_pct) u.set(UiState::SOIL_MOIST, r.moisture_pct);
                if (r.soil_temp_c != 0) u.set(UiState::SOIL_T, r.soil_temp_c);
                if (r.soil_ec_uscm > 0) u.set(UiState::SOIL_EC, r.soil_ec_uscm);
                break;
            case Family::WH0290: case Family::WH43:
                if (r.pm25_ugm3) u.set(UiState::PM25, r.pm25_ugm3);
                if (r.pm10_ugm3) u.set(UiState::PM10, r.pm10_ugm3);
                break;
            case Family::WH45: case Family::WH46:
                if (r.pm25_ugm3) u.set(UiState::PM25, r.pm25_ugm3);
                if (r.co2_ppm) u.set(UiState::CO2, r.co2_ppm);
                break;
            case Family::WH57:
                if (r.strikes) u.set(UiState::LIGHT_N, r.strikes);
                if (r.lightning_dist_km > 0) u.set(UiState::LIGHT_D, r.lightning_dist_km);
                break;
            case Family::WH55: case Family::WH54:
                u.set(UiState::LEAK, r.leak ? 1 : 0);
                break;
            case Family::WH35:
                if (r.leaf_wetness) u.set(UiState::LEAF, r.leaf_wetness);
                break;
            default: break;
        }
    }

// Extra WH31 channels own their roster values, never the shared OUTDOOR readout.
// Array and rain primaries are selected independently; preserve each owner's fields.
inline void update_shared_reading(const Reading &r, UiState &u, bool rain_primary, bool array_primary) {
    if (r.family == Family::WH25 || r.family == Family::WH31) return;
    const UiState previous = u;
    map_reading(r, u);
    const auto restore = [&u, &previous](uint8_t i) {
        u.v[i] = previous.v[i];
        u.seen = (u.seen & ~(1u << i)) | (previous.seen & (1u << i));
    };
    if (!array_primary) {
        for (uint8_t i : {UiState::T_OUT, UiState::H_OUT, UiState::WDIR, UiState::WSPD,
                          UiState::WGUST, UiState::UV, UiState::SOLAR, UiState::BAT_V,
                          UiState::CAP_V, UiState::MAX_GUST, UiState::WIND_AVG}) restore(i);
    }
    if (!rain_primary) {
        for (uint8_t i : {UiState::RAIN, UiState::RAIN_RATE, UiState::RAIN_24H, UiState::RAIN_STATE}) restore(i);
    }
}
}}  // namespace esphome::ecowitt_rf
