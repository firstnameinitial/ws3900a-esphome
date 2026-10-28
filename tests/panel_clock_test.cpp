// SPDX-License-Identifier: GPL-3.0-only
#include "panel_clock.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    struct Clock { int year,month,day_of_month,hour,minute,second; };
    assert(panel_clock_valid(Clock{2000,2,29,23,59,59}));
    assert(panel_clock_valid(Clock{2018,1,1,0,0,0}));
    assert(!panel_clock_valid(Clock{1970,1,1,0,0,0}));
    assert(!panel_clock_valid(Clock{2100,1,1,0,0,0}));
    assert(!panel_clock_valid(Clock{2001,2,29,0,0,0}));

    PanelClockValue t{2024,1,31,23,59,42};
    assert(apply_clock_delta(t,ClockField::MONTH,1) && t.month==2 && t.day==29);
    assert(apply_clock_delta(t,ClockField::YEAR,1) && t.year==2025 && t.day==28);
    assert(apply_clock_delta(t,ClockField::DAY,1) && t.day==1);
    assert(apply_clock_delta(t,ClockField::DAY,-1) && t.day==28);
    assert(apply_clock_delta(t,ClockField::HOUR,1) && t.hour==0 && t.day==28 && t.second==0);
    assert(apply_clock_delta(t,ClockField::MINUTE,1) && t.minute==0);
    t.year=2099;assert(apply_clock_delta(t,ClockField::YEAR,1) && t.year==2000);
    t.month=12;assert(apply_clock_delta(t,ClockField::MONTH,1) && t.month==1);
    assert(!panel_leap_year(2100) && panel_leap_year(2000));
    t.day=32;assert(!apply_clock_delta(t,ClockField::DAY,1));
    puts("manual clock: leap years, month-day clamps, field wraps and invalid dates passed");
}
