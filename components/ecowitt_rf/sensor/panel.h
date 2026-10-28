// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Key debounce, hold/repeat and console display modes. SET cycles pages; holding SET
// enters settings. +/- adjusts values; LIGHT changes backlight or returns to Normal.
// Idle pages return to Normal after 30 seconds. Any key silences a ringing alarm.

#include <Arduino.h>
#include <cstdint>

#include "gpl811x.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ecowitt_rf {

enum class Btn : uint8_t {
    SET = 0, TEMP = 1, RAIN1 = 2, RAIN2 = 3, WIND = 4, INC = 5, DOWN = 6, LIGHT = 7,
    NONE = 0xff,
};

// Stable display-mode identifiers used by the component and diagnostics.
enum class DMode : uint8_t {
    NORMAL = 0, SETTING = 1, MAX = 2, MIN = 3, ALARM = 7, INFO = 10,
};

// Event kinds delivered to the component (stock: short press on release, hold flags at
// 2 s / 5 s while still held, 8.3 Hz repeat for +/- in Setting/Alarm, chords for reset/AP).
enum class EvKind : uint8_t { NONE = 0, SHORT = 1, LONG = 2, XLONG = 3, REPEAT = 4, CHORD_WIFI = 5 };

struct PanelEvent {
    Btn btn = Btn::NONE;
    EvKind kind = EvKind::NONE;
};

// Console-local settings, persisted by the component and edited by the panel.
// LAYOUT IS PERSISTED: do not add, remove or reorder fields. The fourteen-byte
// layout keeps former DST/rain-season storage compatible with the current fields.
struct PanelSettings {
    uint8_t unit_temp = 0;    // 0=C, 1=F
    uint8_t unit_press = 0;   // 0=hPa, 1=inHg, 2=mmHg
    uint8_t unit_wind = 0;    // 0=m/s, 1=km/h, 2=mph, 3=knots, 5=Beaufort; skip 4
    uint8_t unit_rain = 0;    // 0=mm, 1=in
    uint8_t unit_solar = 0;   // 0=kLux, 1=kfc, 2=W/m2
    uint8_t press_abs = 0;    // 0=relative, 1=absolute (Normal-page "-" toggle)
    uint8_t beep = 1;         // key beep on/off
    uint8_t h12 = 0;          // 0=24h, 1=12h
    uint8_t rst_hilo = 0;     // stock setting step 1 "RST HILO" (daily MAX/MIN reset switch).
                              // Stored and shown only -- our records are all-time.
    uint8_t hemisphere = 1;   // 1=north, 0=south
    uint8_t backlight = 3;    // 0..4 (off/low/mid/high/max)
    uint8_t alarm_h = 7, alarm_m = 0, alarm_on = 0;
};
static_assert(sizeof(PanelSettings) == 14, "persisted layout: field_ptr() indexes by declaration order");

struct Panel {
    // bit -> Btn index, learned on hardware 2026-08-22 (learn pass over the API log):
    //   bit7=SET  bit6=TEMP  bit5=WIND  bit4=+  bit3=-  bit2=LIGHT  bit1=RAIN2  bit0=RAIN1
    int8_t map[8] = {2, 3, 7, 6, 5, 4, 1, 0};

    uint8_t raw = 0;
    uint8_t stable = 0;
    uint32_t last_change = 0;
    uint32_t last_poll = 0;

    DMode mode = DMode::NORMAL;
    PanelSettings *cfg = nullptr;   // set by the component after load

    // -- display sub-state --
    uint8_t focus = 0;           // selected group: 0=temperature, 1=wind, 2=rain
    uint8_t setting_step = 0;    // menu step 0..15
    // Manual: outdoor, feels-like, dew, available channels, auto-scroll.
    static constexpr uint8_t TEMP_MAIN_N = 3;
    uint8_t temp_sel = 0;        // main-array variant or extra-channel selector
    bool temp_auto = false;
    uint32_t temp_rotate_ms = 0;
    uint8_t ext_chan_count = 0;  // extra T/H channels present, set by the component
    uint8_t wind_sel = 0;        // 0=speed 1=gust 2=dir 3=10min-avg
    // The + button switches the bottom-left row between month/day and weekday/day.
    uint8_t date_sel = 0;        // 0 = "JUL 31", 1 = "SUN 31"
    // Holding - switches the right-hand rotating zone between UVI/solar and sun times.
    bool rot_sun = false;        // false = UVI + SOLAR (mode 9), true = sunrise + sunset
    bool rain_event = false;     // RAIN1: false=rate, true=event
    bool rain_accum = false;     // RAIN2 switches the readout to the accumulation field
    uint8_t rain_range = 3;      // 3=day, 4=week, 5=month, 6=year, 7=total
    // MAX/MIN reuse the whole layout. These selectors choose the outdoor variant.
    uint8_t max_sel = 0;         // MAX outdoor variant 0..2
    uint8_t min_sel = 0;         // MIN outdoor variant 0..2
    bool mm_gust = true;         // MAX wind zone: true = gust, false = sustained speed
    uint8_t alarm_sub = 0;       // Mode 7 sub-menu: 0=hour, 1=minute
    bool alarm_edit = false;
    int32_t scratch = 0;         // reserved; clock/date settings are read-only
    // Setting step 9 (REL pressure): +/- steps accumulated for the component, which owns
    // the relative-pressure offset (units of the display unit's last digit).
    int16_t rel_steps = 0;
    // Keep a field solid briefly while adjusting it, so auto-repeat stays readable.
    uint32_t last_adj_ms = 0;

