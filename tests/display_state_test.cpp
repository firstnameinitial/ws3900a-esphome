// SPDX-License-Identifier: GPL-3.0-only
#include "display_state.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;
int main() {
    UiState shared, attic;
    Reading array; array.family=Family::WS90;
    array.temp_c=9.4f; array.humidity=95; array.mark(F_TEMP); array.mark(F_HUM);
    array.uv=0; array.mark(F_UV); array.rain_mm=59.8f; array.rain_rate_mmh=0;
    update_shared_reading(array,shared,true,true);
    assert(shared.v[UiState::T_OUT]==9.4f && shared.v[UiState::H_OUT]==95);
    Reading ch; ch.family=Family::WH31; ch.channel=1;
    ch.temp_c=15;ch.humidity=69;ch.mark(F_TEMP);ch.mark(F_HUM);
    map_reading(ch,attic); update_shared_reading(ch,shared,false,false);
    assert(attic.v[UiState::T_OUT]==15 && attic.v[UiState::H_OUT]==69);
    assert(shared.v[UiState::T_OUT]==9.4f && shared.v[UiState::H_OUT]==95);
    array.temp_c=30;array.humidity=20;array.rain_rate_mmh=100;
    update_shared_reading(array,shared,false,false);
    assert(shared.v[UiState::T_OUT]==9.4f && shared.v[UiState::RAIN_RATE]==0);
    update_shared_reading(array,shared,true,false);
    assert(shared.v[UiState::T_OUT]==9.4f && shared.v[UiState::RAIN_RATE]==100);
    UiState empty;update_shared_reading(ch,empty,false,false);assert(!empty.has(UiState::T_OUT));
    puts("display ownership: attic isolated, primary array/rain independent, missing stays missing passed");
}
