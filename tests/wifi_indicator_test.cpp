// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include "wifi_indicator.h"
using esphome::ecowitt_rf::WifiIndicator;
int main() {
    WifiIndicator w;
    assert(w.level(0,false,false,-80,8)==4);
    assert(w.level(250,false,false,-80,8)==0);
    assert(w.level(1000,true,false,-70,8)==0); // historic success cannot qualify a new association
    assert(w.level(2000,true,false,-70,8)==2);
    assert(w.level(3000,true,false,-70,9)==2);
    assert(w.level(4000,true,true,-70,9)==4);
    assert(w.level(4250,true,true,-70,9)==0);
    assert(w.level(5000,true,false,-70,9)==2); // AP did not erase association success
    w.level(6000,false,false,-70,9);
    assert(w.level(7000,true,false,-70,9)==0);
    assert(w.level(8000,true,false,-70,10)==2);
}
