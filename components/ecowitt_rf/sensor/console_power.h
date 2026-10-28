// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#if defined(ESP_PLATFORM)
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#endif

namespace esphome { namespace ecowitt_rf {
// Stock v1.4.8 board strap0/1: battery_read0x42028a4a divides calibrated
// ADC1 channel0 mV by this ratio, clamps4500, sets low<=3600 and clears>3704.
// The same low icon covers absent batteries, including while DC is present;
// the binary provides no separate battery-present threshold.
struct ConsoleBatteryState {
    static constexpr float DIVIDER = 0.5528134107589722f;
    bool known = false;
    uint16_t millivolts = 0;
    bool battery_low = false;
    void invalidate() { known = false; millivolts = 0; }
    bool observe_battery_mv(int mv) {
        if (mv < 0) { invalidate(); return false; }
        millivolts = static_cast<uint16_t>(mv > 4500 ? 4500 : mv);
        if (millivolts <= 3600) battery_low = true;
        else if (millivolts > 3704) battery_low = false;
        known = true;
        return true;
    }
    bool observe_adc_mv(int calibrated_mv) {
        if (calibrated_mv < 0 || calibrated_mv > 3300) { invalidate(); return false; }
        return observe_battery_mv(static_cast<int>(static_cast<float>(calibrated_mv) / DIVIDER));
    }
};

// Owns one ADC1 handle; no GPIO output, radio bus, interrupts or timers.
// Call sample(millis()) from the component loop: at most five ADC reads per30s.
// ADC/calibration ownership or conversion failure leaves voltage unknown.
class ConsolePower {
 public:
    static constexpr uint32_t SAMPLE_INTERVAL_MS = 30000;
    ConsolePower() = default;
    ConsolePower(const ConsolePower &) = delete;
    ConsolePower &operator=(const ConsolePower &) = delete;
    ~ConsolePower() { end(); }
    bool begin() {
        end(); state_.invalidate(); sampled_ = false;
#if defined(ESP_PLATFORM)
        adc_unit_t unit;
        adc_channel_t channel;
        error_ = adc_oneshot_io_to_channel(0, &unit, &channel);
        if (error_ != ESP_OK) return false;
        if (unit != ADC_UNIT_1 || channel != ADC_CHANNEL_0) { error_ = ESP_ERR_INVALID_ARG; return false; }
        adc_oneshot_unit_init_cfg_t init{};
        init.unit_id = ADC_UNIT_1;
        error_ = adc_oneshot_new_unit(&init, &unit_);
        if (error_ != ESP_OK) return false;
        adc_oneshot_chan_cfg_t cfg{};
        cfg.atten = ADC_ATTEN_DB_12; // IDF5 name for stock attenuation enum3 (11dB)
        cfg.bitwidth = ADC_BITWIDTH_12;
        error_ = adc_oneshot_config_channel(unit_, ADC_CHANNEL_0, &cfg);
        if (error_ != ESP_OK) { end(); return false; }
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_curve_fitting_config_t calibration{};
        calibration.unit_id = ADC_UNIT_1;
        calibration.chan = ADC_CHANNEL_0;
        calibration.atten = cfg.atten;
        calibration.bitwidth = cfg.bitwidth;
        error_ = adc_cali_create_scheme_curve_fitting(&calibration, &calibration_);
        if (error_ != ESP_OK) { end(); return false; }
        return true;
#else
        error_ = ESP_ERR_NOT_SUPPORTED; end(); return false;
#endif
#else
        error_ = -1; return false;
#endif
    }
    bool sample(uint32_t now_ms) {
        if (sampled_ && static_cast<uint32_t>(now_ms - sampled_ms_) < SAMPLE_INTERVAL_MS) return false;
        sampled_ = true; sampled_ms_ = now_ms;
#if defined(ESP_PLATFORM)
        if (!unit_ || !calibration_) { state_.invalidate(); return false; }
        int sum = 0;
        for (unsigned i = 0; i < 5; ++i) {
            int raw = 0;
            error_ = adc_oneshot_read(unit_, ADC_CHANNEL_0, &raw);
            if (error_ != ESP_OK) { state_.invalidate(); return false; }
            if (raw < 0 || raw > 4095) { error_ = ESP_ERR_INVALID_ARG; state_.invalidate(); return false; }
            sum += raw;
        }
        int mv = 0;
        error_ = adc_cali_raw_to_voltage(calibration_, sum / 5, &mv);
        if (error_ != ESP_OK) { state_.invalidate(); return false; }
        if (!state_.observe_adc_mv(mv)) { error_ = ESP_ERR_INVALID_ARG; return false; }
        return true;
#else
        state_.invalidate(); return false;
#endif
    }
    bool known() const { return state_.known; }
    uint16_t millivolts() const { return state_.millivolts; }
    bool low() const { return state_.known && state_.battery_low; }
    int error() const { return error_; }
 private:
    void end() {
#if defined(ESP_PLATFORM)
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (calibration_) adc_cali_delete_scheme_curve_fitting(calibration_);
#endif
        calibration_ = nullptr;
        if (unit_) adc_oneshot_del_unit(unit_);
        unit_ = nullptr;
#endif
    }
    ConsoleBatteryState state_{};
    bool sampled_ = false;
    uint32_t sampled_ms_ = 0;
    int error_ = 0;
#if defined(ESP_PLATFORM)
    adc_oneshot_unit_handle_t unit_ = nullptr;
    adc_cali_handle_t calibration_ = nullptr;
#endif
};
}} // namespace esphome::ecowitt_rf
