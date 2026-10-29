// SPDX-License-Identifier: GPL-3.0-only
#include "sensor_overrides.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
using namespace esphome::ecowitt_rf;
int main() {
    const uint32_t attic = (static_cast<uint32_t>(Family::WH31) << 24) | 0xd3;
    const uint32_t other = (static_cast<uint32_t>(Family::WH31) << 24) | 0xe4;
    SensorOverrides s; s.seal(); assert(s.valid() && s.count == 0);
    assert(s.channel_for(attic,1) == 1 && !*s.name_for(attic));
    assert(s.update(attic,SensorChannelMode::KEEP,0,"Attic"));
    assert(s.channel_for(attic,1)==1 && !std::strcmp(s.name_for(attic),"Attic"));
    assert(s.update(attic,SensorChannelMode::SET,2,nullptr)); s.seal();
    // Exact NVS-byte roundtrip: simulated boot retains explicit settings, not RF state.
    unsigned char bytes[sizeof(s)]; std::memcpy(bytes,&s,sizeof(s));
    SensorOverrides boot; std::memcpy(&boot,bytes,sizeof(boot));
    assert(boot.valid() && boot.channel_for(attic,1)==2 && !std::strcmp(boot.name_for(attic),"Attic"));
    assert(boot.channel_for(other,1)==1 && !*boot.name_for(other));
    assert(boot.update(attic,SensorChannelMode::AUTO,0,nullptr)); boot.seal();
    assert(boot.valid() && boot.channel_for(attic,3)==3 && !std::strcmp(boot.name_for(attic),"Attic"));
    assert(boot.update(attic,SensorChannelMode::KEEP,0,"")); boot.seal();
    assert(boot.valid() && boot.count==0);
    const auto empty=boot;
    assert(!boot.update(attic,SensorChannelMode::SET,0,"Bad"));
    assert(!boot.update(attic,SensorChannelMode::SET,9,"Bad"));
    assert(!boot.update(attic,SensorChannelMode::KEEP,0,"1234567890123456"));
    assert(!boot.update(0,SensorChannelMode::KEEP,0,"Bad"));
    assert(!std::memcmp(&boot,&empty,sizeof(boot)));
    for (uint32_t i=1;i<=SensorOverrides::CAPACITY;++i)
        assert(boot.update((static_cast<uint32_t>(Family::WH31)<<24)|i,SensorChannelMode::SET,1,"Named"));
    boot.seal();assert(boot.valid()); const auto full=boot;
    assert(!boot.update(other,SensorChannelMode::SET,2,"Full"));
    assert(!std::memcmp(&boot,&full,sizeof(boot)));
    assert(boot.update((static_cast<uint32_t>(Family::WH31)<<24)|1,SensorChannelMode::AUTO,0,""));
    assert(boot.update(other,SensorChannelMode::SET,8,"Reused"));boot.seal();assert(boot.valid());
    for (size_t i=0;i<sizeof(boot);++i) {
        auto corrupted=boot; reinterpret_cast<uint8_t *>(&corrupted)[i]^=1;
        assert(!corrupted.valid());
    }
    auto bad=boot;bad.version=2;bad.seal();assert(!bad.valid());
    bad=boot;bad.count=25;bad.seal();assert(!bad.valid());
    bad=boot;bad.entries[1].key=bad.entries[0].key;bad.seal();assert(!bad.valid());
    bad=boot;bad.entries[0].flags=4;bad.seal();assert(!bad.valid());
    bad=boot;std::memset(bad.entries[0].name,'X',16);bad.seal();assert(!bad.valid());
    // Exercise the actual commit path across flash/cache failures, retries and no-ops.
    SensorOverrides current;current.seal();const auto original=current;
    SensorOverrides next=current;assert(next.update(attic,SensorChannelMode::SET,2,"Attic"));
    SensorOverrides cache=current,disk=current;int saves=0,flushes=0;
    assert(!commit_sensor_overrides(current,next,[&](const SensorOverrides &v){cache=v;++saves;return true;},
        [&](){++flushes;if(flushes==1)return false;disk=cache;return true;}));
    assert(!std::memcmp(&current,&original,sizeof(current)) && !std::memcmp(&disk,&original,sizeof(disk)));
    assert(saves==2 && flushes==2);
    saves=flushes=0;
    assert(!commit_sensor_overrides(current,next,[&](const SensorOverrides &v){cache=v;return ++saves!=1;},
        [&](){++flushes;disk=cache;return true;}));
    assert(!std::memcmp(&current,&original,sizeof(current)) && !std::memcmp(&disk,&original,sizeof(disk)));
    assert(saves==2 && flushes==1);
    assert(commit_sensor_overrides(current,next,[&](const SensorOverrides &v){cache=v;return true;},
        [&](){disk=cache;return true;}));
    assert(current.valid() && disk.valid() && disk.channel_for(attic,1)==2);
    saves=flushes=0;next=current;
    assert(commit_sensor_overrides(current,next,[&](const SensorOverrides &){++saves;return true;},
        [&](){++flushes;return true;}));
    assert(!saves && !flushes);
    uint32_t id=0;assert(parse_sensor_override_id("0000D3",id)&&id==0xd3);
    assert(parse_sensor_override_id("0xabcdef",id)&&id==0xabcdef);
    for (auto text : {"","-1","1000000","0000D3oops","0x","zz"}) assert(!parse_sensor_override_id(text,id));
    assert(!parse_sensor_override_id(nullptr,id));
    puts("Sensor overrides: byte persistence, physical fallback/auto, identity isolation, bounds/full/clear and corrupt/unknown checkpoints passed");
}
