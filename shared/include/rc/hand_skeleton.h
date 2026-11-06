// Tracked hands in PC games: OpenXR's 26 hand joints -> SteamVR's 31 hand bones.
//
// The conversion follows ALVR (MIT, see THIRD_PARTY_NOTICES.md). SteamVR's bones are relative
// to their parent, with axes that differ per hand; its wrist and "aux" bones are relative to
// the controller's pose, so the hand is given a pose where a held controller would be.
#pragma once

#include <algorithm>
#include <cmath>

#include "rc/protocol.h"

namespace rc {

namespace hand_detail {

struct Q {
    float x = 0, y = 0, z = 0, w = 1;
};
struct V {
    float x = 0, y = 0, z = 0;
};

inline Q Mul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q Conj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }
inline V Rotate(Q q, V v) {
    const V t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + q.y * t.z - q.z * t.y, v.y + q.w * t.y + q.z * t.x - q.x * t.z,
            v.z + q.w * t.z + q.x * t.y - q.y * t.x};
}
inline V Sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Q Rx(float deg) { return {std::sin(deg * 0.00872664626f), 0, 0, std::cos(deg * 0.00872664626f)}; }
inline Q Ry(float deg) { return {0, std::sin(deg * 0.00872664626f), 0, std::cos(deg * 0.00872664626f)}; }
inline Q Rz(float deg) { return {0, 0, std::sin(deg * 0.00872664626f), std::cos(deg * 0.00872664626f)}; }
inline Q GetQ(const Pose& p) { return {p.orientation[0], p.orientation[1], p.orientation[2], p.orientation[3]}; }
inline V GetV(const Pose& p) { return {p.position[0], p.position[1], p.position[2]}; }
inline Pose Make(Q q, V v) { return {{q.x, q.y, q.z, q.w}, {v.x, v.y, v.z}}; }

// The controller's pose relative to the palm; the right hand mirrors the left.
inline Q DeviceInPalmRotation(int hand) { return hand == 0 ? Mul(Ry(-45), Rz(-90)) : Mul(Ry(45), Rz(90)); }
inline V DeviceInPalmPosition(int hand) { return {hand == 0 ? 0.04f : -0.04f, -0.02f, -0.13f}; }

}  // namespace hand_detail

// Where SteamVR should think the controller is, for this palm.
inline Pose HandDevicePose(int hand, const Pose& palm) {
    using namespace hand_detail;
    const Q q = Mul(GetQ(palm), DeviceInPalmRotation(hand));
    const V off = Rotate(q, DeviceInPalmPosition(hand)), p = GetV(palm);
    return Make(q, {p.x + off.x, p.y + off.y, p.z + off.z});
}

// joints: XrHandJointEXT order, all in one space. bones[0] is the root, which SteamVR ignores.
inline void HandJointsToBones(int hand, const Pose joints[kHandJointCount], Pose bones[kHandBoneCount]) {
    using namespace hand_detail;
    const bool left = hand == 0;
    const Q palmQ = GetQ(joints[0]), deviceQ = DeviceInPalmRotation(hand);
    const V palmV = GetV(joints[0]), deviceV = DeviceInPalmPosition(hand);

    // Relative to the controller pose, in SteamVR's axes for the wrist.
    const Q rootFix = Mul(Rx(180), Rz(left ? -90.f : 90.f));
    const auto rootParented = [&](const Pose& j) {
        const V r = Rotate(Conj(deviceQ), Rotate(Conj(palmQ), Sub(GetV(j), palmV)));
        return Make(Mul(Mul(Conj(deviceQ), Mul(Conj(palmQ), GetQ(j))), rootFix),
                    {r.x - deviceV.x, r.y - deviceV.y, r.z - deviceV.z});
    };
    // Relative to the parent joint, in SteamVR's axes for finger bones.
    const auto local = [&](Q parentQ, V parentV, const Pose& j) {
        const Q o = Mul(Conj(parentQ), GetQ(j));
        const V p = Rotate(Conj(parentQ), Sub(GetV(j), parentV));
        return left ? Make({-o.z, -o.y, -o.x, o.w}, {-p.z, -p.y, -p.x}) : Make({o.z, o.y, -o.x, o.w}, {p.z, p.y, -p.x});
    };
    // Tip-pointing helper bones, which SteamVR places straight in controller space.
    const Q auxFix = left ? Mul(Ry(-90), Rx(90)) : Mul(Ry(90), Rx(-90));
    const auto aux = [&](const Pose& j) {
        const Pose r = rootParented(j);
        return Make(Mul(GetQ(r), auxFix), GetV(r));
    };

    bones[0] = Make({}, {});
    bones[1] = rootParented(joints[1]);
    const Q wristQ = Mul(GetQ(joints[1]), Mul(Ry(-90), Rx(90)));
    const V wristV = GetV(joints[1]);
    // Thumb has four joints (2-5), the other fingers five (6-10, 11-15, 16-20, 21-25).
    for (int i = 2; i < kHandJointCount; i++) {
        const bool first = i == 2 || i == 6 || i == 11 || i == 16 || i == 21;
        bones[i] = first ? local(wristQ, wristV, joints[i]) : local(GetQ(joints[i - 1]), GetV(joints[i - 1]), joints[i]);
    }
    const int auxJoints[5] = {4, 9, 14, 19, 24};
    for (int i = 0; i < 5; i++) bones[26 + i] = aux(joints[auxJoints[i]]);
}

// How closed a finger is, 0 straight to 1 a fist. finger: 1 index, 2 middle, 3 ring, 4 little.
inline float HandFingerCurl(const Pose joints[kHandJointCount], int finger) {
    using namespace hand_detail;
    const int base = 1 + finger * 5;  // the metacarpal
    float bent = 0;
    for (int i = base; i < base + 3; i++) {
        const Q d = Mul(Conj(GetQ(joints[i])), GetQ(joints[i + 1]));
        bent += 2 * std::atan2(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), std::fabs(d.w));
    }
    constexpr float kFist = 200.f * 3.14159265f / 180.f;
    return std::clamp(bent / kFist, 0.f, 1.f);
}

// The grip button for a tracked hand: closing the three lower fingers.
inline float HandGrip(const Pose joints[kHandJointCount]) {
    const float curl = (HandFingerCurl(joints, 2) + HandFingerCurl(joints, 3) + HandFingerCurl(joints, 4)) / 3;
    return std::clamp((curl - 0.45f) / 0.4f, 0.f, 1.f);
}

}  // namespace rc
