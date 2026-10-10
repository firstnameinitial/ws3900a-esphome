// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>
#include <initializer_list>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <cstdint>
#include <cstring>
#include "segment_font.h"

// GPL811x board interface: 16-bit address, bit15 selects reads, MSB first.
// SCLK/DIN are shared with the radio. Address/data transfer needs slow clocking,
// 80 us select setup, 45 us read turnaround/recovery, and 30 us select hold.
// Register and segment mappings are interoperability facts; see THIRD_PARTY_NOTICES.md.
namespace esphome { namespace ecowitt_rf {
constexpr uint8_t GPL_SCLK = 6, GPL_DIN = 7, GPL_DOUT = 1, GPL_CS = 18;
static const uint16_t SEG_POS0[14] = {0x1e04, 0x1601, 0x0e01, 0x0702, 0x0608, 0x1608, 0x0e08,
                                      0x0e02, 0x1602, 0x1e02, 0x0602, 0x1604, 0x0e04, 0x0604};
static const uint16_t SEG_POS1[14] = {0x1d40, 0x1d20, 0x0520, 0x0580, 0x0601, 0x1e01, 0x1580,
                                      0x0d20, 0x1540, 0x1520, 0x0540, 0x1d80, 0x0d40, 0x0d80};
static const uint16_t SEG_POS2[14] = {0x1d08, 0x1502, 0x0d02, 0x0510, 0x0d10, 0x1d10, 0x1510,
                                      0x0d04, 0x1504, 0x1d04, 0x0504, 0x1508, 0x0d08, 0x0508};


inline uint8_t gpl_transfer(uint8_t out, bool receive) {
    uint8_t in = 0;
    for (uint8_t mask = 0x80; mask; mask >>= 1) {
        if (!receive) digitalWrite(GPL_DIN, (out & mask) != 0);
        digitalWrite(GPL_SCLK, HIGH);
        delayMicroseconds(6);
        if (receive && digitalRead(GPL_DOUT)) in |= mask;
        digitalWrite(GPL_SCLK, LOW);
        delayMicroseconds(6);
    }
    return in;
}
class LcdTransaction {
 public:
    explicit LcdTransaction(uint16_t address) {
        pinMode(GPL_DIN, OUTPUT); // reclaim after radio released SDIO
        digitalWrite(GPL_CS, LOW);
        delayMicroseconds(80);
        gpl_transfer(uint8_t(address >> 8), false);
        gpl_transfer(uint8_t(address), false);
    }
    ~LcdTransaction() {
        delayMicroseconds(30);
        digitalWrite(GPL_CS, HIGH);
        delayMicroseconds(45);
    }
};
inline void gpl_write_reg(uint16_t address, uint8_t value) {
    LcdTransaction transaction(address & 0x7fff);
    gpl_transfer(value, false);
}
inline uint8_t gpl_read_reg(uint16_t address) {
    LcdTransaction transaction(address | 0x8000);
    delayMicroseconds(45);
    return gpl_transfer(0, true);
}
inline void gpl_set_seg(uint8_t *frame, uint16_t segment, bool on) {
    unsigned byte = segment >> 8;
    if (byte >= 64) return; // absent-segment sentinels must never address past the frame
    uint8_t mask = uint8_t(segment);
    frame[byte] = uint8_t((frame[byte] & ~mask) | (on ? mask : 0));
}
inline void gpl_push(const uint8_t *frame) {
    for (unsigned chunk = 0; chunk != 8; ++chunk) {
        LcdTransaction transaction(uint16_t(0x3e00 + chunk * 16));
        for (unsigned byte = 0; byte != 8; ++byte)
            gpl_transfer(frame[chunk * 8 + byte], false);
    }
}
inline void gpl_render_char(uint8_t *frame, uint8_t position, char character) {
    if (position >= 3) return;
    const uint16_t *positions[] = {SEG_POS0, SEG_POS1, SEG_POS2};
    uint16_t glyph = console_glyph(character);
    for (unsigned bit = 0; bit != 14; ++bit)
        gpl_set_seg(frame, positions[position][bit], (glyph & (1u << bit)) != 0);
}
inline void gpl_render(uint8_t *frame, const char *text) {
    memset(frame, 0, 64);
    for (unsigned pos = 0; pos != 3 && text[pos]; ++pos)
        gpl_render_char(frame, pos, text[pos]);
}
inline uint8_t &gpl_dbg_scratch() { static uint8_t value = 0xff; return value; }
inline bool gpl_init() {
    gpio_reset_pin(GPIO_NUM_18);
    gpio_reset_pin(GPIO_NUM_19);
    for (uint8_t cs : {uint8_t(10), uint8_t(19), GPL_CS}) {
        pinMode(cs, OUTPUT);
        digitalWrite(cs, HIGH);
    }
    pinMode(GPL_SCLK, OUTPUT);
    digitalWrite(GPL_SCLK, LOW);
    pinMode(GPL_DIN, OUTPUT);
    pinMode(GPL_DOUT, INPUT_PULLUP);
    bool ready = false;
    for (unsigned attempt = 0; attempt != 11; ++attempt) {
        gpl_write_reg(0x300, 0x5a);
        gpl_dbg_scratch() = gpl_read_reg(0x300);
        if (gpl_dbg_scratch() == 0x5a) { ready = true; break; }
        delay(1);
    }
    if (!ready) return false;
    // Board configuration sequence, including repeated latch writes and key-scan
    // strobes. These values describe the interface; no controller firmware is loaded.
    struct Setting { uint8_t address, value; };
    constexpr Setting setup[] = {
        {0,0}, {2,15}, {6,0}, {0x15,0}, {0x0b,0}, {3,0}, {0x15,0},
        {0x11,1}, {0x13,0}, {0x22,0x43}, {0x20,0xf9}, {0x21,0xff},
        {0x23,0xff}, {0x23,0xff}, {6,0}, {0x0b,0}, {3,1}, {0x0b,0},
        {3,1}, {0x15,0}, {0x11,1}, {0x13,0}, {7,0xc1}, {0x19,7}
    };
    for (const auto &setting : setup) gpl_write_reg(setting.address, setting.value);
    (void)gpl_read_reg(0x13); // latch boot jumper state before clearing key-scan status
    gpl_write_reg(0x16, 0); // buzzer off
    gpl_write_reg(0x22, 0x43);
    gpl_write_reg(0x11, 1);
    gpl_write_reg(0x13, 0);
    pinMode(5, INPUT_PULLDOWN); // switch/touch panel strap
    return true;
}
inline uint8_t gpl_read_keys() {
    uint8_t main_keys = gpl_read_reg(3);
    uint8_t extra_keys = gpl_read_reg(0x13);
    return (main_keys & 0xfc) | ((extra_keys >> 3) & 3);
}
inline void gpl_backlight_init() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_3_BIT;
    timer.timer_num = LEDC_TIMER_1;
    timer.freq_hz = 40000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);
    ledc_channel_config_t channel = {};
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.timer_sel = LEDC_TIMER_1;
    channel.gpio_num = 3;
    channel.duty = 0;
    ledc_channel_config(&channel);
}
inline void gpl_backlight(uint8_t duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty & 7);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
} }
