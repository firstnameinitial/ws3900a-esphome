// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>

namespace esphome { namespace ecowitt_rf {
// AP lifetime is measured monotonically. A connected client gets a fresh five
// minutes; a borrowed fallback AP is never closed by this policy.
struct HotspotWindow {
    static constexpr uint32_t WINDOW_MS = 300000;
    bool active = false, owned = false;
    uint32_t last_activity = 0;
    void open(uint32_t now, bool own) { active = true; owned = own; last_activity = now; }
    void touch(uint32_t now) { last_activity = now; }
    void clear() { active = owned = false; }
    uint32_t remaining(uint32_t now) const {
        const uint32_t elapsed = now - last_activity;
        return !active || elapsed >= WINDOW_MS ? 0 : WINDOW_MS - elapsed;
    }
};

// Adapter operations never stop/restart the WiFi driver or alter STA settings.
// mode() returns bit 0 = STA, bit 1 = AP. Native calls are bounded IDF APIs;
// tick performs at most one station-list query per second, with no wait loops.
template<class Adapter> class PanelHotspotControl {
 public:
    Adapter adapter;
    bool start(uint32_t now) {
        // Complete a failed rollback before accepting another request. Otherwise
        // an unconfigured, partially started AP could be mistaken for fallback.
        if (pending_stop_) {
            stop_();
            if (pending_stop_) return false;
        }
        uint8_t mode;
        if (!adapter.mode(mode)) return fail_();
        if (mode & 2) {
            // Repeated presses extend our window; pre-existing fallback is borrowed.
            window_.open(now, window_.active && window_.owned);
            last_poll_ = now; error_ = 0; return true;
        }
        window_.clear(); own_portal_ = pending_stop_ = false;
        if (!(mode & 1)) { error_ = -1; return false; } // never enable a disabled radio
        if (!adapter.prepare()) return fail_();
        if (!adapter.enable_ap()) return fail_();
        window_.open(now, true); last_poll_ = now;
        if (!adapter.configure()) {
            error_ = adapter.error();
            // Restore the original STA-only mode even after a partial start.
            pending_stop_ = true;
            stop_();
            return false;
        }
        own_portal_ = !adapter.portal_active() && adapter.start_portal();
        error_ = 0;
        return true;
    }
    void tick(uint32_t now) {
        if (!window_.active || now - last_poll_ < 1000) return;
        last_poll_ = now;
        uint8_t mode;
        if (!adapter.mode(mode)) { error_ = adapter.error(); return; }
        if (!(mode & 2)) {
            // ESPHome may end AP on a later STA connection. Do not fight it.
            end_own_portal_(); window_.clear(); pending_stop_ = false; return;
        }
        if (pending_stop_) { stop_(); return; }
        unsigned clients = 0;
        const bool count_known = adapter.station_count(clients);
        if (count_known && clients) window_.touch(now);
        if (window_.remaining(now)) return;
        if (!window_.owned) { window_.clear(); return; }
        if (!adapter.sta_connected() || !count_known) {
            // Hand recovery back to ESPHome when connectivity is uncertain.
            // Its normal successful-STA transition will end portal/AP.
            error_ = count_known ? 0 : adapter.error();
            own_portal_ = false; window_.clear(); return;
        }
        pending_stop_ = true;
        stop_();
    }
    // This is a requested/owned AP window, not ESPHome's asynchronous event flag.
    bool active() const { return window_.active; }
    bool owned() const { return window_.owned; }
    uint32_t remaining_ms(uint32_t now) const { return window_.remaining(now); }
    int error() const { return error_; }
 private:
    HotspotWindow window_;
    uint32_t last_poll_ = 0;
    int error_ = 0;
    bool own_portal_ = false, pending_stop_ = false;
    bool fail_() { error_ = adapter.error(); return false; }
    void end_own_portal_() {
        if (own_portal_ && adapter.portal_active()) adapter.end_portal();
        own_portal_ = false;
    }
    void stop_() {
        if (!adapter.disable_ap()) { error_ = adapter.error(); return; }
        end_own_portal_(); window_.clear(); pending_stop_ = false;
    }
};
}}  // namespace esphome::ecowitt_rf

