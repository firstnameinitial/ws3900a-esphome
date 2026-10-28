#pragma once
#include "esp_wifi.h"
constexpr int ESP_NETIF_OP_SET=1,ESP_NETIF_REQUESTED_IP_ADDRESS=2,ESP_NETIF_CAPTIVEPORTAL_URI=3;
inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    if(std::strcmp(key,"WIFI_AP_DEF")) __builtin_trap();
    return hotspot_idf_test::netif_present?&hotspot_idf_test::netif:nullptr;
}
inline esp_err_t esp_netif_dhcps_stop(esp_netif_t *) { ++hotspot_idf_test::dhcp_stops;return ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED; }
inline esp_err_t esp_netif_set_ip_info(esp_netif_t *,const esp_netif_ip_info_t *info) { hotspot_idf_test::ip=*info;return hotspot_idf_test::dhcp_error; }
inline esp_err_t esp_netif_dhcps_option(esp_netif_t *,int,int option,void *data,uint32_t len) {
    if(option==ESP_NETIF_CAPTIVEPORTAL_URI) hotspot_idf_test::portal_uri.assign(static_cast<char*>(data),len);
    return 0;
}
inline esp_err_t esp_netif_dhcps_start(esp_netif_t *) { ++hotspot_idf_test::dhcp_starts;return 0; }
