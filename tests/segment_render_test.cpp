// SPDX-License-Identifier: GPL-3.0-only
#include "segment_render.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace esphome::ecowitt_rf;
bool lit(const uint8_t *frame, uint16_t id) { return (frame[id >> 8] & uint8_t(id)) != 0; }
int main() {
    const ForecastSegments expected[7] = {{true,false,false,false}, {true,true,false,false},
        {false,true,false,false}, {false,true,true,false}, {false,true,false,true},
        {false,true,true,false}, {false,true,false,true}};
    uint8_t frame[64]{};
    for (unsigned state = 0; state < 7; ++state) {
        draw_forecast_segments(frame, state, true);
        assert(lit(frame,SEG_FC_SUN)==expected[state].sun);
        assert(lit(frame,SEG_FC_CLOUD)==expected[state].cloud);
        assert(lit(frame,SEG_FC_RAIN)==expected[state].rain);
        assert(lit(frame,SEG_FC_SNOW)==expected[state].snow);
        draw_forecast_segments(frame,state,false);
        assert(lit(frame,SEG_FC_CLOUD)==expected[state].cloud);
        assert(lit(frame,SEG_FC_RAIN)==(state==3));
        assert(lit(frame,SEG_FC_SNOW)==(state==4));
    }
    draw_forecast_segments(frame,255,true);
    assert(!lit(frame,SEG_FC_SUN)&&!lit(frame,SEG_FC_CLOUD)&&!lit(frame,SEG_FC_RAIN)&&!lit(frame,SEG_FC_SNOW));
    const int delta[] = {50,40,30,20,10,0,-10,-20,-30,-40,-50};
    for (unsigned level=0;level<11;++level) {
        assert(pressure_trend_level(delta[level])==static_cast<int8_t>(level));
        draw_trend_segments(frame,level);
        for (unsigned i=0;i<11;++i) assert(lit(frame,BAR22[i])==(i==level));
    }
    assert(pressure_trend_level(49)==1 && pressure_trend_level(9)==5);
    assert(pressure_trend_level(-9)==5 && pressure_trend_level(-49)==9);
    draw_trend_segments(frame,-1);
    for (unsigned i=0;i<11;++i) assert(!lit(frame,BAR22[i]));
    std::memset(frame,0,sizeof(frame)); big_text(frame,"MAX");
    for (unsigned i=0;i<14;++i) assert(lit(frame,SEG_POS0[i])==bool(0x137u&(1u<<i)));
    const uint16_t captions[] = {SEG_ROW_DATE, SEG_DATE_DOT, SEG_AQ_CO2, SEG_AQ_PM1,
        SEG_AQ_PM25, SEG_AQ_PM10, SEG_AQ_AQI, SEG_AQ_PPM, SEG_AQ_UGM3};
    for (unsigned row = 0; row < 6; ++row) {
        std::memset(frame, 0xff, sizeof(frame));
        draw_aq_captions(frame,row);
        const bool expected_caption[] = {row==0,row==0,row==1,row==2,row==3,row==4,row==5,
                                          row==1,row>=2&&row<=4};
        for (unsigned i=0;i<9;++i) assert(lit(frame,captions[i])==expected_caption[i]);
    }
    draw_aq_captions(frame,255);
    for (uint16_t id:captions) assert(!lit(frame,id));
    draw_aq_link(frame,true,2,true);
    assert(lit(frame,SEG_AQ_RF_ANT)&&lit(frame,SEG_AQ_BATT));
    for (unsigned i=0;i<4;++i) assert(lit(frame,AQ_RF_BARS[i])==(i<2));
    draw_aq_link(frame,false,4,true);
    assert(!lit(frame,SEG_AQ_RF_ANT)&&!lit(frame,SEG_AQ_BATT));
    for (uint16_t id:AQ_RF_BARS) assert(!lit(frame,id));
    puts("segments: seven forecasts, storm blink, trend boundaries, physical M and exact AQ captions/units/link passed");
}
