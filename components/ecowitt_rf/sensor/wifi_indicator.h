// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
namespace esphome { namespace ecowitt_rf {
// Manual Table 3: fast pairing/unjoined, slow associated until a real upload,
// then steady RSSI for this association. A later failed request does not erase success.
struct WifiIndicator {
    bool connected = false, uploaded = false;
    uint32_t baseline = 0;
    uint8_t level(uint32_t now, bool associated, bool ap, int rssi, uint32_t successes) {
        if (!associated) { connected = uploaded = false; baseline = successes; }
        else if (!connected) { connected = true; uploaded = false; baseline = successes; }
        else if (successes != baseline) uploaded = true;
        const uint8_t strength = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -78 ? 2 : 1;
        if (ap || !associated) return (now / 250u) % 2u ? 0 : 4;
        if (!uploaded) return (now / 1000u) % 2u ? 0 : strength;
        return strength;
    }
};
}}  // namespace esphome::ecowitt_rf
