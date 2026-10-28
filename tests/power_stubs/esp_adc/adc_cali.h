// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "adc_oneshot.h"
inline int adc_cali_raw_to_voltage(adc_cali_handle_t,int raw,int*mv) {
    adc_test::last_average=raw;*mv=adc_test::cal_mv;return adc_test::convert_error;
}
