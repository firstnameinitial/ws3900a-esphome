// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cstdio>
#include "upload_selection.h"
#include "ecowitt_upload.h"
using namespace esphome::ecowitt_rf;
static uint32_t key(Family f,uint32_t id) {return (uint32_t(f)<<24)|id;}
static bool has(const std::string &q,const char *k) {return ("&"+q).find(std::string("&")+k+"=")!=std::string::npos;}
int main() {
    UploadConfig cfg;cfg.passkey="TEST";
    const auto w90=key(Family::WS90,1),w90b=key(Family::WS90,2),w85=key(Family::WS85,3),wh40=key(Family::WH40,4);
    UploadSources s;s.th=w90b;s.wind=w90;s.uv=w90b;s.solar=w90b;s.rain=wh40;
    Reading input;input.family=Family::WS90;input.mark(F_TEMP|F_HUM|F_PRESS|F_WIND|F_WDIR|F_GUST|F_UV|F_LIGHT|F_BATV|F_CAPV);
    input.temp_c=15;input.rain_mm=200;input.battery_v=3;input.cap_v=5;
    Reading out;assert(select_upload_reading(input,w90,s,out));
    assert(out.has(F_WIND|F_WDIR|F_GUST) && !out.has(F_TEMP) && !out.has(F_HUM) && !out.has(F_PRESS));
    assert(!out.has(F_UV) && !out.has(F_LIGHT) && !out.allow_rain_upload && out.allow_battery_upload);
    auto q=reading_query(cfg,out);assert(has(q,"wh90batt") && has(q,"windspeedkmh") && !has(q,"tempc") && !has(q,"totalrainmm"));
    assert(select_upload_reading(input,w90b,s,out));
    assert(out.has(F_TEMP|F_HUM|F_UV|F_LIGHT) && !out.has(F_WIND) && !out.allow_battery_upload);
    q=reading_query(cfg,out);assert(has(q,"tempc")&&has(q,"uv")&&!has(q,"wh90batt")&&!has(q,"ws90cap_volt")&&!has(q,"winddir_avg10m"));
    assert(input.has(F_PRESS) && input.allow_rain_upload && input.allow_battery_upload); // input remains intact
    assert(!select_upload_reading(input,key(Family::WS90,99),s,out));
    // An assigned role with a missing value cannot manufacture an empty station packet.
    Reading absent;absent.family=Family::WS90;assert(!select_upload_reading(absent,w90b,s,out));
    s.wind=w85;Reading wind;wind.family=Family::WS85;wind.mark(F_WIND|F_WDIR|F_GUST);
    assert(select_upload_reading(wind,w85,s,out) && !out.allow_rain_upload);
    q=reading_query(cfg,out);assert(!has(q,"rrain_piezomm")&&has(q,"winddir"));
    Reading gauge;gauge.family=Family::WH40;
    assert(!select_upload_reading(gauge,key(Family::WH40,5),s,out));
    assert(select_upload_reading(gauge,wh40,s,out) && out.allow_rain_upload);
    q=reading_query(cfg,out);assert(has(q,"rainratemm")&&has(q,"totalrainmm"));
    assert(has(wunderground_query("ID:KEY",out),"rainratein"));

    const auto outside=key(Family::WH25,10),inside=key(Family::WH25,11);
    s.th=outside;s.indoor=inside;
    Reading thp;thp.family=Family::WH25;thp.model=0xD;thp.mark(F_TEMP|F_HUM);
    assert(select_upload_reading(thp,outside,s,out));q=reading_query(cfg,out);
    assert(has(q,"tempc")&&!has(q,"tempinc"));
    thp.model=0xE;thp.mark(F_PRESS);thp.battery_low=true;
    assert(!select_upload_reading(thp,outside,s,out));
    assert(select_upload_reading(thp,inside,s,out));q=reading_query(cfg,out);
    assert(has(q,"tempinc")&&has(q,"baromrelhpa")&&has(q,"wh25batt")&&!has(q,"console_batt"));
    const auto wu_indoor=wunderground_query("ID:KEY",out);
    assert(!has(wu_indoor,"tempf")&&!has(wu_indoor,"humidity")&&has(wu_indoor,"baromin"));
    s.indoor=0;assert(!select_upload_reading(thp,inside,s,out));

    Reading combo;combo.family=Family::WH46;combo.mark(F_TEMP|F_HUM|F_PM25|F_CO2);combo.temp_c=30;
    const auto aq=key(Family::WH46,20);s.aq=aq;
    assert(select_upload_reading(combo,aq,s,out));q=reading_query(cfg,out);
    assert(has(q,"tf_co2")&&has(q,"humi_co2")&&has(q,"pm25_co2")&&!has(q,"tempc"));
    assert(!has(wunderground_query("ID:KEY",out),"tempf"));
    assert(!select_upload_reading(combo,key(Family::WH46,21),s,out));
    for(auto family:{Family::WH43,Family::WH0290,Family::WH31,Family::WH51,Family::WH52,Family::WN34,Family::WH57}) {
        Reading independent;independent.family=family;independent.channel=1;independent.mark(F_TEMP|F_HUM|F_PM25);
        assert(select_upload_reading(independent,key(family,30),s,out));
        assert(out.present==independent.present && out.allow_battery_upload);
        if(family==Family::WH31) {q=reading_query(cfg,out);assert(has(q,"temp1c")&&has(q,"humidity1"));}
        assert(!has(wunderground_query("ID:KEY",out),"tempf"));
    }
    const auto start=http_success_count().load(std::memory_order_relaxed);
    for(int code:{-7,-4,-3,-2,-1,0,100,199,300,301,404,500}) record_http_result(code);
    assert(http_success_count().load(std::memory_order_relaxed)==start);
    record_http_result(200);record_http_result(204);record_http_result(299);
    assert(http_success_count().load(std::memory_order_relaxed)==start+3);
    volatile int result=0;http_request_async(true,"",80,"/","",&result);
    assert(http_success_count().load(std::memory_order_relaxed)==start+3); // queued/failed attempts are not success
    std::puts("upload selection tests passed: mixed owners, battery ownership, indoor/outdoor/AQ isolation, channels, actual HTTP2xx latch");
}
