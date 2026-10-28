// SPDX-License-Identifier: GPL-3.0-only
#include "panel.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    Panel p; PanelSettings cfg; p.cfg=&cfg;
    static_assert(sizeof(PanelSettings)==14);
    assert(Panel::step_def(0).field==6 && Panel::step_def(1).field==7);
    assert(Panel::step_def(2).field==Panel::F_HOUR && Panel::step_def(6).field==Panel::F_DAY);
    assert(Panel::step_def(7).field==1 && Panel::step_def(8).field==Panel::F_REL);
    assert(Panel::step_def(13).field==9 && Panel::step_def(14).field==8);
    p.handle({Btn::SET,EvKind::LONG});
    for (unsigned i=1;i<=14;++i) { p.handle({Btn::SET,EvKind::SHORT}); assert(p.mode==DMode::SETTING && p.setting_step==i); }
    p.handle({Btn::SET,EvKind::SHORT}); assert(p.mode==DMode::NORMAL);
    p.has_co2_calibration=true;p.handle({Btn::SET,EvKind::LONG});
    for(unsigned i=0;i<15;++i)p.handle({Btn::SET,EvKind::SHORT});
    assert(p.mode==DMode::SETTING && p.setting_step==15);
    p.clear_fx();p.handle({Btn::WIND,EvKind::SHORT});assert(p.fx_co2_calibrate);
    p.mode=DMode::SETTING;p.setting_step=2;p.clear_fx();p.handle({Btn::INC,EvKind::SHORT});
    assert(p.fx_clock && p.clock_field==ClockField::HOUR && p.clock_delta==1 && !p.fx_settings);
    p.clear_fx();p.setting_step=6;p.handle({Btn::DOWN,EvKind::REPEAT});
    assert(p.fx_clock && p.clock_field==ClockField::DAY && p.clock_delta==-1);
    p.clear_fx();p.setting_step=8;p.handle({Btn::INC,EvKind::REPEAT});assert(p.fx_rel && p.rel_steps==10);
    p.clear_fx();p.setting_step=10;cfg.unit_wind=3;p.handle({Btn::INC,EvKind::SHORT});assert(cfg.unit_wind==5);
    p.handle({Btn::INC,EvKind::SHORT});assert(cfg.unit_wind==0);
    p.mode=DMode::NORMAL;cfg.backlight=4;
    for(uint8_t v:{uint8_t(3),uint8_t(2),uint8_t(1),uint8_t(0),uint8_t(4)}) {
        p.handle({Btn::LIGHT,EvKind::SHORT});assert(cfg.backlight==v);
    }
    p.dc_power=false;p.backlight_lit=false;p.handle({Btn::LIGHT,EvKind::SHORT});assert(cfg.backlight==2);
    p.backlight_lit=true;p.handle({Btn::LIGHT,EvKind::SHORT});assert(cfg.backlight==0);
    p.aq_count=0;p.handle({Btn::INC,EvKind::SHORT});assert(p.aq_sel==0);
    p.aq_count=2;p.handle({Btn::INC,EvKind::SHORT});assert(p.aq_sel==1);
    p.handle({Btn::INC,EvKind::SHORT});assert(p.aq_sel==2);
    p.handle({Btn::INC,EvKind::SHORT});assert(p.aq_sel==0);
    mock::now=5000;p.tick();assert(p.date_sel==1);mock::now=10000;p.tick();assert(p.date_sel==0);
    p.aq_sel=2;p.aq_count=1;p.tick();assert(p.aq_sel==0);
    p.ext_chan_count=1;p.temp_sel=3;p.temp_auto=true;p.ext_chan_count=0;p.tick();assert(p.temp_sel==0);
    p.clear_fx();p.handle({Btn::TEMP,EvKind::LONG});assert(!p.fx_reregister);
    p.ext_chan_count=2;p.temp_sel=4;p.temp_auto=false;p.handle({Btn::TEMP,EvKind::XLONG});
    assert(p.fx_reregister && p.reregister_mode==1 && p.reregister_index==1);
    p.clear_fx();p.temp_auto=true;p.handle({Btn::TEMP,EvKind::XLONG});assert(p.reregister_mode==2);
    p.clear_fx();p.handle({Btn::SET,EvKind::XLONG});assert(p.fx_factory_reset);
    // Physical register scan: the actual five-second boundary, not only synthetic events.
    mock::reset(6,7,10,19);assert(gpl_init());Panel held;held.cfg=&cfg;PanelEvent e;
    mock::lcd[3]=0x42;mock::now=50;assert(!held.event(e));
    mock::now=100;assert(!held.event(e)); // stable TEMP down at100ms
    mock::now=2100;assert(!held.event(e));
    mock::now=5099;assert(!held.event(e));
    mock::now=5100;assert(held.event(e) && e.btn==Btn::TEMP && e.kind==EvKind::XLONG);
    held.handle(e);assert(held.fx_reregister);
    mock::now=5200;assert(!held.event(e));
    mock::reset(6,7,10,19);assert(gpl_init());Panel chord;chord.cfg=&cfg;
    mock::lcd[3]=0x86;mock::now=50;assert(!chord.event(e));
    mock::now=100;assert(!chord.event(e));
    mock::now=2100;assert(!chord.event(e));assert(chord.mode==DMode::NORMAL);
    mock::now=5100;assert(chord.event(e) && e.kind==EvKind::XLONG && e.btn==Btn::SET);
    chord.handle(e);assert(chord.fx_factory_reset && chord.mode==DMode::NORMAL);
    mock::reset(6,7,10,19);assert(gpl_init());Panel ap;ap.cfg=&cfg;
    mock::lcd[3]=0x4a;mock::now=50;assert(!ap.event(e));mock::now=100;assert(!ap.event(e));
    mock::now=2100;assert(ap.event(e) && e.kind==EvKind::CHORD_WIFI);
    ap.handle(e);assert(ap.fx_wifi_hotspot && !ap.fx_reregister);
    mock::now=5100;assert(!ap.event(e));
    puts("panel controls: manual settings/clock deltas, AQ/date, power-aware brightness, sparse channels and physical five-second registration passed");
}
