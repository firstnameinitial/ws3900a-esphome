// SPDX-License-Identifier: GPL-3.0-only
#include "application_reset.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
using namespace esphome::ecowitt_rf;
int main() {
    std::map<uint32_t,std::string> persisted{{1,"wifi"},{2,"encrypted-api"},{3,"ota"}};
    for (auto k:APPLICATION_PREF_KEYS) persisted[k]="changed";
    bool flushed=false;
    assert(reset_application_preferences([&](uint32_t k){persisted[k]="default";return true;},[&]{flushed=true;return true;}));
    assert(flushed && persisted[1]=="wifi" && persisted[2]=="encrypted-api" && persisted[3]=="ota");
    for (auto k:APPLICATION_PREF_KEYS) assert(persisted[k]=="default");
    unsigned attempted=0;
    assert(!reset_application_preferences([&](uint32_t k){++attempted;return k!=0xE617;},[]{return true;}));
    assert(attempted==sizeof(APPLICATION_PREF_KEYS)/sizeof(APPLICATION_PREF_KEYS[0]));
    assert(!reset_application_preferences([](uint32_t){return true;},[]{return false;}));
    puts("Application reset: app preference scope, transport identity retention and failed-save reboot guard passed");
}
