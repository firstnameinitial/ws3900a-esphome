#pragma once
#include "esp_wifi.h"
#include <cstdio>
namespace esphome {
namespace network {
struct IPAddress {
    uint32_t addr;
    IPAddress(uint8_t a,uint8_t b,uint8_t c,uint8_t d):addr(uint32_t(a)<<24|uint32_t(b)<<16|uint32_t(c)<<8|d) {}
    explicit IPAddress(const esp_ip4_addr_t *value):addr(value->addr) {}
    operator esp_ip4_addr_t() const { return {addr}; }
    void operator+=(unsigned value) {addr+=value;}
    void str_to(char *out) const {std::snprintf(out,16,"%u.%u.%u.%u",addr>>24,(addr>>16)&255,(addr>>8)&255,addr&255);}
};
}
namespace wifi {
struct ManualIP { network::IPAddress static_ip{192,168,7,1},gateway{192,168,7,1},subnet{255,255,255,0}; };
struct WiFiAP {
    std::string ssid="test-console",password="test-only-password";
    bool hidden=true;
    std::optional<ManualIP> manual;
    const std::string &get_ssid() const {return ssid;}
    const std::string &get_password() const {return password;}
    bool get_hidden() const {return hidden;}
    bool has_channel() const {return true;}
    uint8_t get_channel() const {return 1;}
    std::optional<ManualIP> get_manual_ip() const {return manual;}
};
struct WiFiComponent {
    WiFiAP ap;
    bool connected=true,has=true;
    WiFiAP get_ap() const {return ap;}
    bool has_ap() const {return has;}
    bool is_connected() const {return connected;}
};
inline WiFiComponent test_wifi;
inline WiFiComponent *global_wifi_component=&test_wifi;
}
}
