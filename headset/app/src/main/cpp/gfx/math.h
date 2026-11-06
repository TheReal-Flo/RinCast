// Minimal 3D math for the headset renderer (right-handed, Y up, -Z forward,
// column-major matrices to match OpenGL).
#pragma once

#include <algorithm>
#include <cmath>

namespace rv {

constexpr float kPi = 3.14159265358979f;

struct Vec2 {
    float x = 0, y = 0;
    Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s) const { return {x * s, y * s}; }
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) {
        x += o.x, y += o.y, z += o.z;
        return *this;
    }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3& v) {
    const float l = Length(v);
    return l > 1e-8f ? v * (1.f / l) : Vec3{0, 0, -1};
}
inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float Clamp01(float v) { return std::clamp(v, 0.f, 1.f); }
inline float SmoothStep(float e0, float e1, float x) {
    const float t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}
// Frame-rate independent exponential approach.
inline float Damp(float current, float target, float lambda, float dt) {
    return Lerp(current, target, 1.f - std::exp(-lambda * dt));
}
inline Vec3 Damp(const Vec3& current, const Vec3& target, float lambda, float dt) {
    return Lerp(current, target, 1.f - std::exp(-lambda * dt));
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    static Quat AxisAngle(const Vec3& axis, float angle) {
        const Vec3 a = Normalize(axis) * std::sin(angle / 2);
        return {a.x, a.y, a.z, std::cos(angle / 2)};
    }
    static Quat Yaw(float angle) { return AxisAngle({0, 1, 0}, angle); }
    Quat operator*(const Quat& b) const {
        return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
    }
    Quat Conjugate() const { return {-x, -y, -z, w}; }
    Vec3 Rotate(const Vec3& v) const {
        const Vec3 u{x, y, z};
        const Vec3 t = Cross(u, v) * 2.f;
        return v + t * w + Cross(u, t);
    }
};

inline Quat Normalize(const Quat& q) {
    const float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 0 ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
inline Quat Slerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995f)
        return Normalize(Quat{Lerp(a.x, b.x, t), Lerp(a.y, b.y, t), Lerp(a.z, b.z, t), Lerp(a.w, b.w, t)});
    const float th = std::acos(d), s = std::sin(th);
    const float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}
// Rotation that looks along `forward` (-Z) with `up`.
inline Quat LookRotation(const Vec3& forward, const Vec3& up = {0, 1, 0}) {
    const Vec3 f = Normalize(forward);
    const Vec3 r = Normalize(Cross(up, -f));
    const Vec3 u = Cross(-f, r);
    // Rotation matrix columns: r, u, -f
    const float m00 = r.x, m01 = u.x, m02 = -f.x;
    const float m10 = r.y, m11 = u.y, m12 = -f.y;
    const float m20 = r.z, m21 = u.z, m22 = -f.z;
    const float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1 + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1 + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1 + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return Normalize(q);
}

struct Pose {
    Quat orientation;
    Vec3 position;
    Vec3 Transform(const Vec3& p) const { return orientation.Rotate(p) + position; }
    Vec3 Forward() const { return orientation.Rotate({0, 0, -1}); }
    Vec3 Up() const { return orientation.Rotate({0, 1, 0}); }
    Vec3 Right() const { return orientation.Rotate({1, 0, 0}); }
    Pose operator*(const Pose& local) const {
        return {orientation * local.orientation, Transform(local.position)};
    }
    Pose Inverse() const {
        const Quat inv = orientation.Conjugate();
        return {inv, inv.Rotate(-position)};
    }
};

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column-major

    static Mat4 Identity() { return {}; }
    float& operator()(int row, int col) { return m[col * 4 + row]; }
    float operator()(int row, int col) const { return m[col * 4 + row]; }

    Mat4 operator*(const Mat4& b) const {
        Mat4 r;
        for (int c = 0; c < 4; c++)
            for (int rr = 0; rr < 4; rr++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += (*this)(rr, k) * b(k, c);
                r(rr, c) = s;
            }
        return r;
    }
    Vec3 TransformPoint(const Vec3& p) const {
        const float x = m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12];
        const float y = m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13];
        const float z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
        return {x, y, z};
    }

    static Mat4 FromPose(const Pose& p, const Vec3& scale = {1, 1, 1}) {
        const Quat& q = p.orientation;
        Mat4 r;
        const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
        const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
        const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
        r(0, 0) = (1 - 2 * (yy + zz)) * scale.x;
        r(1, 0) = (2 * (xy + wz)) * scale.x;
        r(2, 0) = (2 * (xz - wy)) * scale.x;
        r(0, 1) = (2 * (xy - wz)) * scale.y;
        r(1, 1) = (1 - 2 * (xx + zz)) * scale.y;
        r(2, 1) = (2 * (yz + wx)) * scale.y;
        r(0, 2) = (2 * (xz + wy)) * scale.z;
        r(1, 2) = (2 * (yz - wx)) * scale.z;
        r(2, 2) = (1 - 2 * (xx + yy)) * scale.z;
        r(0, 3) = p.position.x;
        r(1, 3) = p.position.y;
        r(2, 3) = p.position.z;
        return r;
    }

    // Asymmetric projection from tangent half-angles (OpenXR fov).
    static Mat4 Projection(float tanLeft, float tanRight, float tanUp, float tanDown, float nearZ, float farZ) {
        Mat4 r;
        const float w = tanRight - tanLeft, h = tanUp - tanDown;
        r(0, 0) = 2 / w;
        r(0, 2) = (tanRight + tanLeft) / w;
        r(1, 1) = 2 / h;
        r(1, 2) = (tanUp + tanDown) / h;
        r(2, 2) = -(farZ + nearZ) / (farZ - nearZ);
        r(2, 3) = -(2 * farZ * nearZ) / (farZ - nearZ);
        r(3, 2) = -1;
        r(3, 3) = 0;
        return r;
    }
};

// Ray / plane intersection against a rectangle centred at `pose` with half
// extents (hw, hh) in its local XY plane. Returns distance or -1.
inline float RayRect(const Vec3& origin, const Vec3& dir, const Pose& pose, float hw, float hh, Vec2* local) {
    const Vec3 n = pose.orientation.Rotate({0, 0, 1});
    const float denom = Dot(dir, n);
    if (std::fabs(denom) < 1e-6f) return -1;
    const float t = Dot(pose.position - origin, n) / denom;
    if (t < 0) return -1;
    const Vec3 hit = origin + dir * t;
    const Vec3 l = pose.orientation.Conjugate().Rotate(hit - pose.position);
    if (local) *local = {l.x, l.y};
    if (std::fabs(l.x) > hw || std::fabs(l.y) > hh) return -1;
    return t;
}

}  // namespace rv