    // -- hold/repeat timers --
    Btn held = Btn::NONE;
    uint32_t press_ms = 0;
    bool long_fired = false, xlong_fired = false;
    uint32_t last_repeat = 0;
    bool repeat_active = false;

    uint32_t last_act = 0;

    // -- side-effect flags for the component (cleared by clear_fx) --
    bool fx_redraw = false;
    bool fx_backlight = false;       // backlight level changed -> reapply PWM
    bool fx_reset_max = false;       // SET 2 s in MAX
    bool fx_reset_min = false;       // SET 2 s in MIN
    bool fx_factory_reset = false;   // SET+LIGHT 5 s in Normal (log stub)
    bool fx_wifi_hotspot = false;    // TEMP+- 2 s in Normal (Wi-Fi AP / hotspot)
    bool fx_settings = false;        // a PanelSettings field changed -> persist
    bool fx_reregister = false;      // TEMP 2 s / 5 s in Normal (RF sensor search — stub)
    bool fx_beep = false;            // key beep (if cfg->beep)
    bool fx_rel = false;             // rel_steps pending (setting step 9)

    void clear_fx() {
        fx_redraw = fx_backlight = fx_reset_max = fx_reset_min = fx_factory_reset =
            fx_wifi_hotspot = fx_settings = fx_reregister = fx_beep = fx_rel = false;
    }

    // Raw key bits for a button (for chord detection).
    uint8_t btn_bit(Btn b) const {
        for (int i = 0; i < 8; i++)
            if (this->map[i] == (int8_t)b)
                return (uint8_t)(1u << i);
        return 0;
    }

    // Poll the GPL811x key regs (50 ms throttle, 20 ms debounce) and generate at most
    // one event per call. Call every loop; drain with a while loop.
    bool event(PanelEvent &e) {
        e.btn = Btn::NONE;
        e.kind = EvKind::NONE;
        uint32_t now = millis();

        // --- hardware poll (throttled) ---
        if (now - this->last_poll >= 50) {
            this->last_poll = now;
            uint8_t k = gpl_read_keys();
            if (k != this->raw) {
                ESP_LOGD("panel", "keys raw 0x%02X -> 0x%02X", this->raw, k);
                this->raw = k;
                this->last_change = now;
            }
            if (now - this->last_change >= 20 && this->stable != k) {
                uint8_t rising = k & (uint8_t)~this->stable;
                uint8_t falling = this->stable & (uint8_t)~k;
                this->stable = k;
                if (rising) {
                    for (int i = 0; i < 8; i++) {
                        if (rising & (1u << i) && this->map[i] >= 0 && this->map[i] < 8) {
                            this->held = (Btn)this->map[i];
                            this->press_ms = now;
                            this->long_fired = this->xlong_fired = false;
                            this->repeat_active = false;
                            this->last_repeat = now;
                            break;
                        }
                    }
                }
                if (falling && this->held != Btn::NONE && !(k & this->btn_bit(this->held))) {
                    // Released: emit the short press unless a hold already consumed it.
                    if (!this->long_fired && !this->xlong_fired && !this->repeat_active) {
                        e.btn = this->held;
                        e.kind = EvKind::SHORT;
                    }
                    this->held = Btn::NONE;
                }
            }
        }

        // --- hold / repeat timers for the currently held key ---
        if (e.kind == EvKind::NONE && this->held != Btn::NONE) {
            uint32_t dt = now - this->press_ms;
            // SET+LIGHT 5 s chord (Normal): factory reset. TEMP+- 2 s chord: Wi-Fi hotspot / RF test.
            bool set_light_chord = (this->stable & this->btn_bit(Btn::SET)) && (this->stable & this->btn_bit(Btn::LIGHT));
            bool temp_down_chord = (this->stable & this->btn_bit(Btn::TEMP)) && (this->stable & this->btn_bit(Btn::DOWN));

            if (set_light_chord) {
                if (!this->xlong_fired && dt >= 5000) {
                    this->xlong_fired = true;
                    this->long_fired = true;
                    e.btn = Btn::SET;
                    e.kind = EvKind::XLONG;  // component maps this to factory reset stub
                    return true;
                }
                if (dt >= 2000) {
                    // Suppress normal 2 s single-key action while holding SET+LIGHT chord
                    this->long_fired = true;
                }
            } else if (temp_down_chord) {
                if (!this->long_fired && dt >= 2000) {
                    this->long_fired = true;
                    e.btn = Btn::TEMP;
                    e.kind = EvKind::CHORD_WIFI;  // Wi-Fi hotspot / config mode
                    return true;
                }
            } else {
                // Single-key holds
                if (!this->xlong_fired && dt >= 5000) {
                    this->xlong_fired = true;
                }
                if (!this->long_fired && dt >= 2000) {
                    this->long_fired = true;
                    e.btn = this->held;
                    e.kind = EvKind::LONG;
                    return true;
                }
            }

            // 8.3 Hz auto-repeat: only +/-, only in Setting/Alarm, after 400 ms.
            if (!set_light_chord && !temp_down_chord && dt >= 400 &&
                (this->held == Btn::INC || this->held == Btn::DOWN) &&
                (this->mode == DMode::SETTING || this->mode == DMode::ALARM) &&
                now - this->last_repeat >= 120) {
                this->last_repeat = now;
                this->repeat_active = true;
                e.btn = this->held;
                e.kind = EvKind::REPEAT;
                return true;
            }
        }
        return e.kind != EvKind::NONE;
    }

