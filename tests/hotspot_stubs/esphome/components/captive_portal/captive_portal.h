#pragma once
#include "esp_wifi.h"
namespace esphome {namespace captive_portal {
struct CaptivePortal {
    bool active=false;
    bool is_active() const {return active;}
    void start() {active=true;++hotspot_idf_test::portal_starts;}
    void end() {active=false;++hotspot_idf_test::portal_ends;}
};
inline CaptivePortal test_portal;
inline CaptivePortal *global_captive_portal=&test_portal;
}}
