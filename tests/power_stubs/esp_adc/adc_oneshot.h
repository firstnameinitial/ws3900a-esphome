// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_NOT_SUPPORTED=2;
using adc_unit_t=int; using adc_channel_t=int; using adc_atten_t=int; using adc_bitwidth_t=int;
using adc_oneshot_unit_handle_t=void*; using adc_cali_handle_t=void*;
constexpr int ADC_UNIT_1=0,ADC_CHANNEL_0=0,ADC_ATTEN_DB_12=3,ADC_BITWIDTH_12=12;
struct adc_oneshot_unit_init_cfg_t { int unit_id=0,clk_src=0,ulp_mode=0; };
struct adc_oneshot_chan_cfg_t { int atten=0,bitwidth=0; };
namespace adc_test {
inline int new_error=0,config_error=0,calibration_error=0,read_error=0,convert_error=0;
inline int reads=0,created=0,deleted=0,cal_created=0,cal_deleted=0,last_average=-1,cal_mv=2487;
inline int raw[5]={1000,1001,1002,1003,1008};
inline adc_oneshot_chan_cfg_t config{};
}
inline int adc_oneshot_io_to_channel(int gpio,adc_unit_t*u,adc_channel_t*c) {
    if (gpio != 0) return ESP_ERR_INVALID_ARG;
    *u = ADC_UNIT_1;
    *c = ADC_CHANNEL_0;
    return ESP_OK;
}
inline int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t* cfg,adc_oneshot_unit_handle_t*h) {
    if(cfg->unit_id!=ADC_UNIT_1)return ESP_ERR_INVALID_ARG;
    if(adc_test::new_error)return adc_test::new_error;
    ++adc_test::created;*h=reinterpret_cast<void*>(1);return ESP_OK;
}
inline int adc_oneshot_config_channel(adc_oneshot_unit_handle_t,adc_channel_t channel,const adc_oneshot_chan_cfg_t*c) {
    if(channel!=ADC_CHANNEL_0)return ESP_ERR_INVALID_ARG;
    adc_test::config=*c;return adc_test::config_error;
}
inline int adc_oneshot_read(adc_oneshot_unit_handle_t,adc_channel_t,int*r) {
    *r=adc_test::raw[(adc_test::reads++)%5];return adc_test::read_error;
}
inline int adc_oneshot_del_unit(adc_oneshot_unit_handle_t) { ++adc_test::deleted;return ESP_OK; }
