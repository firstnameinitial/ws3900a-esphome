// SPDX-License-Identifier: GPL-3.0-only
#include "display.h"
#include "panel.h"
#include <cassert>
#include <climits>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    mock::reset(6,7,10,19);
    assert(gpl_init()); assert(gpl_dbg_scratch()==0x5a);
    assert(mock::lcd[0x16]==0 && mock::lcd[0x22]==0x43); // silent startup
    assert(mock::level[10] && mock::level[18] && mock::level[19]);
    uint8_t frame[64]; for(unsigned i=0;i<64;++i) frame[i]=uint8_t(i*3);
    pinMode(7,INPUT); // previous radio transaction released shared data pin
    mock::lcd_writes.clear(); gpl_push(frame);
    assert(mock::direction[7]==OUTPUT && mock::lcd_writes.size()==64);
    for(unsigned i=0;i<64;++i) {
        unsigned address=0x3e00+(i/8)*16+i%8;
        assert(mock::lcd[address]==frame[i]);
    }
    uint8_t b[6]; fmt_decimal(-5,b); assert(b[0]==5&&b[1]==SM_MINUS&&b[2]==SM_BLANK);
    fmt_decimal(0,b); assert(b[0]==0&&b[1]==SM_BLANK);
    fmt_decimal(10132,b); assert(b[0]==2&&b[4]==1);
    fmt_decimal(INT_MIN,b); for(auto x:b) assert(x==SM_MINUS);
    fmt_pair(7,b); assert(b[0]==7&&b[1]==0);
    assert(!fmt_codes(INT_MIN,b,6)); assert(!fmt_codes(-100,b,3));
    assert(!fmt_codes(0,b,0)); assert(fmt_codes(-99,b,3));
    assert(b[0]==SM_MINUS&&b[1]==9&&b[2]==9);
    assert(rose_sector(0)==0&&rose_sector(90)==4&&rose_sector(359)==0);
    assert(rose_sector(-90)==12&&rose_sector(720)==0);
    assert(console_glyph('I')==0x1109); // physical centre verticals + caps
    assert(console_glyph('R')==0x04f3); // lower-right diagonal remapping
    for(unsigned c=32;c<127;++c) assert(console_glyph(char(c))<0x4000);
    assert(small_glyph(SM_MINUS)==0x40 && small_glyph(SM_BLANK)==0);
    memset(frame,0,sizeof(frame)); big_text(frame,""); big_text(frame,"I");
    assert(frame[0x1602>>8] & 2); // centre-upper of first glyph
    gpl_set_seg(frame,0x4620,true); // sentinel is safe
    gpl_backlight_level(0); assert(mock_duty==8);
    gpl_backlight_level(4); assert(mock_duty==0);
    mock::lcd[3]=0xfc; mock::lcd[0x13]=0x18; assert(gpl_read_keys()==0xff);
    Panel panel; PanelSettings settings; panel.cfg=&settings;
    panel.handle({Btn::SET,EvKind::SHORT}); assert(panel.mode==DMode::MAX);
    panel.handle({Btn::SET,EvKind::SHORT}); assert(panel.mode==DMode::MIN);
    panel.handle({Btn::LIGHT,EvKind::SHORT}); assert(panel.mode==DMode::NORMAL);
    panel.handle({Btn::SET,EvKind::LONG}); assert(panel.mode==DMode::SETTING);
    puts("display: SPI/frame RAM, fonts, limits, text safety, keys and menu passed");
}