#if defined(USE_ESP32) && defined(USE_WIFI_AP)
#include <cstring>
#include <cstdio>
#include <esp_wifi.h>
#include <esp_netif.h>
#include <esp_idf_version.h>
#include "dhcpserver/dhcpserver.h"
#include "esphome/components/wifi/wifi_component.h"
#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#endif
namespace esphome { namespace ecowitt_rf {
class IdfPanelHotspotAdapter {
 public:
    int error() const { return error_; }
    bool mode(uint8_t &out) {
        wifi_mode_t mode;
        if (!ok_(esp_wifi_get_mode(&mode))) return false;
        out = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA ? 1 : 0) |
              (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA ? 2 : 0);
        return true;
    }
    bool prepare() {
        auto *wifi_component = wifi::global_wifi_component;
        if (!wifi_component || !wifi_component->has_ap()) { error_ = ESP_ERR_INVALID_STATE; return false; }
        auto ap = wifi_component->get_ap();
        const auto ssid = ap.get_ssid(), password = ap.get_password();
        if (ssid.empty() || ssid.size() > sizeof(config_.ap.ssid) ||
            (!password.empty() && (password.size() < 8 || password.size() > sizeof(config_.ap.password)))) {
            error_ = ESP_ERR_INVALID_ARG; return false;
        }
        netif_ = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (!netif_) { error_ = ESP_ERR_INVALID_STATE; return false; }
        config_ = {};
        std::memcpy(config_.ap.ssid, ssid.c_str(), ssid.size());
        config_.ap.ssid_len = ssid.size();
        std::memcpy(config_.ap.password, password.c_str(), password.size());
        config_.ap.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        config_.ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
        config_.ap.ssid_hidden = ap.get_hidden();
        config_.ap.max_connection = 5;
        config_.ap.beacon_interval = 100;
        config_.ap.channel = ap.has_channel() ? ap.get_channel() : 1;
        // APSTA follows the associated STA channel. Never request a channel switch.
        uint8_t sta_channel; wifi_second_chan_t secondary;
        if (esp_wifi_get_channel(&sta_channel, &secondary) == ESP_OK && sta_channel)
            config_.ap.channel = sta_channel;
        info_.ip = network::IPAddress(192,168,4,1);
        info_.gw = info_.ip;
        info_.netmask = network::IPAddress(255,255,255,0);
#ifdef USE_WIFI_MANUAL_IP
        if (ap.get_manual_ip().has_value()) {
            const auto manual = *ap.get_manual_ip();
            info_.ip = manual.static_ip; info_.gw = manual.gateway; info_.netmask = manual.subnet;
        }
#endif
        error_ = 0; return true;
    }
    bool enable_ap() { return ok_(esp_wifi_set_mode(WIFI_MODE_APSTA)); }
    bool disable_ap() { return ok_(esp_wifi_set_mode(WIFI_MODE_STA)); }
    bool configure() {
        if (!ok_(esp_wifi_set_config(WIFI_IF_AP, &config_))) return false;
        esp_err_t err = esp_netif_dhcps_stop(netif_);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return ok_(err);
        if (!ok_(esp_netif_set_ip_info(netif_, &info_))) return false;
        dhcps_lease_t lease{};
        lease.enable = true;
        network::IPAddress address(&info_.ip); address += 99; lease.start_ip = address;
        address += 10; lease.end_ip = address;
        if (!ok_(esp_netif_dhcps_option(netif_, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                                      &lease, sizeof(lease)))) return false;
#if defined(USE_CAPTIVE_PORTAL) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,4,0)
        // Persistent storage: IDF retains this pointer. /wifi avoids the custom
        // dashboard's / route and is handled by ESPHome's normal portal page.
        static char portal_uri[32];
        std::memcpy(portal_uri, "http://", 7);
        network::IPAddress(&info_.ip).str_to(portal_uri + 7);
        std::strcat(portal_uri, "/wifi");
        // Discovery is optional; DNS/HTTP portal still works without DHCP 114.
        esp_netif_dhcps_option(netif_, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                              portal_uri, std::strlen(portal_uri));
#endif
        err = esp_netif_dhcps_start(netif_);
        return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? true : ok_(err);
    }
    bool station_count(unsigned &count) {
        wifi_sta_list_t list{};
        if (!ok_(esp_wifi_ap_get_sta_list(&list))) return false;
        count = list.num; return true;
    }
    bool sta_connected() const {
        return wifi::global_wifi_component && wifi::global_wifi_component->is_connected();
    }
    bool portal_active() const {
#ifdef USE_CAPTIVE_PORTAL
        return captive_portal::global_captive_portal && captive_portal::global_captive_portal->is_active();
#else
        return false;
#endif
    }
    bool start_portal() {
#ifdef USE_CAPTIVE_PORTAL
        if (captive_portal::global_captive_portal) {
            captive_portal::global_captive_portal->start(); return true;
        }
#endif
        return false;
    }
    void end_portal() {
#ifdef USE_CAPTIVE_PORTAL
        if (captive_portal::global_captive_portal) captive_portal::global_captive_portal->end();
#endif
    }
 private:
    int error_ = 0;
    wifi_config_t config_{};
    esp_netif_ip_info_t info_{};
    esp_netif_t *netif_ = nullptr;
    bool ok_(esp_err_t err) { error_ = err; return err == ESP_OK; }
};
using PanelHotspot = PanelHotspotControl<IdfPanelHotspotAdapter>;
}}  // namespace esphome::ecowitt_rf
#endif
