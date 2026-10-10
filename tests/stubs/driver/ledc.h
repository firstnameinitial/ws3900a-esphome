// SPDX-License-Identifier: GPL-3.0-only
#pragma once
constexpr int LEDC_LOW_SPEED_MODE=0, LEDC_TIMER_3_BIT=3, LEDC_TIMER_1=1, LEDC_AUTO_CLK=0, LEDC_CHANNEL_0=0;
struct ledc_timer_config_t { int speed_mode, duty_resolution, timer_num, freq_hz, clk_cfg; };
struct ledc_channel_config_t { int speed_mode, channel, timer_sel, gpio_num, duty; };
inline unsigned mock_duty=0;
inline void ledc_timer_config(ledc_timer_config_t*) {}
inline void ledc_channel_config(ledc_channel_config_t*) {}
inline void ledc_set_duty(int,int,unsigned duty) { mock_duty=duty; }
inline void ledc_update_duty(int,int) {}
