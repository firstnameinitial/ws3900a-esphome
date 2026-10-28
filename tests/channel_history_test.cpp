// SPDX-License-Identifier: GPL-3.0-only
#include "channel_history.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    ChannelHistory h; assert(h.valid());
    assert(h.sample(1,0x310001,15,70));h.sample(1,0x310001,12,65);
    assert(h.channel[0].t_max==15 && h.channel[0].t_min==12);
    h.reset(true); assert(!(h.channel[0].valid&2) && h.channel[0].valid&1);
    h.sample(1,0x310001,13,66);assert(h.channel[0].t_max==13 && h.channel[0].t_min==12);
    h.sample(1,0x310002,20,50);assert(h.channel[0].t_min==20 && h.channel[0].h_max==50);
    h.sample(2,0x310003,NAN,0);assert(h.channel[1].valid==12 && h.channel[1].h_min==0);
    h.forget(1);assert(h.channel[0].valid==0 && h.channel[0].key==0);
    assert(!h.sample(0,1,0,0) && !h.sample(9,1,0,0));
    puts("Channel extrema: source identity, independent resets, missing/zero and range passed");
}
