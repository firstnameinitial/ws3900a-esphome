// SPDX-License-Identifier: GPL-3.0-only
#include "panel_hotspot.h"
#include <cassert>
#include <cstdio>
using namespace esphome::ecowitt_rf;

// Models the IDF mode, station list and portal APIs. All mutations assert that
// the existing STA interface survives. No API for changing STA credentials or
// stopping the driver is provided, so the controller cannot use one.
struct FakeIdf {
    uint8_t radio_mode = 1;
    bool connected = true, portal = false, ready = true;
    bool mode_ok = true, configure_ok = true, enable_ok = true, disable_ok = true, count_ok = true;
    unsigned clients = 0, polls = 0, mode_reads = 0, prepares = 0;
    unsigned enables = 0, disables = 0, configurations = 0, portal_starts = 0, portal_ends = 0;
    int last_error = 0;
    bool mode(uint8_t &out) { ++mode_reads; out = radio_mode; last_error = mode_ok ? 0 : 11; return mode_ok; }
    bool prepare() { ++prepares; last_error = ready ? 0 : 12; return ready; }
    bool enable_ap() {
        ++enables; assert(radio_mode & 1); last_error = enable_ok ? 0 : 13;
        if (enable_ok) radio_mode = 3;
        return enable_ok;
    }
    bool configure() { ++configurations; assert(radio_mode == 3); last_error = configure_ok ? 0 : 14; return configure_ok; }
    bool disable_ap() {
        ++disables; assert(radio_mode & 1); last_error = disable_ok ? 0 : 15;
        if (disable_ok) radio_mode = 1;
        return disable_ok;
    }
    bool station_count(unsigned &out) { ++polls; out = clients; last_error = count_ok ? 0 : 16; return count_ok; }
    bool sta_connected() const { return connected; }
    bool portal_active() const { return portal; }
    bool start_portal() { ++portal_starts; portal = true; return true; }
    void end_portal() { ++portal_ends; portal = false; }
    int error() const { return last_error; }
};
using Hotspot = PanelHotspotControl<FakeIdf>;
int main() {
    {
        Hotspot h;
        assert(h.start(100)); assert(h.active() && h.owned() && h.adapter.radio_mode == 3);
        assert(h.adapter.connected && h.adapter.portal && h.remaining_ms(100) == 300000);
        for (unsigned t = 101; t < 1100; ++t) h.tick(t);
        assert(h.adapter.polls == 0); // no busy polling
        h.tick(1100); assert(h.adapter.polls == 1);
        h.tick(299100); assert(h.active());
        h.tick(300100); assert(!h.active() && h.adapter.radio_mode == 1);
        assert(h.adapter.connected && !h.adapter.portal && h.adapter.portal_ends == 1);
        assert(h.adapter.disables == 1 && h.adapter.configurations == 1);
    }
    {
        Hotspot h; assert(h.start(0));
        h.adapter.clients = 1; h.tick(300000);
        assert(h.active() && h.remaining_ms(300000) == 300000);
        h.adapter.clients = 0; h.tick(599000); assert(h.active());
        h.tick(600000); assert(!h.active()); // five minutes after last connected observation
    }
    {
        Hotspot h; assert(h.start(0)); assert(h.start(299999));
        assert(h.owned() && h.adapter.configurations == 1 && h.adapter.portal_starts == 1);
        h.tick(300999); assert(h.active());
        h.tick(599999); assert(!h.active() && h.adapter.disables == 1);
    }
    {
        Hotspot h; h.adapter.radio_mode = 3; h.adapter.portal = true;
        assert(h.start(0) && !h.owned()); h.tick(300000);
        assert(!h.active() && h.adapter.radio_mode == 3 && h.adapter.portal);
        assert(h.adapter.prepares == 0 && h.adapter.configurations == 0 && h.adapter.disables == 0);
        assert(h.adapter.portal_starts == 0 && h.adapter.portal_ends == 0);
    }
    {
        Hotspot h; h.adapter.portal = true; assert(h.start(0)); h.tick(300000);
        assert(h.adapter.portal && h.adapter.portal_ends == 0); // pre-existing service not ours
    }
    {
        Hotspot h; h.adapter.ready = false;
        assert(!h.start(0) && h.error() == 12 && !h.active());
        assert(h.adapter.enables == 0 && h.adapter.radio_mode == 1);
        h.adapter.ready = true; h.adapter.configure_ok = false;
        assert(!h.start(1) && h.error() == 14 && !h.active());
        assert(h.adapter.radio_mode == 1 && h.adapter.connected && h.adapter.disables == 1);
        assert(h.adapter.portal_starts == 0);
    }
    {
        Hotspot h; h.adapter.configure_ok = h.adapter.disable_ok = false;
        assert(!h.start(0) && h.active() && h.owned() && h.error() == 15);
        h.tick(999); assert(h.adapter.disables == 1);
        h.adapter.disable_ok = true; h.tick(1000);
        assert(!h.active() && h.adapter.radio_mode == 1 && h.adapter.connected);
    }
    {
        Hotspot h; h.adapter.configure_ok = h.adapter.disable_ok = false;
        assert(!h.start(0)); assert(!h.start(1)); // failed rollback cannot become a successful AP request
        h.adapter.configure_ok = h.adapter.disable_ok = true;
        assert(h.start(2) && h.owned() && h.adapter.configurations == 2);
        h.tick(300002); assert(!h.active());
    }
    {
        Hotspot h; assert(h.start(0)); h.adapter.disable_ok = false;
        h.tick(300000); assert(h.active() && h.error() == 15);
        for (unsigned t = 300001; t < 301000; ++t) h.tick(t);
        assert(h.adapter.disables == 1);
        h.adapter.disable_ok = true; h.tick(301000);
        assert(!h.active() && h.adapter.radio_mode == 1 && h.adapter.portal_ends == 1);
    }
    {
        Hotspot h; assert(h.start(0)); h.adapter.connected = false; h.tick(300000);
        assert(!h.active() && h.adapter.radio_mode == 3 && h.adapter.portal);
        assert(h.adapter.disables == 0 && h.adapter.portal_ends == 0); // fallback handover
    }
    {
        Hotspot h; assert(h.start(0)); h.adapter.count_ok = false; h.tick(300000);
        assert(!h.active() && h.error() == 16 && h.adapter.radio_mode == 3);
        assert(h.adapter.disables == 0 && h.adapter.portal); // unknown clients never evicted
    }
    {
        Hotspot h; assert(h.start(0));
        h.adapter.radio_mode = 1; h.adapter.portal = false; // normal ESPHome reconnect cleanup
        h.tick(1000); assert(!h.active() && h.adapter.enables == 1 && h.adapter.disables == 0);
        h.tick(301000); assert(h.adapter.enables == 1); // no restart fight
    }
    {
        Hotspot h; const uint32_t initial = 0xfffff000;
        assert(h.start(initial)); h.tick(initial + 299000); assert(h.active());
        h.tick(initial + 300000); assert(!h.active() && h.adapter.connected);
    }
    {
        Hotspot h; h.adapter.radio_mode = 0;
        assert(!h.start(0) && h.error() == -1 && h.adapter.enables == 0);
        h.adapter.radio_mode = 1; h.adapter.mode_ok = false;
        assert(!h.start(1) && h.error() == 11 && h.adapter.prepares == 0);
        h.adapter.mode_ok = true; h.adapter.enable_ok = false;
        assert(!h.start(2) && h.error() == 13 && !h.active() && h.adapter.radio_mode == 1);
    }
    puts("hotspot: STA retention, ownership, bounded polling/retry, clients, expiry, failures and millis wrap passed");
}
