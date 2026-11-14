// Backward-compatible optional controller aim data in tracking datagrams.
#pragma once
#include <cmath>
#include <cstring>
#include <string_view>
#include "rc/protocol.h"

namespace rc {

inline bool IsControllerProfilePath(std::string_view value) {
    if (value.empty()) return true;
    if (value.size() >= 256 || value.find("/interaction_profiles/") != 0) return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' || c == '.')) return false;
    return true;
}

inline bool NormalizeTrackingPose(Pose& pose) {
    double norm = 0;
    for (double value : pose.orientation) {
        if (!std::isfinite(value)) return false;
        norm += value * value;
    }
    for (double value : pose.position)
        if (!std::isfinite(value)) return false;
    if (norm < 0.25 || norm > 4) return false;
    const double length = std::sqrt(norm);
    for (float& value : pose.orientation) value = float(value / length);
    return true;
}

inline bool ReadControllerAimExtension(const uint8_t* data, size_t size, ControllerAimPayload& aim) {
    aim = {};
    constexpr size_t offset = sizeof(TrackingPayload) + sizeof(GazePayload);
    if (!data || size < offset + sizeof(ControllerAimPayload)) return false;
    std::memcpy(&aim, data + offset, sizeof(aim));
    aim.validMask &= 3;
    for (int hand = 0; hand < 2; ++hand)
        if ((aim.validMask & (1u << hand)) && !NormalizeTrackingPose(aim.poses[hand])) aim.validMask &= ~(1u << hand);
    return true;
}

}  // namespace rc
