// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "fineoffset.h"

namespace esphome { namespace ecowitt_rf {
enum class SensorChannelMode : uint8_t { KEEP, SET, AUTO };
inline bool sensor_override_channel_valid(uint8_t family, int channel) {
    return family == static_cast<uint8_t>(Family::WH31) ? channel >= 1 && channel <= 8
                                                       : channel >= 0 && channel <= 16;
}
inline bool sensor_override_identity_valid(uint32_t key) {
    const uint8_t family = key >> 24;
    return family > 0 && family <= static_cast<uint8_t>(Family::WH35);
}
inline bool parse_sensor_override_id(const char *text, uint32_t &id) {
    if (!text) return false;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
    id = 0;
    size_t count = 0;
    for (; *text; ++text) {
        const char c = *text;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                       : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0 || ++count > 6) return false;
        id = (id << 4) | static_cast<uint32_t>(d);
    }
    return count > 0;
}
// Separate bounded checkpoint. No discovered values, enable flags or RF freshness
// are persisted here. Legacy preferences remain byte-for-byte compatible.
struct SensorOverrides {
    static constexpr uint32_t MAGIC = 0x534f5631;
    static constexpr uint8_t CAPACITY = 24, CHANNEL = 1, NAME = 2;
    struct Entry {
        uint32_t key = 0;
        uint8_t channel = 0, flags = 0;
        char name[16]{};
        uint8_t reserved[2]{};
    };
    uint32_t magic = MAGIC;
    uint16_t version = 1;
    uint8_t count = 0, reserved = 0;
    Entry entries[CAPACITY]{};
    uint32_t checksum = 0;
    uint32_t digest() const {
        const auto *bytes = reinterpret_cast<const uint8_t *>(this);
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < offsetof(SensorOverrides, checksum); ++i) hash = (hash ^ bytes[i]) * 16777619u;
        return hash;
    }
    void seal() { checksum = digest(); }
    bool valid() const {
        if (magic != MAGIC || version != 1 || count > CAPACITY || reserved || checksum != digest()) return false;
        for (uint8_t i = 0; i < CAPACITY; ++i) {
            const auto &e = entries[i];
            if (i >= count) {
                if (e.key || e.channel || e.flags || e.reserved[0] || e.reserved[1]) return false;
                for (char c : e.name) if (c) return false;
                continue;
            }
            if (!sensor_override_identity_valid(e.key) || !e.flags || (e.flags & ~(CHANNEL | NAME)) ||
                e.reserved[0] || e.reserved[1] || !std::memchr(e.name, '\0', sizeof(e.name))) return false;
            if ((e.flags & CHANNEL) ? !sensor_override_channel_valid(e.key >> 24, e.channel) : e.channel != 0) return false;
            if ((e.flags & NAME) ? e.name[0] == '\0' : e.name[0] != '\0') return false;
            for (uint8_t j = 0; j < i; ++j) if (entries[j].key == e.key) return false;
        }
        return true;
    }
    const Entry *find(uint32_t key) const {
        for (uint8_t i = 0; i < count; ++i) if (entries[i].key == key) return &entries[i];
        return nullptr;
    }
    uint8_t channel_for(uint32_t key, uint8_t physical) const {
        const Entry *e = find(key);
        return e && (e->flags & CHANNEL) ? e->channel : physical;
    }
    const char *name_for(uint32_t key) const {
        const Entry *e = find(key);
        return e && (e->flags & NAME) ? e->name : "";
    }
    // nullptr name preserves it; empty name clears it. AUTO clears only channel.
    // Invalid/full updates leave the checkpoint intact. Call seal() before saving.
    bool update(uint32_t key, SensorChannelMode mode, uint8_t channel, const char *name) {
        if (!sensor_override_identity_valid(key) ||
            (mode != SensorChannelMode::KEEP && mode != SensorChannelMode::SET && mode != SensorChannelMode::AUTO) ||
            (mode == SensorChannelMode::SET && !sensor_override_channel_valid(key >> 24, channel)) ||
            (name && std::strlen(name) >= sizeof(Entry::name))) return false;
        uint8_t index = count;
        for (uint8_t i = 0; i < count; ++i) if (entries[i].key == key) { index = i; break; }
        Entry next = index < count ? entries[index] : Entry{};
        next.key = key;
        if (mode == SensorChannelMode::SET) { next.channel = channel; next.flags |= CHANNEL; }
        else if (mode == SensorChannelMode::AUTO) { next.channel = 0; next.flags &= ~CHANNEL; }
        if (name) {
            std::memset(next.name, 0, sizeof(next.name));
            std::memcpy(next.name, name, std::strlen(name));
            if (*name) next.flags |= NAME; else next.flags &= ~NAME;
        }
        if (!next.flags) {
            if (index < count) {
                for (uint8_t i = index + 1; i < count; ++i) entries[i - 1] = entries[i];
                entries[--count] = Entry{};
            }
            return true;
        }
        if (index == count) {
            if (count == CAPACITY) return false;
            ++count;
        }
        entries[index] = next;
        return true;
    }
};
// Only adopt the new runtime settings after the preference is committed. Failed
// saves restore pending cache data too, so a later unrelated sync cannot save it.
template<class Save, class Flush>
inline bool commit_sensor_overrides(SensorOverrides &current, SensorOverrides &next,
                                    Save save, Flush flush) {
    next.seal();
    if (!std::memcmp(&current, &next, sizeof(next))) return true;
    if (save(next) && flush()) { current = next; return true; }
    save(current);
    flush();
    return false;
}
static_assert(sizeof(SensorOverrides::Entry) == 24, "Stable sensor override record layout");
static_assert(offsetof(SensorOverrides, checksum) == 584 && sizeof(SensorOverrides) == 588,
              "Stable sensor override preference layout");
}}  // namespace esphome::ecowitt_rf
