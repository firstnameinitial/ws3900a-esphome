// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
namespace esphome { namespace ecowitt_rf {
// Only this component's application preferences; Wi-Fi/API/OTA credentials are
// owned by ESPHome and are deliberately outside this reset transaction.
constexpr uint32_t APPLICATION_PREF_KEYS[] = {
    0xE601,0xE612,0xE613,0xE614,0xE616,0xE617,0xE618,0xE619,
    0xE620,0xE621,0xE622,0xE623,0xE624,0xE625,0xE626,0xE627
};
template<typename ResetOne, typename Flush>
inline bool reset_application_preferences(ResetOne reset_one, Flush flush) {
    bool complete = true;
    for (uint32_t key : APPLICATION_PREF_KEYS) complete = reset_one(key) && complete;
    return flush() && complete;
}
}}  // namespace esphome::ecowitt_rf
