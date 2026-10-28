// SPDX-License-Identifier: GPL-3.0-only
#include "console_power.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    ConsoleBatteryState s;
    assert(!s.known);
    assert(s.observe_battery_mv(4000) && !s.battery_low);
    s.observe_battery_mv(3601); assert(!s.battery_low);
    s.observe_battery_mv(3600); assert(s.battery_low);
    s.observe_battery_mv(3704); assert(s.battery_low);
    s.observe_battery_mv(3705); assert(!s.battery_low);
    s.observe_battery_mv(0); assert(s.known && s.battery_low && s.millivolts==0);
    s.observe_battery_mv(6000); assert(s.millivolts==4500 && !s.battery_low);
    s.observe_adc_mv(2487); assert(s.millivolts==4498 && !s.battery_low);
    s.observe_adc_mv(1990); assert(s.millivolts==3599 && s.battery_low);
    s.observe_adc_mv(2047); assert(s.millivolts==3702 && s.battery_low);
    s.observe_adc_mv(2049); assert(s.millivolts==3706 && !s.battery_low);
    assert(!s.observe_adc_mv(-1) && !s.known && s.millivolts==0);
    assert(!s.observe_adc_mv(3301) && !s.known);
    ConsolePower p;
    assert(!p.begin() && !p.known() && !p.low());
    assert(!p.sample(0) && !p.known());
    assert(!p.sample(29999)); assert(!p.sample(30000));
    puts("console power: stock divider, low/recovery hysteresis, absent-cell zero, clamp and unknown/failure passed");
}