    struct StepDef { int8_t field; int16_t lo, hi; };
    static constexpr int8_t F_NONE = -1;   // display-only / read-only step
    static constexpr int8_t F_REL = -2;    // REL pressure (component-owned offset)
    // field indices into PanelSettings (order of declaration)
    static StepDef step_def(uint8_t step) {
        static const StepDef DEFS[16] = {
            {6, 0, 1},          // 0 beep on/off
            {8, 0, 1},          // 1 RST HILO (daily MAX/MIN reset switch)
            {7, 0, 1},          // 2 12/24 h
            {F_NONE, 0, 23},    // 3 hour   (SNTP, read-only)
            {F_NONE, 0, 59},    // 4 minute (SNTP, read-only)
            {F_NONE, 2000, 2099},  // 5 year (SNTP, read-only)
            {F_NONE, 1, 12},    // 6 month  (SNTP, read-only)
            {F_NONE, 1, 31},    // 7 day    (SNTP, read-only)
            {1, 0, 2},          // 8 pressure unit
            {F_REL, 0, 0},      // 9 relative pressure
            {0, 0, 1},          // 10 temperature unit
            {2, 0, 5},          // 11 wind unit (0,1,2,3,5 -- 4 skipped like stock)
            {3, 0, 1},          // 12 rain unit
            {4, 0, 2},          // 13 solar unit
            {9, 0, 1},          // 14 hemisphere (1 = north)
            {F_NONE, 0, 1},     // 15 CO2 calibration (WS3910-only; display-only here)
        };
        return DEFS[step & 0x0f];
    }

    uint8_t *field_ptr(int8_t f) {
        if (f < 0 || this->cfg == nullptr)
            return nullptr;
        uint8_t *base = (uint8_t *)this->cfg;
        return base + f;   // PanelSettings is all-uint8_t, declaration order = index
    }

    int32_t step_value(uint8_t step) {
        StepDef d = step_def(step);
        uint8_t *p = field_ptr(d.field);
        if (p != nullptr)
            return *p;
        return this->scratch;
    }

    void step_adjust(bool inc, bool fast) {
        StepDef d = step_def(this->setting_step);
        if (d.field == F_REL) {
            this->rel_steps = (int16_t)(this->rel_steps + (inc ? 1 : -1) * (fast ? 10 : 1));
            this->fx_rel = true;
            this->last_adj_ms = millis();
            return;
        }
        uint8_t *p = field_ptr(d.field);
        if (p == nullptr)
            return;  // read-only (clock/date: SNTP) or display-only (CO2 calibration)
        int32_t v = *p;
        if (this->setting_step == 11) {
            v += inc ? 1 : -1;
            if (v == 4) v = inc ? 5 : 3;
            if (v > 5) v = 0;
            if (v < 0) v = 5;
        } else {
            v += inc ? 1 : -1;
            if (v > d.hi) v = d.lo;  // stock wraps unit/enum steps
            if (v < d.lo) v = d.hi;
        }
        *p = (uint8_t)v;
        this->fx_settings = true;
        this->last_adj_ms = millis();
    }

