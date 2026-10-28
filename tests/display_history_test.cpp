// SPDX-License-Identifier: GPL-3.0-only
#include "display_history.h"
#include "panel.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
using namespace esphome::ecowitt_rf;
int main() {
    using H = DisplayHistory;
    H h;
    assert(h.valid() && !h.has(H::IN_TEMP, true));
    assert(h.sample(H::TEMP, 9.4f));
    assert(h.sample(H::TEMP, -0.1f));
    assert(h.sample(H::TEMP, 31.3f));
    assert(h.get(H::TEMP, false) == -0.1f && h.get(H::TEMP, true) == 31.3f);
    assert(!h.sample(H::TEMP, 15));
    assert(h.sample(H::RAIN_RATE, 0)); // dry is known, unlike missing
    assert(h.has(H::RAIN_RATE, true));
    assert(!h.sample(H::RAIN_RATE, NAN) && !h.sample(H::UV, INFINITY));
    assert(!h.has(H::UV, true));
    h.reset(true); assert(!h.has(H::TEMP, true) && h.has(H::TEMP, false));
    h.sample(H::TEMP, 10); assert(h.get(H::TEMP, true)==10 && h.get(H::TEMP, false)==-0.1f);
    H restored; std::memcpy(&restored, &h, sizeof(h)); assert(restored.valid());
    restored.magic=0; assert(!restored.valid());
    restored=h; restored.high[H::TEMP]=NAN; assert(!restored.valid());
    restored=h; restored.valid_min |= 1u<<31; assert(!restored.valid());
    const float legacy[6] = {31.3f, -0.1f, 97, 32, 7.3f, 124};
    h.sample(H::IN_TEMP, 21);
    h.import_legacy(legacy, 15);
    assert(h.get(H::TEMP,true)==31.3f && h.get(H::TEMP,false)==-0.1f);
    assert(h.get(H::RAIN_RATE,true)==124 && h.get(H::IN_TEMP,true)==21);
    assert(h.legacy_signature==H::signature(legacy,15));
    float changed[6]; std::memcpy(changed,legacy,sizeof(legacy)); changed[0]=32;
    assert(H::signature(changed,15)!=h.legacy_signature);
    h.import_legacy(changed,3); // an older firmware extended temp, then reset gust/rate
    assert(h.get(H::TEMP,true)==32 && !h.has(H::GUST,true) && !h.has(H::RAIN_RATE,true));
    assert(h.get(H::IN_TEMP,true)==21);
    for (int i=0;i<=15;++i) assert(uvi_level(i)==(i==0?0:(i+2)/3));
    Panel p; PanelSettings cfg; p.cfg=&cfg; p.ext_chan_count=2;
    for (unsigned i=1;i<=4;++i) { p.handle({Btn::TEMP,EvKind::SHORT}); assert(p.temp_sel==i && !p.temp_auto); }
    p.handle({Btn::TEMP,EvKind::SHORT}); assert(p.temp_auto && p.temp_sel==0);
    mock::now+=5000; p.tick(); assert(p.temp_sel==3);
    mock::now+=5000; p.tick(); assert(p.temp_sel==4);
    mock::now+=5000; p.tick(); assert(p.temp_sel==0);
    p.handle({Btn::TEMP,EvKind::SHORT}); assert(!p.temp_auto && p.temp_sel==0);
    p.ext_chan_count=0;
    for (unsigned i=0;i<3;++i) p.handle({Btn::TEMP,EvKind::SHORT});
    assert(p.temp_auto); mock::now+=5000; p.tick(); assert(p.temp_sel==0);
    puts("display history: extrema, zero/missing, independent resets, persistence validation, UV boundaries, TEMP/manual/auto cycle passed");
}
