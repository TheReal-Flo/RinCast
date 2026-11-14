#pragma once
#include <cmath>
#include <cstring>
#include "rc/protocol.h"

namespace rc {
// PICO controller power is a coarse 1..5 level, not an exact percentage.
inline BatteryState PicoBatteryLevel(float level) {
    if (!std::isfinite(level) || level < 1 || level > 5 || level != std::floor(level)) return {};
    return {level / 5.f, kBatteryValid};
}
inline BatteryState SanitizeBattery(BatteryState state) {
    if (!(state.flags & kBatteryValid) || !std::isfinite(state.level) || state.level < 0 || state.level > 1) return {};
    state.flags &= kBatteryValid | kBatteryChargingKnown | kBatteryCharging;
    if (!(state.flags & kBatteryChargingKnown)) state.flags &= ~kBatteryCharging;
    return state;
}
inline bool ReadBatteryExtension(const uint8_t* data, size_t size, BatteryPayload& battery) {
    battery = {};
    constexpr size_t offset = sizeof(TrackingPayload) + sizeof(GazePayload);
    if (!data || size < offset + sizeof(battery)) return false;
    std::memcpy(&battery, data + offset, sizeof(battery));
    if (battery.magic != kBatteryMagic) { battery = {}; return false; }
    for (auto& device : battery.devices) device = SanitizeBattery(device);
    return true;
}
}  // namespace rc
