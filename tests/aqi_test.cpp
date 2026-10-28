// SPDX-License-Identifier: GPL-3.0-only
#include "air_quality.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    assert(stock_us_epa_2012_pm25_aqi(0xffff) == -1);
    assert(stock_us_epa_2012_pm25_aqi(0) == 0);
    assert(stock_us_epa_2012_pm25_aqi(40) == 17);
    assert(stock_us_epa_2012_pm25_aqi(160) == 59);
    assert(stock_us_epa_2012_pm25_aqi(54) == 23); // retain original fractional concentration
    constexpr uint16_t lower[] = {0,121,355,555,1505,2505,3505};
    constexpr uint16_t upper[] = {120,354,554,1504,2504,3504,5003};
    constexpr int16_t low_index[] = {0,51,101,151,201,301,401};
    constexpr int16_t high_index[] = {50,100,150,200,300,400,499};
    for (unsigned i = 0; i < 7; ++i) {
        assert(stock_us_epa_2012_pm25_aqi(lower[i]) == low_index[i]);
        assert(stock_us_epa_2012_pm25_aqi(upper[i]) == high_index[i]);
    }
    int16_t previous = -1;
    for (unsigned concentration = 0; concentration < 65535; ++concentration) {
        const int16_t value = stock_us_epa_2012_pm25_aqi(concentration);
        assert(value >= previous && value >= 0 && value <= 500);
        previous = value;
    }
    assert(stock_us_epa_2012_pm25_aqi(5004) == 500);
    assert(stock_us_epa_2012_pm25_aqi(65534) == 500);
    puts("AQI: proven console US EPA 2012 scale, fractional inputs, every boundary, monotonicity and missing/saturation passed");
}
