// SPDX-License-Identifier: GPL-3.0-only
#include "source_selection.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
static SourceCandidate sensor(uint32_t key, Family family, uint32_t fields, uint8_t model = 0) {
    return {key,family,model,true,true,true,fields,is_rain_family(family)};
}
int main() {
    constexpr uint32_t weather = F_TEMP|F_HUM|F_WIND|F_WDIR|F_GUST|F_UV|F_LIGHT;
    SourceCandidate s[] = {
        sensor(50,Family::WS90,weather), sensor(40,Family::WS85,F_WIND|F_WDIR|F_GUST),
        sensor(30,Family::WS80,weather), sensor(20,Family::WH25,F_TEMP|F_HUM,0xD),
        sensor(60,Family::WH40,0), sensor(70,Family::WH25,F_TEMP|F_HUM|F_PRESS,0xE),
        sensor(80,Family::WS68,F_WIND|F_WDIR|F_GUST|F_UV|F_LIGHT),
        sensor(90,Family::WH31,F_TEMP|F_HUM), sensor(10,Family::WH25,F_TEMP|F_HUM,0xE)
    };
    constexpr size_t n = sizeof(s)/sizeof(*s);
    assert(select_outdoor_source(s,n)==20);
    assert(select_wind_source(s,n)==40);
    assert(select_solar_source(s,n,F_UV)==50 && select_solar_source(s,n,F_LIGHT)==50);
    assert(select_indoor_override(s,n)==70);
    assert(select_rain_source(s,n,RainSourcePreference::Traditional)==60);
    assert(select_rain_source(s,n,RainSourcePreference::Piezo)==40);
    s[3].fresh=false; assert(select_outdoor_source(s,n)==50);
    s[0].fields &= ~F_HUM; assert(select_outdoor_source(s,n)==30);
    assert(select_outdoor_source(s,n,0,F_TEMP)==50); // field-specific fallback
    s[1].fields &= ~F_GUST; assert(select_wind_source(s,n)==50);
    assert(select_wind_source(s,n,0,F_WIND)==40);
    s[4].enabled=false; assert(select_rain_source(s,n,RainSourcePreference::Traditional)==40);
    s[1].rain_present=false; assert(select_rain_source(s,n,RainSourcePreference::Piezo)==50);
    s[0].rain_present=false; assert(select_rain_source(s,n,RainSourcePreference::Traditional)==0);
    s[5].fields &= ~F_PRESS; assert(select_indoor_override(s,n)==0);
    assert(select_indoor_override(s,n,0,F_TEMP)==0);
    assert(select_outdoor_source(s,n,0,0)==0 && select_outdoor_source(s,n,0,F_PRESS)==0);
    assert(select_wind_source(s,n,0,F_TEMP)==0 && select_solar_source(s,n,F_WIND)==0);
    assert(select_solar_source(nullptr,100,F_UV)==0);
    SourceCandidate ties[] = {sensor(8,Family::WS90,weather),sensor(4,Family::WS90,weather)};
    assert(select_outdoor_source(ties,2)==4);
    assert(select_outdoor_source(ties,2,8)==8);
    std::swap(ties[0],ties[1]); assert(select_outdoor_source(ties,2,8)==8);
    ties[1].seen=false; assert(select_outdoor_source(ties,2,8)==4);
    ties[0].key=0; assert(select_outdoor_source(ties,2)==0);
    SourceCandidate priorities[] = {sensor(1,Family::WH24,weather),sensor(2,Family::WS68,weather),
        sensor(3,Family::WS80,weather),sensor(4,Family::WS90,weather),sensor(5,Family::WS85,weather)};
    for (size_t end=5;end;--end) assert(select_wind_source(priorities,end)==end);
    SourceCandidate gauges[] = {sensor(1,Family::WH65,0),sensor(2,Family::WN20,0),sensor(3,Family::WH40,0)};
    for (size_t end=3;end;--end) assert(select_rain_source(gauges,end,RainSourcePreference::Traditional)==end);
    puts("source selection: independent priorities, complete field masks, fresh/accepted/enabled gating, rain fallback and stable ties passed");
}
