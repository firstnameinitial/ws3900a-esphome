#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <optional>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_ERR_INVALID_ARG = 2;
constexpr int ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED = 3, ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED = 4;
enum wifi_mode_t { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA };
enum wifi_second_chan_t { WIFI_SECOND_CHAN_NONE };
constexpr int WIFI_AUTH_OPEN = 0, WIFI_AUTH_WPA2_PSK = 3, WIFI_CIPHER_TYPE_CCMP = 4, WIFI_IF_AP = 1;
struct wifi_config_t { struct { uint8_t ssid[32]{}, password[64]{}; uint8_t ssid_len=0,channel=0,ssid_hidden=0,max_connection=0; uint16_t beacon_interval=0; int authmode=0,pairwise_cipher=0; } ap; };
struct wifi_sta_list_t { int num = 0; };
struct esp_ip4_addr_t { uint32_t addr=0; };
struct esp_netif_ip_info_t { esp_ip4_addr_t ip,gw,netmask; };
struct esp_netif_t {};
namespace hotspot_idf_test {
inline wifi_mode_t mode = WIFI_MODE_STA;
inline wifi_config_t config;
inline esp_netif_ip_info_t ip;
inline esp_netif_t netif;
inline bool netif_present = true;
inline int clients=0, config_error=0, dhcp_error=0, station_error=0, mode_error=0;
inline unsigned mode_changes=0, config_calls=0, dhcp_stops=0, dhcp_starts=0, portal_starts=0, portal_ends=0;
inline std::string portal_uri;
inline void reset() {
    mode=WIFI_MODE_STA;config={};ip={};netif_present=true;clients=0;
    config_error=dhcp_error=station_error=mode_error=0;
    mode_changes=config_calls=dhcp_stops=dhcp_starts=portal_starts=portal_ends=0;portal_uri.clear();
}
}
inline esp_err_t esp_wifi_get_mode(wifi_mode_t *out) { *out=hotspot_idf_test::mode;return hotspot_idf_test::mode_error; }
inline esp_err_t esp_wifi_set_mode(wifi_mode_t value) {
    // AP-only/NULL would drop STA and must fail this test immediately.
    if(value!=WIFI_MODE_STA && value!=WIFI_MODE_APSTA) __builtin_trap();
    hotspot_idf_test::mode=value;++hotspot_idf_test::mode_changes;return 0;
}
inline esp_err_t esp_wifi_get_channel(uint8_t *channel,wifi_second_chan_t *secondary) { *channel=11;*secondary=WIFI_SECOND_CHAN_NONE;return 0; }
inline esp_err_t esp_wifi_set_config(int interface,const wifi_config_t *config) {
    if(interface!=WIFI_IF_AP) __builtin_trap();
    ++hotspot_idf_test::config_calls;hotspot_idf_test::config=*config;return hotspot_idf_test::config_error;
}
inline esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t *out) { out->num=hotspot_idf_test::clients;return hotspot_idf_test::station_error; }
