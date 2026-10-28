#pragma once
namespace esphome { namespace wifi {
struct WiFiComponent { bool is_connected() const {return false;} };
inline WiFiComponent *global_wifi_component=nullptr;
}}
