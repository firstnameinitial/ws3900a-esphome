// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "adc_oneshot.h"
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
struct adc_cali_curve_fitting_config_t { int unit_id=0,chan=0,atten=0,bitwidth=0; };
inline int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t*c,adc_cali_handle_t*h) {
    if(c->unit_id!=ADC_UNIT_1 || c->chan!=ADC_CHANNEL_0 || c->atten!=ADC_ATTEN_DB_12 || c->bitwidth!=ADC_BITWIDTH_12)
        return ESP_ERR_INVALID_ARG;
    if(adc_test::calibration_error)return adc_test::calibration_error;
    ++adc_test::cal_created;*h=reinterpret_cast<void*>(2);return ESP_OK;
}
inline int adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t) {++adc_test::cal_deleted;return ESP_OK;}
