// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cmath>
#include <cstdio>
#include "rain_total.h"
using namespace esphome::ecowitt_rf;
static void eq(float a,float b) { assert(std::fabs(a-b)<0.02f); }
static RainCalendarKeys date(uint16_t y,uint8_t m,uint8_t d,uint8_t h,const RainCalendarPolicy &p={}) {
    return rain_calendar_keys(y,rain_day_of_year(y,m,d),h,p);
}
int main() {
    RainCalendarPolicy p{8,1,10};
    const auto sep=date(2026,9,30,23,p), oct0=date(2026,10,1,0,p), oct8=date(2026,10,1,8,p);
    assert(sep.day==oct0.day && oct0.day+1==oct8.day);
    assert(sep.month!=oct0.month && sep.season+1==oct0.season);
    assert(date(2026,12,31,23).week==date(2027,1,1,0).week);
    assert(date(2026,9,26,23).week!=date(2026,9,27,0).week); // Sunday
    assert(date(2026,9,26,23,p).week==date(2026,9,27,0,p).week);
    assert(date(2026,9,27,23,p).week!=date(2026,9,28,0,p).week); // Monday
    assert(rain_day_of_year(2024,2,29)==60 && rain_day_of_year(2026,2,29)==0);
    assert(!rain_calendar_keys(2026,366,0).valid && !rain_calendar_keys(1969,1,0).valid);
    assert(!rain_calendar_keys(2026,1,24).valid && !date(2026,1,1,0,{24,0,1}).valid);

    RainAggregator r; RainAggregate a;
    auto sample=[&](float raw,uint32_t ms,uint16_t doy=273,uint8_t hour=23,uint8_t month=9) {
        r.sample((uint8_t)Family::WS90,1,raw,doy,hour,rain_week_key(2026,doy),month,2026,ms,a);
    };
    sample(59.8f,0); auto &b=r.buckets()[0];
    b.year_mm=503.6f; b.day_mm=2; b.week_mm=10;
    RainConsoleTotal total; assert(total.migrate_legacy(b)); eq(total.value(),59.8f);
    assert(total.migrated() && total.history_incomplete());
    assert(!total.migrate_legacy(b));
    sample(60.8f,1000); assert(total.observe(b)); eq(total.value(),60.8f);
    r.set_gain(2); sample(60.8f,2000); assert(!total.observe(b)); eq(total.value(),60.8f);
    sample(61.8f,3000); total.observe(b); eq(total.value(),62.8f);
    sample(0,4000); total.observe(b); eq(total.value(),62.8f); // raw reset
    sample(0.5f,5000); total.observe(b); eq(total.value(),63.8f);
    auto cp=total.checkpoint(b,r.calendar_policy()); assert(cp.valid());
    auto bad=cp; bad.total_mm+=1; assert(!bad.valid());
    bad=cp; bad.event_coverage_ms=86400001; bad.checksum=bad.digest(); assert(!bad.valid());
    bad=cp; bad.periods.valid=2; bad.checksum=bad.digest(); assert(!bad.valid());

    const auto rolling=RainCheckpoint::capture(b,1790000000,5000); assert(rolling.valid());
    RainAggregator restored; restored.set_gain(2); restored.set_count(1); auto &rb=restored.buckets()[0];
    rolling.seed(rb,2); RainConsoleTotal again;
    assert(again.restore(cp,rb,restored.calendar_policy(),true)); eq(again.value(),63.8f);
    assert(rb.calendar_initialized && rb.period_keys.day==b.period_keys.day);
    assert(rb.event_coverage_ms==b.event_coverage_ms);
    assert(rolling.restore(rb,1790003600,100));
    assert(rb.event_coverage_ms==b.event_coverage_ms+3600000);
    restored.sample((uint8_t)Family::WS90,1,0.5f,273,23,rain_week_key(2026,273),9,2026,200,a);
    again.observe(rb); eq(again.value(),63.8f);
    restored.sample((uint8_t)Family::WS90,1,0.6f,273,23,rain_week_key(2026,273),9,2026,300,a);
    again.observe(rb); eq(again.value(),64.0f);

    // An interrupted multi-key save or rollback must retain the console anchor,
    // preserve newer old calendar values, and avoid replaying uncertain history.
    auto mismatch=b; mismatch.cal_total+=3; mismatch.day_mm+=3; mismatch.last_raw+=1.5f;
    RainConsoleTotal stale; assert(stale.restore(cp,mismatch,{},true)); eq(stale.value(),63.8f);
    eq(mismatch.day_mm,b.day_mm+3); assert(stale.history_incomplete());
    assert(!stale.observe(mismatch)); mismatch.cal_total+=0.2f; stale.observe(mismatch); eq(stale.value(),64.0f);
    auto interrupted=b;
    interrupted.day_mm+=2; interrupted.mark_doy-=1;
    interrupted.period_keys={}; interrupted.calendar_initialized=false;
    RainConsoleTotal dry_boundary;
    assert(dry_boundary.restore(cp,interrupted,{},true));
    assert(dry_boundary.history_incomplete() && interrupted.calendar_initialized);
    assert(interrupted.period_keys.day!=cp.periods.day); // derives from matching old marks
    eq(interrupted.day_mm,b.day_mm+2); eq(dry_boundary.value(),63.8f);
    auto provider=mismatch; provider.key+=1; provider.cal_total=700;
    stale.observe(provider); eq(stale.value(),64.0f); provider.cal_total+=0.5f;
    stale.observe(provider); eq(stale.value(),64.5f);
    RainConsoleTotal fresh; fresh.observe(provider); eq(fresh.value(),0);

    // A matching legacy calendar without windows may recover the total baseline,
    // but must never inherit complete event coverage with empty rolling windows.
    auto legacy=b; legacy.last_raw=-1; legacy.cal_total=0; legacy.event_coverage_ms=0;
    RainConsoleTotal legacy_total; assert(legacy_total.restore(cp,legacy,{},false));
    eq(legacy.cal_total,b.cal_total); assert(legacy.event_coverage_ms==0);
    assert(!legacy_total.observe(legacy)); eq(legacy_total.value(),63.8f);

    // Policy change preserves accumulated real measurements at the point of change;
    // subsequent DAILY/season/month/week boundaries are independent.
    r.set_gain(1); assert(r.set_calendar_policy(p)); sample(0.5f,6000);
    const float prior_day=b.day_mm,prior_week=b.week_mm;
    sample(0.5f,7000,274,0,10); eq(a.day_mm,prior_day); eq(a.week_mm,prior_week); eq(a.month_mm,0); eq(a.year_mm,0);
    sample(0.5f,8000,274,8,10); eq(a.day_mm,0);
    assert(!r.set_calendar_policy({0,2,1}));

    // Event must not use fresh/partial history as 24h of zero rain, including before NTP.
    RainAggregator events; events.set_count(1); auto &eb=events.buckets()[0];
    eb.key=((uint32_t)Family::WS90<<24)|2; eb.last_raw=eb.last_counter=10; eb.event_mm=7;
    auto event_sample=[&](float raw,uint32_t ms,uint32_t unix_s=0) {
        events.sample((uint8_t)Family::WS90,2,raw,0,0,0,0,0,ms,a,unix_s);
    };
    event_sample(10,0); event_sample(10,7200000); eq(a.event_mm,7);
    event_sample(10,86400000); eq(a.event_mm,0); // complete dry observation
    event_sample(10.9f,86401000); eq(a.event_mm,0.9f);
    // Full rolling history exists, but 59min dry is insufficient even when a short
    // hourly bucket boundary has already removed the rain.
    event_sample(10.9f,86401000+59*60000UL); eq(a.event_mm,0.9f);
    event_sample(10.9f,86401000+60*60000UL); eq(a.event_mm,0);
    event_sample(11.9f,91000000); eq(a.event_mm,1);
    event_sample(11.9f,95000000); eq(a.event_mm,1); // threshold is strictly <1mm
    event_sample(0,95001000); eq(a.event_mm,1); // reset invalidates complete-history proof
    assert(!a.event_history_complete);
    event_sample(0,99000000); eq(a.event_mm,1);
    // A counter reset before NTP must discard a very old persisted dry timestamp;
    // synchronization later cannot suddenly turn the missing interval into dry history.
    eb.last_rain_unix=1704067200UL;
    event_sample(1,99001000); event_sample(0,99002000); assert(eb.last_rain_unix==0);
    event_sample(0,99003000,1790000000); eq(a.event_mm,2);
    event_sample(0,102603000,1790003600); eq(a.event_mm,2);
    std::puts("rain policy/total tests passed");
}
