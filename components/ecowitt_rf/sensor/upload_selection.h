// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "fineoffset.h"
#include <initializer_list>

namespace esphome::ecowitt_rf {
struct UploadSources {
    uint32_t th = 0, wind = 0, uv = 0, solar = 0, indoor = 0, rain = 0, aq = 0;
};
inline bool upload_station_array(Family family) {
    switch (family) {
        case Family::WH24: case Family::WH65: case Family::WS69:
        case Family::WS68: case Family::WS80: case Family::WS85: case Family::WS90: return true;
        default: return false;
    }
}
// Each family has one station battery key even if different physical sensors own
// different weather fields. Pick one owner consistently instead of interleaving it.
inline uint32_t upload_array_battery_owner(Family family, const UploadSources &s) {
    for (uint32_t key : {s.wind,s.rain,s.th,s.uv,s.solar})
        if (key && static_cast<Family>(key >> 24) == family) return key;
    return 0;
}
// Built-in I2C routing is handled separately by the caller. The input stays intact
// for per-sensor/native diagnostics; only the returned upload copy loses station fields.
inline bool select_upload_reading(const Reading &input, uint32_t key,
                                  const UploadSources &sources, Reading &out) {
    out = input;
    const auto owns = [key](uint32_t selected) { return key && key == selected; };
    if (input.family == Family::WH45 || input.family == Family::WH46) {
        // Combo T/H retain their own tf_co2/humi_co2 upload keys. WU independently
        // excludes this family from station T/H; its measurements are indoors.
        return owns(sources.aq);
    }
    const bool array = upload_station_array(input.family);
    const bool thp = input.family == Family::WH25;
    const bool rain_gauge = input.family == Family::WH40 || input.family == Family::WN20;
    if (!array && !thp && !rain_gauge) return true; // WH31/channel/soil/WH43 stay independent

    const bool outside = array || (thp && input.model == 0xD && !input.has(F_PRESS));
    const bool th_owner = outside ? owns(sources.th) : owns(sources.indoor);
    if (!th_owner) out.present &= ~(F_TEMP | F_HUM);
    if (!owns(sources.indoor)) out.present &= ~F_PRESS;
    if (!owns(sources.wind)) out.present &= ~(F_WIND | F_WDIR | F_GUST);
    if (!owns(sources.uv)) out.present &= ~F_UV;
    if (!owns(sources.solar)) out.present &= ~F_LIGHT;
    out.allow_rain_upload = input.allow_rain_upload && is_rain_family(input.family) && owns(sources.rain);
    const uint32_t station_fields = F_TEMP | F_HUM | F_PRESS | F_WIND | F_WDIR | F_GUST | F_UV | F_LIGHT;
    if (!(out.present & station_fields) && !out.allow_rain_upload) return false;
    out.allow_battery_upload = input.allow_battery_upload &&
        (array ? key == upload_array_battery_owner(input.family,sources)
               : thp ? th_owner : owns(sources.rain));
    if (!out.allow_battery_upload) out.present &= ~(F_BATV | F_CAPV);
    return true;
}
}  // namespace esphome::ecowitt_rf
