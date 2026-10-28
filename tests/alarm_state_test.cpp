// SPDX-License-Identifier: GPL-3.0-only
#include "alarm_state.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    AlarmState a;
    a.tick(10,true,true,123,true);assert(a.ringing); // no ninety-second boot blind spot
    assert(a.buzzer(10) && !a.buzzer(210));
    assert(a.press(20,false) && !a.ringing);
    a.tick(21,true,true,123,true);assert(!a.ringing); // once per wall calendar minute
    a.tick(30,true,true,124,true);assert(a.ringing);
    assert(a.press(100,true) && a.snoozing && a.snooze_seconds(100)==600);
    a.tick(600099,true,true,134,false);assert(!a.ringing);
    a.tick(600100,true,true,134,false);assert(a.ringing && !a.snoozing); // HH:MM no longer matches
    a.press(600200,true);assert(a.cancel_snooze() && !a.cancel_snooze());
    a.tick(600201,true,true,134,false);assert(!a.ringing);
    a.start(1000);
    assert(a.buzzer(1000) && !a.buzzer(1100) && a.buzzer(2000));
    assert(a.buzzer(31000) && !a.buzzer(31100) && a.buzzer(31500));
    assert(a.buzzer(61000) && !a.buzzer(61100) && a.buzzer(61250));
    assert(a.buzzer(91000) && a.buzzer(119999) && !a.buzzer(121000));
    a.tick(121000,true,true,999,false);assert(!a.ringing);
    a.start(0xfffffff0u);a.press(0xfffffff5u,true);
    a.tick(uint32_t(0xfffffff5u+600000u),true,false,0,false);assert(a.ringing);
    a.tick(123,false,false,0,false);assert(!a.ringing && !a.snoozing);
    puts("clock alarm: immediate boot, once-per-minute, ten-minute snooze, cancel, escalating cadence, timeout and millis wrap passed");
}
