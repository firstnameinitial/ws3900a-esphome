// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include "startup_display.h"
using esphome::ecowitt_rf::StartupDisplay;
int main() {
    StartupDisplay s; uint8_t row=4; bool sun=false;
    s.tick(0,5,row,sun); assert(row==0 && sun);
    s.tick(19999,5,row,sun); assert(row==0 && sun);
    s.tick(20000,5,row,sun); assert(row==1 && !sun && s.done);
    row=3;sun=true;s.tick(25000,5,row,sun);assert(row==3&&sun);
    StartupDisplay missing;row=0;missing.tick(20000,0,row,sun);assert(row==0&&!sun);
    StartupDisplay manual;manual.aq_manual=manual.sun_manual=true;row=3;sun=false;
    manual.tick(1000,5,row,sun);manual.tick(20000,5,row,sun);assert(row==3&&!sun);
    StartupDisplay wrap;wrap.began=0xfffffff0;row=0;sun=false;
    wrap.tick(0x10,5,row,sun);assert(row==0&&sun);
    wrap.tick(0x5000,5,row,sun);assert(row==1&&!sun);
}
