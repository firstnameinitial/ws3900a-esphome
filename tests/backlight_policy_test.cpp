// SPDX-License-Identifier: GPL-3.0-only
#include "backlight_policy.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    BacklightPolicy p;BacklightSchedule s;uint8_t level=4;
    assert(p.tick(0,true,level,s)==4);
    assert(p.tick(100,false,level,s)==2 && level==2); // DC removal: medium then timeout
    assert(p.tick(15099,false,level,s)==2);
    assert(p.tick(15100,false,level,s)==0);
    p.activity(16000);assert(p.tick(16000,false,level,s)==2);
    p.manual_level(16100,0);assert(p.tick(16100,false,level,s)==0);
    p.activity(16200);assert(p.tick(16200,false,level,s)==2);
    assert(p.tick(17000,true,level,s)==2 && level==2);
    s.enabled=1;s.on_minute=7*60;s.off_minute=22*60;
    assert(p.tick(18000,true,level,s,true,21*60)==2);
    assert(p.tick(19000,true,level,s,true,22*60)==0);
    level=3;p.manual_level(19500,level);assert(p.tick(19500,true,level,s,true,23*60)==3);
    assert(p.tick(20000,true,level,s,true,7*60)==3); // next boundary cancels manual override
    level=0;p.manual_level(20500,level);assert(p.tick(20500,true,level,s,true,8*60)==0);
    assert(p.tick(21000,true,level,s,true,22*60)==0);
    assert(p.tick(22000,true,level,s,true,7*60)==3 && level==3); // scheduled on restores level
    assert(p.tick(23000,true,level,s,false,0)==3); // clock unavailable cannot black out display
    s.on_minute=22*60;s.off_minute=7*60;assert(s.active(23*60)&&s.active(6*60)&&!s.active(12*60));
    s.off_minute=s.on_minute;assert(s.active(12*60));
    s.on_minute=1440;assert(!s.valid());
    BacklightPolicy b;level=4;assert(b.tick(0xffffff00u,false,level,s)==2);
    assert(b.tick(uint32_t(0xffffff00u+15000u),false,level,s)==0);
    puts("backlight: DC/battery transitions, any-key wake, medium/off, daily/overnight schedule, manual boundary override and wrap passed");
}
