#pragma once
#include <cstdint>
constexpr int ESP_MAC_WIFI_STA=0;
inline int esp_read_mac(uint8_t *mac,int) {for(unsigned i=0;i<6;++i) mac[i]=i; return 0;}
