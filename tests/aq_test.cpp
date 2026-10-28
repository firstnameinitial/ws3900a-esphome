// SPDX-License-Identifier: GPL-3.0-only
#include "air_quality.h"
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
using namespace esphome::ecowitt_rf;

static void seal_wh46(std::array<uint8_t, 21> &packet) {
    packet[19] = crc8(0x31, packet.data(), 19);
    packet[20] = add_sum(packet.data(), 20);
}
static void seal_wh45(std::array<uint8_t, 15> &packet) {
    packet[13] = crc8(0x31, packet.data(), 13);
    packet[14] = add_sum(packet.data(), 14);
}
int main() {
    static_assert(UiState::PM25 == 16 && UiState::PM10 == 17 && UiState::CO2 == 18);
    static_assert(UiState::RAIN_STATE == 27 && UiState::PM1 == 28 && UiState::AQI == 29);
    static_assert(F_CAPV == (1u << 9) && F_PM1 == (1u << 10) && F_CO2 == (1u << 13));
    // Recorded upstream rtl_433 WH46 example, including its independent CRC/sum.
    std::array<uint8_t, 21> example = {0x46,0,0x27,0xf1,2,0xb5,0x33,0x40,0x32,
        0x40,0x39,3,0x0b,0,0x2a,0,0x36,1,0x90,0xe4,0x16};
    Reading r;
    assert(decode_wh46(example.data(), example.size(), r));
    assert(r.family == Family::WH46 && r.id == 0x27f1);
    assert(std::fabs(r.pm1_ugm3 - 4.2f) < 0.0001f);
    assert(r.pm25_ugm3 == 5 && r.pm10_ugm3 == 6 && r.co2_ppm == 779);
    assert(std::fabs(r.pm25_exact_ugm3 - 5.0f) < 0.0001f);
    assert(r.has(F_PM1) && r.has(F_PM25) && r.has(F_PM10) && r.has(F_CO2));
    UiState u; map_reading(r, u);
    assert(u.has(UiState::PM1) && u.has(UiState::PM10) && u.has(UiState::CO2));
    assert(std::fabs(u.v[UiState::PM1] - 4.2f) < 0.0001f);

    auto fractional = example; fractional[8] = 0x2a; seal_wh46(fractional);
    Reading fine; assert(decode_wh46(fractional.data(), fractional.size(), fine));
    assert(fine.pm25_ugm3 == 4 && std::fabs(fine.pm25_exact_ugm3 - 4.2f) < 0.0001f);
    UiState exact; map_reading(fine, exact);
    assert(std::fabs(exact.v[UiState::PM25] - 4.2f) < 0.0001f);
    auto zero = example;
    zero[7] = zero[9] = 0x40; // USB supply flags remain intact.
    zero[8] = zero[10] = zero[11] = zero[12] = zero[13] = zero[14] = 0;
    seal_wh46(zero);
    Reading z;
    assert(decode_wh46(zero.data(), zero.size(), z));
    assert(z.has(F_PM1) && z.has(F_PM25) && z.has(F_PM10) && z.has(F_CO2));
    map_reading(z, u);
    assert(u.has(UiState::PM25) && u.v[UiState::PM25] == 0);
    assert(u.has(UiState::PM10) && u.v[UiState::PM10] == 0);
    assert(u.has(UiState::PM1) && u.v[UiState::PM1] == 0);
    assert(u.has(UiState::CO2) && u.v[UiState::CO2] == 0);
    float value = -1;
    assert(aq_row_value(1, u, value) && value == 0);
    assert(aq_next_row(0,u) == 1 && aq_next_row(1,u) == 2 && aq_next_row(2,u) == 3);
    assert(aq_next_row(3,u) == 4 && aq_next_row(4,u) == 0); // no invented AQI
    assert(!aq_row_available(5,u));

    auto unavailable = example;
    unavailable[7] = 0x7f;
    for (unsigned n : {8,9,10,11,12,13,14}) unavailable[n] = 0xff;
    seal_wh46(unavailable);
    Reading missing;
    assert(decode_wh46(unavailable.data(), unavailable.size(), missing));
    assert(!missing.has(F_PM1) && !missing.has(F_PM25) && !missing.has(F_PM10) && !missing.has(F_CO2));
    map_reading(missing,u);
    assert(!u.has(UiState::PM1) && !u.has(UiState::PM25) && !u.has(UiState::PM10) && !u.has(UiState::CO2));
    assert(aq_next_row(0,u) == 0 && aq_normalize_row(3,u) == 0);
    assert(aq_row_available(0,u) && !aq_row_value(0,u,value));
    assert(aq_row_slot(255) == UiState::N && aq_normalize_row(255,u) == 0);
    assert(aq_next_row(255,u) == 0);
    u.set(UiState::AQI,42);
    assert(aq_row_value(5,u,value) && value == 42 && aq_next_row(0,u) == 5);
    u.set(UiState::AQI,NAN); assert(!aq_row_available(5,u));
    u.set(UiState::AQI,INFINITY); assert(!aq_row_available(5,u));
    u.set(UiState::AQI,-1); assert(!aq_row_available(5,u));

    std::array<uint8_t,15> wh45{};
    wh45[0]=0x45; wh45[4]=2; wh45[5]=0x58;wh45[6]=50;wh45[7]=wh45[9]=0x40;
    seal_wh45(wh45);
    Reading combo;
    assert(decode_wh45(wh45.data(),wh45.size(),combo));
    u = UiState{};u.set(UiState::PM1,5);map_reading(combo,u);
    assert(!u.has(UiState::PM1));
    assert(aq_next_row(1,u)==3); // WH45 has no PM1.
    assert(aq_supports_lcd(Family::WH45) && aq_supports_lcd(Family::WH46));
    assert(!aq_supports_lcd(Family::WH0290) && !aq_supports_lcd(Family::WH43));

    std::array<uint8_t,8> wh41={0x41,0x2a,0x40,0,0x40,0,0,0};
    wh41[6]=crc8(0x31,wh41.data(),6);wh41[7]=add_sum(wh41.data(),7);
    Reading particle;assert(decode_wh0290(wh41.data(),wh41.size(),particle));
    assert(particle.has(F_PM25) && particle.has(F_PM10));
    assert(particle.pm25_ugm3==0 && particle.pm10_ugm3==0);
    assert(!particle.has(F_PM1) && !particle.has(F_CO2));
    example[8]^=1;Reading invalid;assert(!decode_wh46(example.data(),example.size(),invalid));
    assert(!decode_wh46(zero.data(),20,invalid));
    assert(!aq_date_show_weekday(4999) && aq_date_show_weekday(5000));
    assert(aq_date_show_weekday(9999) && !aq_date_show_weekday(10000));
    puts("AQ: upstream packet, PM1 tenths, zero/missing flags, bounded row cycle, unknown AQI, sensor applicability and date alternation passed");
}
