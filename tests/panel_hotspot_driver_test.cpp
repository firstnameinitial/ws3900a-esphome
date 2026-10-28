// SPDX-License-Identifier: GPL-3.0-only
// Compile with USE_ESP32/USE_WIFI_AP and hotspot_stubs to exercise native branch.
#include "panel_hotspot.h"
#include <cassert>
#include <cstdio>
using namespace esphome;
using namespace esphome::ecowitt_rf;
using namespace hotspot_idf_test;
static void clean() {reset();wifi::test_wifi={};captive_portal::test_portal={};}
int main() {
    clean();
    {
        PanelHotspot h;assert(h.start(100));
        assert(mode==WIFI_MODE_APSTA && wifi::test_wifi.connected);
        assert(config_calls==1 && config.ap.channel==11 && config.ap.ssid_len==12);
        assert(config.ap.authmode==WIFI_AUTH_WPA2_PSK && config.ap.pairwise_cipher==WIFI_CIPHER_TYPE_CCMP);
        assert(config.ap.ssid_hidden && config.ap.max_connection==5 && config.ap.beacon_interval==100);
        assert(dhcp_stops==1 && dhcp_starts==1 && ip.ip.addr==0xc0a80401);
        assert(portal_uri=="http://192.168.4.1/wifi" && portal_starts==1);
        h.tick(300100);assert(mode==WIFI_MODE_STA && portal_ends==1);
    }
    clean();
    {
        wifi::test_wifi.ap.manual=wifi::ManualIP{};
        wifi::test_wifi.ap.password.clear();
        PanelHotspot h;assert(h.start(0));
        assert(config.ap.authmode==WIFI_AUTH_OPEN && ip.ip.addr==0xc0a80701);
        assert(portal_uri=="http://192.168.7.1/wifi");
        clients=2;h.tick(300000);assert(h.active());clients=0;h.tick(600000);
        assert(!h.active() && mode==WIFI_MODE_STA);
    }
    clean();
    {
        wifi::test_wifi.ap.password="short";
        PanelHotspot h;assert(!h.start(0) && h.error()==ESP_ERR_INVALID_ARG);
        assert(mode_changes==0 && config_calls==0 && portal_starts==0);
    }
    clean();
    {
        netif_present=false;PanelHotspot h;assert(!h.start(0));assert(mode_changes==0);
    }
    clean();
    {
        dhcp_error=91;PanelHotspot h;assert(!h.start(0) && h.error()==91);
        assert(mode==WIFI_MODE_STA && mode_changes==2 && wifi::test_wifi.connected);
        assert(portal_starts==0 && dhcp_starts==0);
    }
    clean();
    {
        mode=WIFI_MODE_APSTA;captive_portal::test_portal.active=true;
        PanelHotspot h;assert(h.start(0));h.tick(300000);
        assert(mode==WIFI_MODE_APSTA && mode_changes==0 && config_calls==0 && portal_ends==0);
    }
    clean();
    {
        PanelHotspot h;assert(h.start(0));wifi::test_wifi.connected=false;h.tick(300000);
        assert(!h.active() && mode==WIFI_MODE_APSTA && portal_ends==0);
    }
    puts("hotspot IDF: compiled AP credentials/config, DHCP/manual IP, portal lifecycle, STA retention and rollback passed");
}
