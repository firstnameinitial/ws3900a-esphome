// SPDX-License-Identifier: GPL-3.0-only
// Compile with -DESP_PLATFORM -Itests/power_stubs to exercise the IDF branch.
#include "console_power.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    {
        ConsolePower p;assert(p.begin());
        assert(adc_test::config.atten==3 && adc_test::config.bitwidth==12);
        assert(p.sample(0));assert(adc_test::reads==5 && adc_test::last_average==1002);
        assert(p.known() && p.millivolts()==4498 && !p.low());
        assert(!p.sample(29999) && adc_test::reads==5);
        adc_test::cal_mv=0;assert(p.sample(30000) && p.known() && p.low());
        adc_test::read_error=7;assert(!p.sample(60000) && !p.known() && !p.low() && p.error()==7);
        assert(adc_test::reads==11); // stop after first failed conversion
        adc_test::read_error=0;adc_test::convert_error=8;
        assert(!p.sample(90000) && !p.known() && p.error()==8);
        adc_test::convert_error=0;adc_test::cal_mv=2487;
        assert(p.sample(0xfffff000));
        assert(!p.sample(5000)); // millis wrap, interval still <30s
        assert(p.sample(30000));
    }
    assert(adc_test::created==adc_test::deleted && adc_test::cal_created==adc_test::cal_deleted);
    adc_test::calibration_error=9;
    {
        ConsolePower p;assert(!p.begin() && !p.known() && p.error()==9);
        assert(!p.sample(0) && !p.known() && !p.low());
    }
    assert(adc_test::created==adc_test::deleted);
    adc_test::calibration_error=0;adc_test::new_error=10;
    { ConsolePower p;assert(!p.begin() && p.error()==10); }
    assert(adc_test::created==adc_test::deleted);
    puts("console ADC: IDF configuration, calibrated 5-read mean, 30s/millis wrap, failures/cleanup and ownership passed");
}
