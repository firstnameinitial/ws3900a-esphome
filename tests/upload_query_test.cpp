// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cstdio>
#include "ecowitt_upload.h"
using namespace esphome::ecowitt_rf;
static bool has(const std::string &q,const char *key) { return ("&"+q).find(std::string("&")+key+"=")!=std::string::npos; }
static void value(const std::string &q,const char *key,const char *expected) {
    assert(("&"+q+"&").find(std::string("&")+key+"="+expected+"&")!=std::string::npos);
}
int main() {
    UploadConfig cfg; cfg.passkey="TEST";
    for (auto family:{Family::WH0290,Family::WH43,Family::WH45,Family::WH46}) {
        Reading r; r.family=family;
        auto q=reading_query(cfg,r);
        for(auto key:{"pm25_ch1","pm25_co2","pm1_co2","pm10_co2","co2"}) assert(!has(q,key));
        r.mark(F_PM25|F_PM10|F_CO2|F_PM1); r.pm25_exact_ugm3=0;
        q=reading_query(cfg,r);
        const bool combo=family==Family::WH45||family==Family::WH46;
        value(q,combo?"pm25_co2":"pm25_ch1","0.0");
        if(combo) {value(q,"pm1_co2","0.0");value(q,"pm10_co2","0");value(q,"co2","0");}
        r.pm25_exact_ugm3=12.1f;r.pm25_ugm3=12;
        value(reading_query(cfg,r),combo?"pm25_co2":"pm25_ch1","12.1");
    }
    Reading outside;outside.family=Family::WH25;outside.model=0xD;outside.mark(F_TEMP|F_HUM);
    auto q=reading_query(cfg,outside);value(q,"tempc","0.0");value(q,"humidity","0");
    assert(!has(q,"tempinc")&&!has(q,"humidityin")&&!has(q,"baromrelhpa"));
    outside.model=0xE;q=reading_query(cfg,outside);assert(has(q,"tempinc")&&!has(q,"tempc"));
    outside.model=0;q=reading_query(cfg,outside);assert(has(q,"tempinc")&&!has(q,"tempc"));
    for(auto family:{Family::WH24,Family::WH65,Family::WS69,Family::WH40,Family::WN20,Family::WS85,Family::WS90}) {
        Reading r;r.family=family;r.allow_rain_upload=false;r.mark(F_TEMP|F_HUM|F_UV|F_LIGHT);
        r.temp_c=9.4f;r.rain_mm=700;r.rain_day_mm=30;r.rain_rate_mmh=5;
        q=reading_query(cfg,r);
        if(family!=Family::WH40 && family!=Family::WN20) {
            value(q,"tempc","9.4");value(q,"humidity","0");value(q,"uv","0");value(q,"solarradiation","0.0");
        }
        for(auto key:{"rainratemm","eventrainmm","hourlyrainmm","dailyrainmm","weeklyrainmm","monthlyrainmm","yearlyrainmm","totalrainmm","erain_piezomm","hrain_piezomm","drain_piezomm","wrain_piezomm","mrain_piezomm","yrain_piezomm","last24hrain_piezomm","rrain_piezomm","srain_piezo","maxdailygust","winddir_avg10m"}) assert(!has(q,key));
        const auto wu=wunderground_query("ID:KEY",r);
        for(auto key:{"rainin","dailyrainin","rainratein"}) assert(!has(wu,key));
        if(upload_station_outdoor(r)) value(wu,"tempf","48.9");
        else assert(!has(wu,"tempf"));
        std::string cloud="existing=1";add_cloud_rain_fields(cloud,r);assert(cloud=="existing=1");
        r.allow_rain_upload=true;r.rain_mm=r.rain_day_mm=r.rain_rate_mmh=0;
        q=reading_query(cfg,r);assert(has(q,"totalrainmm"));value(q,"totalrainmm","0.0");
        value(wunderground_query("ID:KEY",r),"rainin","0.00");
        cloud.clear();add_cloud_rain_fields(cloud,r);assert(!cloud.empty());
    }
    Reading selected_wind;selected_wind.family=Family::WS90;selected_wind.allow_rain_upload=false;
    selected_wind.mark(F_WIND|F_WDIR|F_GUST);selected_wind.wind_mps=1;selected_wind.gust_mps=2;
    q=reading_query(cfg,selected_wind);assert(has(q,"windspeedkmh")&&has(q,"winddir")&&has(q,"windgustkmh"));
    assert(!has(q,"tempc")&&!has(q,"totalrainmm"));
    Reading channel;channel.family=Family::WH31;channel.channel=1;channel.mark(F_TEMP|F_HUM);channel.allow_rain_upload=false;
    q=reading_query(cfg,channel);assert(has(q,"temp1c")&&has(q,"humidity1")&&!has(q,"tempc"));
    std::puts("upload actual query tests passed: AQ zero/missing/exact, WN32 indoor/outdoor, selected rain gates, WH31 continuity");
}