    // Apply one event to the state machine. Any event reloads the 30 s auto-return.
    void handle(const PanelEvent &e) {
        if (e.kind == EvKind::NONE)
            return;
        this->last_act = millis();
        this->fx_redraw = true;
        this->fx_beep = true;
        bool inc = (e.btn == Btn::INC);
        bool adj = inc || e.btn == Btn::DOWN;

        // Chords first (factory reset and Wi-Fi hotspot only fire from Normal, per stock).
        if (e.kind == EvKind::XLONG) {
            if (this->mode == DMode::NORMAL && e.btn == Btn::SET)
                this->fx_factory_reset = true;
            return;
        }
        if (e.kind == EvKind::CHORD_WIFI) {
            if (this->mode == DMode::NORMAL)
                this->fx_wifi_hotspot = true;
            return;
        }

        switch (this->mode) {
            case DMode::NORMAL:
                switch (e.btn) {
                    case Btn::SET:
                        if (e.kind == EvKind::LONG) {
                            this->mode = DMode::SETTING;
                            this->setting_step = 0;
                            this->scratch = 0;
                        } else if (e.kind == EvKind::SHORT) {
                            this->mode = DMode::MAX;
                            this->max_sel = 0;
                        }
                        break;
                    case Btn::TEMP:
                        if (e.kind == EvKind::LONG)
                            this->fx_reregister = true;  // RF sensor search (stub)
                        else if (e.kind == EvKind::SHORT) {
                            uint8_t n = (uint8_t)(TEMP_MAIN_N + this->ext_chan_count);
                            if (this->temp_auto) {
                                this->temp_auto = false;
                                this->temp_sel = 0;
                            } else if (this->temp_sel + 1 >= n) {
                                this->temp_auto = true;
                                this->temp_sel = 0;
                                this->temp_rotate_ms = millis();
                            } else {
                                ++this->temp_sel;
                            }
                            this->focus = 0;
                        }
                        break;
                    case Btn::WIND:
                        if (e.kind == EvKind::SHORT) {
                            this->wind_sel = (uint8_t)((this->wind_sel + 1) % 4);
                            this->focus = 1;
                        }
                        break;
                    case Btn::RAIN1:
                        if (e.kind == EvKind::SHORT) {
                            this->rain_event = !this->rain_event;
                            this->rain_accum = false;  // back to the rate/event field
                            this->focus = 2;
                        }
                        break;
                    case Btn::RAIN2:
                        if (e.kind == EvKind::SHORT) {
                            this->rain_range = (uint8_t)(this->rain_range >= 7 ? 3 : this->rain_range + 1);
                            this->rain_accum = true;   // the daily..total field
                            this->focus = 2;
                        }
                        break;
                    case Btn::INC:
                        if (e.kind == EvKind::SHORT)
                            this->date_sel ^= 1;   // bottom-left row: month <-> weekday
                        break;
                    case Btn::DOWN:
                        if (e.kind == EvKind::SHORT && this->cfg != nullptr) {
                            this->cfg->press_abs ^= 1;   // REL/ABS quick toggle
                            this->fx_settings = true;
                        } else if (e.kind == EvKind::LONG) {
                            // Stock / back-label: hold - switches UVI / sunrise & sunset display
                            this->rot_sun = !this->rot_sun;
                        }
                        break;
                    case Btn::LIGHT:
                        if (e.kind == EvKind::SHORT && this->cfg != nullptr) {
                            this->cfg->backlight = (uint8_t)((this->cfg->backlight + 1) % 5);
                            this->fx_backlight = true;
                            this->fx_settings = true;
                        }
                        break;
                    default: break;
                }
                break;

            case DMode::SETTING:
                if (e.btn == Btn::LIGHT && e.kind == EvKind::SHORT) {
                    this->mode = DMode::NORMAL;
                } else if (e.btn == Btn::SET && e.kind == EvKind::SHORT) {
                    if (++this->setting_step > 15) {
                        this->setting_step = 0;
                        this->mode = DMode::NORMAL;  // F_ReturnNormalMode at end of steps
                    }
                } else if (adj && (e.kind == EvKind::SHORT || e.kind == EvKind::REPEAT ||
                                   e.kind == EvKind::LONG)) {
                    step_adjust(inc, e.kind == EvKind::REPEAT);
                } else if (e.btn == Btn::WIND && e.kind == EvKind::SHORT && this->setting_step == 15) {
                    ESP_LOGW("panel", "CO2 calibration step: not implemented (WS3910-only)");  // TODO
                }
                break;

            case DMode::MAX:
                if (e.btn == Btn::LIGHT && e.kind == EvKind::SHORT) {
                    this->mode = DMode::NORMAL;
                } else if (e.btn == Btn::SET && e.kind == EvKind::SHORT) {
                    this->mode = DMode::MIN;
                    this->min_sel = 0;
                } else if (e.btn == Btn::SET && e.kind == EvKind::LONG) {
                    this->fx_reset_max = true;
                } else if (e.btn == Btn::TEMP && e.kind == EvKind::SHORT) {
                    this->max_sel = (uint8_t)((this->max_sel + 1) % 3);
                } else if (e.btn == Btn::WIND && e.kind == EvKind::SHORT) {
                    this->mm_gust = !this->mm_gust;   // wind speed <-> gust record
                } else if (adj && e.kind == EvKind::SHORT) {
                    this->max_sel = (uint8_t)((this->max_sel + (inc ? 1 : 2)) % 3);
                }
                break;

            case DMode::MIN:
                if (e.btn == Btn::LIGHT && e.kind == EvKind::SHORT) {
                    this->mode = DMode::NORMAL;
                } else if (e.btn == Btn::SET && e.kind == EvKind::SHORT) {
                    this->mode = DMode::ALARM;
                    this->alarm_sub = 0;
                    this->alarm_edit = false;
                } else if (e.btn == Btn::SET && e.kind == EvKind::LONG) {
                    this->fx_reset_min = true;
                } else if (e.btn == Btn::TEMP && e.kind == EvKind::SHORT) {
                    this->min_sel = (uint8_t)((this->min_sel + 1) % 3);
                } else if (adj && e.kind == EvKind::SHORT) {
                    this->min_sel = (uint8_t)((this->min_sel + (inc ? 1 : 2)) % 3);
                }
                break;

            case DMode::ALARM: {
                if (this->cfg == nullptr)
                    break;
                if (e.btn == Btn::LIGHT && e.kind == EvKind::SHORT) {
                    this->mode = DMode::NORMAL;
                } else if (e.btn == Btn::SET && e.kind == EvKind::LONG) {
                    this->alarm_edit = !this->alarm_edit;
                } else if (e.btn == Btn::SET && e.kind == EvKind::SHORT) {
                    if (++this->alarm_sub > 1) {
                        this->alarm_sub = 0;
                        this->mode = DMode::INFO;  // sub-menu done -> MAC/Info page
                    }
                } else if (e.btn == Btn::TEMP && e.kind == EvKind::SHORT) {
                    // Stock / back-label: in alarm-setting = switch hour/minute
                    this->alarm_sub ^= 1;
                } else if (e.btn == Btn::WIND && e.kind == EvKind::SHORT) {
                    this->cfg->alarm_on ^= 1;
                    this->fx_settings = true;
                } else if (adj && (e.kind == EvKind::SHORT || e.kind == EvKind::REPEAT ||
                                   e.kind == EvKind::LONG)) {
                    if (this->alarm_sub == 0)
                        this->cfg->alarm_h = (uint8_t)((this->cfg->alarm_h + (inc ? 1 : 23)) % 24);
                    else
                        this->cfg->alarm_m = (uint8_t)((this->cfg->alarm_m + (inc ? 1 : 59)) % 60);
                    this->fx_settings = true;
                    this->last_adj_ms = millis();
                }
                break;
            }

            case DMode::INFO:
                if ((e.btn == Btn::SET || e.btn == Btn::LIGHT) && e.kind == EvKind::SHORT)
                    this->mode = DMode::NORMAL;
                break;

            default:
                this->mode = DMode::NORMAL;
                break;
        }
    }

    void tick() {
        if (this->mode == DMode::NORMAL && this->temp_auto &&
            millis() - this->temp_rotate_ms >= 5000) {
            // Auto-scroll measured outdoor and registered channels; skip derived indices.
            if (this->ext_chan_count == 0) this->temp_sel = 0;
            else if (this->temp_sel < TEMP_MAIN_N) this->temp_sel = TEMP_MAIN_N;
            else if (++this->temp_sel >= TEMP_MAIN_N + this->ext_chan_count) this->temp_sel = 0;
            this->temp_rotate_ms = millis();
            this->fx_redraw = true;
        }
        if (this->mode != DMode::NORMAL && millis() - this->last_act > 30000) {
            this->mode = DMode::NORMAL;
            this->fx_redraw = true;
        }
    }
};

}  // namespace ecowitt_rf
}  // namespace esphome
