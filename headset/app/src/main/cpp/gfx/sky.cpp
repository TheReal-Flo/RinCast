#include "sky.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "rc/platform.h"

#ifndef GL_TIME_ELAPSED_EXT
#define GL_TIME_ELAPSED_EXT 0x88BF
#endif
#ifndef GL_GPU_DISJOINT_EXT
#define GL_GPU_DISJOINT_EXT 0x8FBB
#endif

namespace rv {

namespace {

// A cube face has about 13 texels per degree at its centre and 40 at its corners.
// Five mips are enough for that range, and the chain costs a third more memory.
constexpr int kFaceSize = 1536;
constexpr int kMipLevels = 5;
constexpr int kTileSize = 256;
constexpr int kTilesPerSide = kFaceSize / kTileSize;
constexpr int kStarCount = 70000;

constexpr int kStreakCount = 420;
constexpr int kRingSegments = 128;

constexpr float kBakeBudgetMs = 2.5f;
constexpr int kMaxTilesPerFrame = 24;
constexpr float kFadeSeconds = 0.45f;
constexpr float kDriftRadiansPerSecond = 0.00017f;  // one turn in about ten hours

// Direction = u * right + v * up + forward, for texture coordinates in the cube
// map's own convention (s, t in -1..1 across each face).
struct FaceBasis {
    float m[9];  // column-major: right, up, forward
};
constexpr FaceBasis kFaces[6] = {
    {{0, 0, -1, 0, -1, 0, 1, 0, 0}},   // +X
    {{0, 0, 1, 0, -1, 0, -1, 0, 0}},   // -X
    {{1, 0, 0, 0, 0, 1, 0, 1, 0}},     // +Y
    {{1, 0, 0, 0, 0, -1, 0, -1, 0}},   // -Y
    {{1, 0, 0, 0, -1, 0, 0, 0, 1}},    // +Z
    {{-1, 0, 0, 0, -1, 0, 0, 0, -1}},  // -Z
};

// Everything the bake shaders share. Themes 0-5 are the space colours in
// Environment order.
const char kCommon[] = R"GLSL(
precision highp int;

uniform int uTheme;

const vec3 kNebulaA[6] = vec3[6](vec3(0.20, 0.07, 0.62), vec3(0.04, 0.13, 0.70), vec3(0.01, 0.30, 0.46),
                                 vec3(0.01, 0.30, 0.17), vec3(0.50, 0.04, 0.34), vec3(0.52, 0.04, 0.05));
const vec3 kNebulaB[6] = vec3[6](vec3(0.62, 0.10, 0.48), vec3(0.03, 0.40, 0.62), vec3(0.04, 0.16, 0.58),
                                 vec3(0.02, 0.28, 0.42), vec3(0.20, 0.06, 0.62), vec3(0.58, 0.20, 0.04));
const vec3 kNebulaCore[6] = vec3[6](vec3(1.00, 0.72, 0.86), vec3(0.72, 0.92, 1.00), vec3(0.70, 1.00, 0.92),
                                    vec3(0.82, 1.00, 0.72), vec3(1.00, 0.76, 0.72), vec3(1.00, 0.80, 0.52));
const vec3 kDeep[6] = vec3[6](vec3(0.0052, 0.0034, 0.0130), vec3(0.0026, 0.0048, 0.0140), vec3(0.0018, 0.0070, 0.0110),
                              vec3(0.0018, 0.0066, 0.0066), vec3(0.0088, 0.0028, 0.0096), vec3(0.0094, 0.0028, 0.0038));
const vec3 kSeed[6] = vec3[6](vec3(3.7, -8.1, 12.4), vec3(-21.3, 5.6, 2.2), vec3(9.9, 17.2, -6.5),
                              vec3(-4.4, -13.7, -19.1), vec3(27.5, 1.9, 8.8), vec3(-15.2, 23.4, 14.6));
const vec3 kGalaxyPole[6] = vec3[6](vec3(0.34, 0.86, 0.38), vec3(-0.48, 0.74, -0.47), vec3(0.62, 0.70, -0.35),
                                    vec3(-0.27, 0.91, 0.31), vec3(0.51, 0.66, 0.55), vec3(-0.58, 0.79, 0.20));

uint Scramble(ivec3 c) {
    uint h = uint(c.x) * 0x8DA6B343u ^ uint(c.y) * 0xD8163841u ^ uint(c.z) * 0xCB1AB31Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    return h ^ (h >> 15);
}

float Unit(uint h) { return float(h >> 8) / 16777216.0; }
)GLSL";

// Simplex noise rather than a cube lattice: only four corners to visit, and no
// axis-aligned features for the eye to pick up across the sky.
const char kNoise[] = R"GLSL(
vec3 CornerGradient(ivec3 c) {
    uint h = Scramble(c);
    return vec3(float(h & 1023u), float((h >> 10) & 1023u), float((h >> 20) & 1023u)) / 511.5 - 1.0;
}

float Simplex(vec3 p) {
    vec3 cell = floor(p + dot(p, vec3(1.0 / 3.0)));
    vec3 x0 = p - cell + dot(cell, vec3(1.0 / 6.0));
    vec3 order = step(x0.yzx, x0.xyz);
    vec3 lo = order * (1.0 - order.zxy);
    vec3 hi = 1.0 - order.zxy * (1.0 - order);
    vec3 x1 = x0 - lo + 1.0 / 6.0;
    vec3 x2 = x0 - hi + 1.0 / 3.0;
    vec3 x3 = x0 - 0.5;
    ivec3 c = ivec3(cell);
    vec4 w = max(0.5 - vec4(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), 0.0);
    w *= w;
    w *= w;
    return 38.0 * dot(w, vec4(dot(x0, CornerGradient(c)), dot(x1, CornerGradient(c + ivec3(lo))),
                              dot(x2, CornerGradient(c + ivec3(hi))), dot(x3, CornerGradient(c + ivec3(1)))));
}

float Fbm(vec3 p, int octaves) {
    float sum = 0.0, amplitude = 0.5;
    for (int i = 0; i < octaves; i++) {
        sum += amplitude * Simplex(p);
        p = p.zxy * 2.03 + vec3(1.7, -2.9, 4.1);
        amplitude *= 0.5;
    }
    return sum;
}
)GLSL";

// The hardware encodes to sRGB on write, so the shader outputs linear light. To
// dither in the encoded domain, where the banding lives, it goes through the exact
// transfer function and back.
const char kDither[] = R"GLSL(
vec3 EncodeSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

vec3 DecodeSrgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
}

vec3 Dithered(vec3 linear) {
    ivec3 at = ivec3(ivec2(gl_FragCoord.xy), uTheme);
    uint a = Scramble(at), b = Scramble(at + ivec3(7919, 104729, 11));
    vec3 triangular = vec3(Unit(a), Unit(a * 0x9E3779B1u), Unit(b)) -
                      vec3(Unit(b * 0x9E3779B1u), Unit(a ^ b), Unit((a ^ b) * 0x85EBCA77u));
    return DecodeSrgb(EncodeSrgb(linear) + triangular / 255.0);
}
)GLSL";

const char kBakeVs[] = R"GLSL(
uniform mat3 uFace;
out vec3 vDir;
void main() {
    vec2 ndc = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vDir = uFace * vec3(ndc, 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
}
)GLSL";

const char kSpaceFs[] = R"GLSL(
in vec3 vDir;
out vec4 fragColor;

void main() {
    vec3 d = normalize(vDir);
    vec3 seed = kSeed[uTheme];
    vec3 pole = normalize(kGalaxyPole[uTheme]);

    // Two-stage domain warp: the coordinates of the gas are themselves noise.
    vec3 p = d * 1.55 + seed;
    vec3 warp = vec3(Fbm(p, 3), Fbm(p + vec3(5.2, 1.3, -2.8), 3), Fbm(p + vec3(-3.1, 7.7, 4.4), 3));
    vec3 q = p + warp * 1.25;

    float region = smoothstep(-0.16, 0.34, Fbm(d * 1.05 + seed * 0.37, 2));
    float gas = Fbm(q * 1.65, 5);
    float wisps = 1.0 - abs(Fbm(q * 3.2 + warp * 0.8, 4)) * 2.6;

    float lanes = smoothstep(0.55, 1.0, wisps) * smoothstep(0.05, -0.30, warp.z);
    float density = region * smoothstep(-0.10, 0.50, gas + 0.08) * (1.0 - 0.55 * lanes);
    float bright = density * density;
    float filaments = region * smoothstep(0.35, 1.0, wisps) * smoothstep(-0.25, 0.25, gas);

    vec3 tint = mix(kNebulaA[uTheme], kNebulaB[uTheme], smoothstep(-0.3, 0.3, warp.y));
    vec3 color = kDeep[uTheme] * (0.75 + 0.5 * smoothstep(-0.5, 0.5, warp.x));
    color += tint * (0.10 * density + 0.72 * bright + 0.20 * filaments * (0.35 + density));
    color += kNebulaCore[uTheme] * 0.46 * bright * smoothstep(0.35, 0.95, density);

    // The unresolved stars of a galactic disc, cut by dust lanes.
    float lat = dot(d, pole);
    float along = dot(d, normalize(cross(pole, vec3(0.31, -0.22, 0.92))));
    float bulge = 0.55 + 0.45 * smoothstep(-0.2, 0.95, along);
    float disc = exp(-lat * lat * 34.0) * bulge;
    float dust = smoothstep(-0.18, 0.34, Fbm(q * 2.7 + 9.0, 3) + 3.0 * abs(lat));
    vec3 starlight = mix(mix(kNebulaB[uTheme], vec3(0.72, 0.70, 0.80), 0.45), vec3(0.95, 0.84, 0.72), smoothstep(0.4, 1.0, disc));
    color *= 1.0 - 0.45 * disc * (1.0 - dust);
    color += starlight * 0.016 * disc * mix(0.15, 1.0, dust);

    fragColor = vec4(Dithered(color), 1.0);
}
)GLSL";

// Stars are real instanced quads rather than cells of a noise function: position
// and brightness come straight from probability distributions, so there's no grid
// to see. Each quad is a tangent patch on the sphere pushed through the face's
// projection, so a star keeps its size and shape across a cube edge.
const char kStarsVs[] = R"GLSL(
uniform mat3 uFace;
uniform float uTexel;
out vec2 vCorner;
out vec3 vColor;
out float vHalo;

const vec3 kStarTint[6] = vec3[6](vec3(0.56, 0.68, 1.00), vec3(0.76, 0.85, 1.00), vec3(1.00, 0.98, 0.95),
                                  vec3(1.00, 0.90, 0.72), vec3(1.00, 0.74, 0.48), vec3(1.00, 0.54, 0.34));

void main() {
    int id = gl_InstanceID;
    uint a = Scramble(ivec3(id, 1, uTheme)), b = Scramble(ivec3(id, 2, uTheme));
    uint c = Scramble(ivec3(id, 3, uTheme)), e = Scramble(ivec3(id, 4, uTheme));
    uint f = Scramble(ivec3(id, 5, uTheme)), g = Scramble(ivec3(id, 6, uTheme));

    // Direction in the galaxy's frame: a uniform sphere, plus a share of the stars
    // squeezed towards the disc and the bulge.
    float sinLat = Unit(a) * 2.0 - 1.0;
    float lon = Unit(b) * 6.2831853;
    if (Unit(c) < 0.52) {
        // Triangular, not a power of one sample: those pile up into visible lines.
        sinLat = (Unit(a) + Unit(f) - 1.0) * 0.46;
        if (Unit(c * 0x9E3779B1u) < 0.45) lon = (Unit(b) + Unit(g) - 1.0) * 3.14159265;
    }
    float cosLat = sqrt(max(1.0 - sinLat * sinLat, 0.0));
    vec3 pole = normalize(kGalaxyPole[uTheme]);
    vec3 axisX = normalize(cross(pole, vec3(0.31, -0.22, 0.92)));
    vec3 axisY = cross(pole, axisX);
    vec3 d = axisX * cosLat * cos(lon) + axisY * cosLat * sin(lon) + pole * sinLat;

    // Pareto brightness, as for stars spread through space: N(>F) falls as F^-1.4.
    float u = Unit(e * 0x9E3779B1u);
    float flux = min(pow(max(1.0 - u, 1e-5), -0.72), 900.0);
    float sigma = 0.72 + 0.11 * log2(flux);
    float peak = 0.0085 * flux * (0.72 * 0.72) / (sigma * sigma);
    float halo = smoothstep(90.0, 500.0, flux);
    float extent = (halo > 0.0 ? 8.0 : 3.2) * sigma * uTexel;

    vec3 tint = kStarTint[int(Unit(a * 0x85EBCA77u) * 5.999)];
    tint = mix(vec3(dot(tint, vec3(0.3, 0.55, 0.15))), tint, 0.45 + 0.55 * smoothstep(1.0, 30.0, flux));

    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vec3 side = normalize(cross(d, abs(d.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 p = d + (side * corner.x + cross(d, side) * corner.y) * extent;

    mat3 toFace = transpose(uFace);
    vec3 clip = toFace * p;
    vCorner = corner;
    vColor = tint * peak;
    vHalo = halo * 0.035;
    // A star this far off-axis can't touch the face.
    gl_Position = (toFace * d).z < 0.5 ? vec4(2.0, 2.0, 2.0, 1.0) : vec4(clip.xy, 0.0, clip.z);
}
)GLSL";

const char kStarsFs[] = R"GLSL(
in vec2 vCorner;
in vec3 vColor;
in float vHalo;
out vec4 fragColor;
void main() {
    float r2 = dot(vCorner, vCorner);
    if (r2 >= 1.0) discard;
    float window = (1.0 - r2) * (1.0 - r2);
    float shape = vHalo > 0.0 ? exp(-r2 * 58.0) + vHalo * exp(-r2 * 7.0) * window : exp(-r2 * 5.1) * window;
    fragColor = vec4(vColor * shape, 0.0);
}
)GLSL";

// A short march through a slab of cloud, seen from below. Rays near the horizon
// dissolve into haze, and everything under it is one flat, soft tone, so looking
// down gives the eye nothing to fall into.
const char kCloudsFs[] = R"GLSL(
in vec3 vDir;
out vec4 fragColor;

const float kBase = 1.0;
const float kTop = 1.42;
const vec3 kSun = vec3(0.4183, 0.5777, -0.7008);
const vec3 kZenith = vec3(0.075, 0.190, 0.500);
const vec3 kMidSky = vec3(0.230, 0.430, 0.780);
const vec3 kHorizon = vec3(0.660, 0.770, 0.880);
const vec3 kGround = vec3(0.400, 0.470, 0.520);
const vec3 kGroundNadir = vec3(0.235, 0.285, 0.320);

float Cloud(vec3 p, bool detail) {
    float h = clamp((p.y - kBase) / (kTop - kBase), 0.0, 1.0);
    float profile = smoothstep(0.0, 0.10, h) * (1.0 - smoothstep(0.25, 1.0, h));
    vec3 s = p * vec3(1.15, 1.9, 1.15);
    float shape = Fbm(s + vec3(4.1, 0.0, -2.3), 3) + 0.07;
    float d = shape - (1.0 - profile) * 0.55;
    if (d <= 0.0 || !detail) return max(d, 0.0);
    return max(d - 0.16 * (0.5 + Fbm(p * 5.3 + vec3(0.0, -1.3, 2.7), 3)), 0.0);
}

void main() {
    vec3 d = normalize(vDir);
    float up = d.y;
    float e = max(up, 0.0);
    vec3 sky = mix(kHorizon, kMidSky, smoothstep(0.0, 0.32, e));
    sky = mix(sky, kZenith, smoothstep(0.22, 1.0, e));
    float toSun = max(dot(d, kSun), 0.0);
    sky += vec3(0.95, 0.80, 0.60) * (0.08 * pow(toSun, 6.0) + 0.30 * pow(toSun, 120.0));

    vec3 color = sky;
    if (up > 0.012) {
        float tNear = kBase / up, tFar = min(kTop / up, 34.0);
        const int kSteps = 9;
        float dt = max(tFar - tNear, 0.0) / float(kSteps);
        float t = tNear + dt * (0.25 + 0.5 * Unit(Scramble(ivec3(ivec2(gl_FragCoord.xy), 3))));
        float transmit = 1.0;
        vec3 light = vec3(0.0);
        for (int i = 0; i < kSteps && transmit > 0.02 && t < tFar; i++) {
            vec3 p = d * t;
            float density = Cloud(p, true);
            if (density > 0.001) {
                float sunlit = exp(-Cloud(p + kSun * 0.20, false) * 7.0);
                float h = clamp((p.y - kBase) / (kTop - kBase), 0.0, 1.0);
                vec3 lit = mix(vec3(0.34, 0.42, 0.56), vec3(0.62, 0.68, 0.78), h) + vec3(0.62, 0.58, 0.50) * sunlit;
                float absorb = 1.0 - exp(-density * dt * 22.0);
                light += transmit * absorb * lit;
                transmit *= 1.0 - absorb;
            }
            t += dt;
        }
        float haze = 1.0 - exp(-tNear * 0.15);
        color = mix(light + sky * transmit, mix(kHorizon, sky, 0.25), haze * haze);
    }

    if (up < 0.0) {
        color = mix(kGround, kGroundNadir, smoothstep(0.0, 0.9, -up));
        color = mix(kHorizon, color, smoothstep(0.0, 0.22, -up));
    } else {
        color = mix(color, kHorizon, 1.0 - smoothstep(0.0, 0.045, up));
    }
    fragColor = vec4(Dithered(color), 1.0);
}
)GLSL";

// All the headset has to do every frame.
const char kSkyVs[] = R"GLSL(
uniform vec4 uRay;  // the projection's x scale, y scale, x shift, y shift
uniform mat3 uToWorld;
out vec3 vDir;
void main() {
    vec2 ndc = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vDir = uToWorld * vec3((ndc + uRay.zw) / uRay.xy, -1.0);
    gl_Position = vec4(ndc, 1.0, 1.0);
}
)GLSL";

// Interleaved gradient noise, lifted through a square root (close enough to the
// sRGB curve for this): keeps the 8-bit eye buffer from banding the dark skies.
const char kEyeDither[] = R"GLSL(
vec3 EyeDither(vec3 c) {
    float n = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715)))) - 0.5;
    vec3 s = sqrt(max(c, vec3(0.0))) + n / 255.0;
    return s * s;
}
)GLSL";

const char kSkyFs[] = R"GLSL(
uniform highp samplerCube uSky;
uniform float uBrightness;
uniform float uAlpha;
in vec3 vDir;
out vec4 fragColor;
void main() {
    vec3 c = texture(uSky, vDir).rgb * uBrightness;
    fragColor = vec4(EyeDither(c) * uAlpha, uAlpha);
}
)GLSL";

// The void, and the stand-in shown while a sky bakes. Themes 0-5 are space, 6 the
// day sky, 7 the void.
const char kGradientFs[] = R"GLSL(
uniform int uTheme;
uniform float uBrightness;
uniform float uAlpha;
in vec3 vDir;
out vec4 fragColor;

const vec3 kTop[8] = vec3[8](vec3(0.0052, 0.0034, 0.0130), vec3(0.0026, 0.0048, 0.0140), vec3(0.0018, 0.0070, 0.0110),
                             vec3(0.0018, 0.0066, 0.0066), vec3(0.0088, 0.0028, 0.0096), vec3(0.0094, 0.0028, 0.0038),
                             vec3(0.120, 0.280, 0.640), vec3(0.0034, 0.0032, 0.0062));
const vec3 kMiddle[8] = vec3[8](vec3(0.0052, 0.0034, 0.0130), vec3(0.0026, 0.0048, 0.0140), vec3(0.0018, 0.0070, 0.0110),
                                vec3(0.0018, 0.0066, 0.0066), vec3(0.0088, 0.0028, 0.0096), vec3(0.0094, 0.0028, 0.0038),
                                vec3(0.660, 0.770, 0.880), vec3(0.0150, 0.0136, 0.0230));
const vec3 kBottom[8] = vec3[8](vec3(0.0052, 0.0034, 0.0130), vec3(0.0026, 0.0048, 0.0140), vec3(0.0018, 0.0070, 0.0110),
                                vec3(0.0018, 0.0066, 0.0066), vec3(0.0088, 0.0028, 0.0096), vec3(0.0094, 0.0028, 0.0038),
                                vec3(0.300, 0.360, 0.400), vec3(0.0026, 0.0025, 0.0044));

void main() {
    float y = vDir.y / length(vDir);
    vec3 c = y > 0.0 ? mix(kMiddle[uTheme], kTop[uTheme], smoothstep(0.0, 0.75, y))
                     : mix(kMiddle[uTheme], kBottom[uTheme], smoothstep(0.0, 0.45, -y));
    fragColor = vec4(EyeDither(c * uBrightness) * uAlpha, uAlpha);
}
)GLSL";

// The streaks live on the walls of a long cylinder around the viewer, flying
// towards them. No eye offset is applied, so they read as far away, and the sky
// behind stays still: that steady background is what keeps this comfortable.
const char kStreaksVs[] = R"GLSL(
precision highp int;
uniform mat4 uTunnelToClip;
uniform float uPhase;
uniform float uRate;
uniform float uFade;
out vec2 vRibbon;
out vec3 vColor;

uint Next(inout uint state) {
    state = state * 0x2C1B3C6Du + 0x9E3779B1u;
    uint h = state ^ (state >> 15);
    h *= 0x297A2D39u;
    return h ^ (h >> 13);
}
float Unit(uint h) { return float(h >> 8) / 16777216.0; }

const float kTravel = 104.0;

void main() {
    uint state = uint(gl_InstanceID) * 0x8DA6B343u + 17u;
    float angle = Unit(Next(state)) * 6.2831853;
    float radius = mix(4.5, 11.0, Unit(Next(state)));
    float lane = mix(0.72, 1.30, Unit(Next(state)));
    float start = Unit(Next(state));
    float shade = Unit(Next(state));
    float weight = Unit(Next(state));

    float head = mod(start * kTravel + uPhase * 26.0 * lane, kTravel) - 92.0;  // -92 far ahead, +12 behind
    float pace = clamp(uRate, 0.0, 1.0);
    float burst = max(uRate - 1.0, 0.0);
    float len = (1.0 + 26.0 * pace * pace + 16.0 * burst) * lane;

    float side = float(gl_VertexID & 1) * 2.0 - 1.0;
    float end = float(gl_VertexID >> 1);  // 0 tail, 1 head

    vec3 radial = vec3(cos(angle), sin(angle), 0.0);
    vec3 tangent = vec3(-radial.y, radial.x, 0.0);
    vec3 center = radial * radius + vec3(0.0, 0.0, head - (1.0 - end) * len);

    // There's no MSAA: a ribbon never gets thinner than about a pixel and a half,
    // and dims instead, so distant streaks fade out rather than crawl.
    float wanted = mix(0.010, 0.030, weight * weight) * (1.0 + 0.5 * pace);
    float width = max(wanted, length(center) * 0.0012);
    vec3 p = center + tangent * side * width;

    float arrive = smoothstep(-92.0, -38.0, head) * (1.0 - smoothstep(4.0, 12.0, head));
    vec3 tint = mix(vec3(0.46, 0.66, 1.00), vec3(0.74, 0.60, 1.00), smoothstep(0.55, 1.0, shade));
    tint = mix(tint, vec3(1.0), 0.10 + 0.30 * weight);

    vRibbon = vec2(side, end);
    vColor = tint * arrive * uFade * (0.30 + 0.70 * pace) * mix(0.40, 1.0, weight) * wanted / width;
    gl_Position = uTunnelToClip * vec4(p, 1.0);
}
)GLSL";

const char kStreaksFs[] = R"GLSL(
in vec2 vRibbon;
in vec3 vColor;
out vec4 fragColor;
void main() {
    float across = 1.0 - vRibbon.x * vRibbon.x;
    float along = vRibbon.y * vRibbon.y;
    float a = across * across * along * smoothstep(1.0, 0.94, vRibbon.y);
    vec3 c = vColor * a;
    fragColor = vec4(c * 1.05, max(c.r, max(c.g, c.b)) * 0.45);
}
)GLSL";

// A ring of light ahead of the viewer: it tightens as the jump builds, then opens
// and sweeps past on arrival.
const char kRingVs[] = R"GLSL(
uniform mat4 uTunnelToClip;
uniform float uPhase;
uniform float uRate;
uniform float uFade;
uniform int uSegments;
out float vBand;
out vec3 vColor;
void main() {
    int i = gl_VertexID >> 1;
    float band = float(gl_VertexID & 1) * 2.0 - 1.0;
    float a = 6.2831853 * float(i) / float(uSegments);

    float pace = clamp(uRate, 0.0, 1.0);
    float burst = clamp((uRate - 1.0) / 2.8, 0.0, 1.0);
    float radius = mix(0.60, 0.33, pace) + 1.9 * burst * burst;
    float halfWidth = radius * 0.34;
    vec3 p = vec3(vec2(cos(a), sin(a)) * (radius + band * halfWidth), -1.0);

    float t = uPhase * 2.2;
    float shimmer = 0.78 + 0.13 * sin(a * 3.0 + t) + 0.09 * sin(a * 7.0 - t * 1.6);
    float strength = uFade * shimmer * (0.26 + 0.74 * pace) * (1.0 - 0.75 * burst);

    vBand = band;
    vColor = mix(vec3(0.42, 0.62, 1.00), vec3(0.72, 0.56, 1.00), 0.5 + 0.5 * sin(a + t * 0.4)) * strength;
    gl_Position = uTunnelToClip * vec4(p * 30.0, 1.0);
}
)GLSL";

const char kRingFs[] = R"GLSL(
in float vBand;
in vec3 vColor;
out vec4 fragColor;
void main() {
    float b = vBand;
    float edge = 1.0 - b * b;
    float glow = edge * edge * edge * 0.20;
    float core = exp(-b * b * 90.0);
    vec3 c = vColor * glow + mix(vColor, vec3(max(vColor.r, max(vColor.g, vColor.b))), 0.30) * core * 0.85;
    fragColor = vec4(c, max(c.r, max(c.g, c.b)) * 0.5);
}
)GLSL";

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The index of an environment's colours in the shaders' theme tables.
int ThemeOf(Environment env) { return std::clamp(int(env), 0, 7); }

bool NeedsBake(Environment env) { return env != Environment::Void && env != Environment::Passthrough; }

void FillToWorld(const Quat& q, float out[9]) {
    const Vec3 x = q.Rotate({1, 0, 0}), y = q.Rotate({0, 1, 0}), z = q.Rotate({0, 0, 1});
    const float m[9] = {x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z};
    std::memcpy(out, m, sizeof(m));
}

}  // namespace

const std::vector<EnvironmentInfo>& Environments() {
    static const std::vector<EnvironmentInfo> list = {
        {Environment::SpaceViolet, "space-violet", "Violet Nebula"},
        {Environment::SpaceBlue, "space-blue", "Blue Nebula"},
        {Environment::SpaceCyan, "space-cyan", "Cyan Nebula"},
        {Environment::SpaceGreen, "space-green", "Green Nebula"},
        {Environment::SpacePink, "space-pink", "Pink Nebula"},
        {Environment::SpaceRed, "space-red", "Red Nebula"},
        {Environment::DayClouds, "day-clouds", "Day Clouds"},
        {Environment::Void, "void", "Void"},
        {Environment::Passthrough, "passthrough", "Passthrough"},
    };
    return list;
}

Environment EnvironmentFromKey(const std::string& key) {
    for (const auto& e : Environments())
        if (key == e.key) return e.id;
    return Environment::SpaceViolet;
}

bool SkyRenderer::Init(Assets&) {
    const std::string bakeVs = std::string(kCommon) + kBakeVs;
    const std::string starsVs = std::string(kCommon) + kStarsVs;
    const std::string spaceFs = std::string(kCommon) + kNoise + kDither + kSpaceFs;
    const std::string cloudsFs = std::string(kCommon) + kNoise + kDither + kCloudsFs;
    const std::string skyFs = std::string(kEyeDither) + kSkyFs;
    const std::string gradientFs = std::string(kEyeDither) + kGradientFs;
    bool ok = m_sky.Build(kSkyVs, skyFs.c_str(), "sky");
    ok &= m_gradient.Build(kSkyVs, gradientFs.c_str(), "sky-gradient");
    ok &= m_bakeSpace.Build(bakeVs.c_str(), spaceFs.c_str(), "sky-bake-space");
    ok &= m_bakeClouds.Build(bakeVs.c_str(), cloudsFs.c_str(), "sky-bake-clouds");
    ok &= m_bakeStars.Build(starsVs.c_str(), kStarsFs, "sky-bake-stars");
    ok &= m_streaks.Build(kStreaksVs, kStreaksFs, "sky-streaks");
    // ok &= m_ring.Build(kRingVs, kRingFs, "sky-ring");

    glGenFramebuffers(1, &m_bakeFbo);
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (extensions && std::strstr(extensions, "GL_EXT_disjoint_timer_query")) glGenQueries(1, &m_timer);
    return ok;
}

int SkyRenderer::FindReadySlot(Environment env) const {
    for (int i = 0; i < 2; i++)
        if (m_slots[i].ready && m_slots[i].env == env) return i;
    return -1;
}

void SkyRenderer::Show(Layer next) {
    m_from = m_to;
    m_to = next;
    m_fade = 0;
}

void SkyRenderer::Select(Environment env) {
    if (m_hasTarget && env == m_target) return;
    const bool first = !m_hasTarget;
    m_hasTarget = true;
    m_target = env;
    m_bake.active = false;  // a partly baked slot just stays not ready

    if (!NeedsBake(env)) {
        Show({env, -1});
    } else if (const int ready = FindReadySlot(env); ready >= 0) {
        Show({env, ready});
    } else {
        // Keep a finished sky up while baking. Passthrough has just been switched
        // off, though, so it needs the stand-in gradient, as does a stand-in for
        // some other environment.
        const int slot = m_to.slot == 0 ? 1 : 0;
        if (m_from.slot == slot) m_fade = 1;  // the fade reading that slot has to end now
        if (m_to.slot < 0 && m_to.env != Environment::Void) Show({env, -1});
        StartBake(env, slot);
    }
    if (first) m_fade = 1;
}

void SkyRenderer::Prepare(Environment env, float time) {
    if (!m_hasTarget) m_lastPrepareTime = time;
    Select(env);
    const float dt = std::max(time - m_lastPrepareTime, 0.f);
    m_lastPrepareTime = time;
    m_fade = std::min(1.f, m_fade + dt / kFadeSeconds);
    m_drift += dt * kDriftRadiansPerSecond;
    if (m_bake.active) BakeSlice();
}

void SkyRenderer::StartBake(Environment env, int slotIndex) {
    Slot& slot = m_slots[slotIndex];
    slot.ready = false;
    slot.env = env;
    if (!slot.texture) {
        glGenTextures(1, &slot.texture);
        glBindTexture(GL_TEXTURE_CUBE_MAP, slot.texture);
        glTexStorage2D(GL_TEXTURE_CUBE_MAP, kMipLevels, GL_SRGB8_ALPHA8, kFaceSize, kFaceSize);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
    m_bake = Bake{};
    m_bake.active = true;
    m_bake.slot = slotIndex;
    m_bake.startSeconds = NowSeconds();
}

void SkyRenderer::CollectTimerResult() {
    if (!m_timerPending) return;
    GLuint available = 0;
    glGetQueryObjectuiv(m_timer, GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) return;
    m_timerPending = false;
    GLuint ns = 0;
    glGetQueryObjectuiv(m_timer, GL_QUERY_RESULT, &ns);
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    if (disjoint || m_timerTiles <= 0) return;
    const double ms = double(ns) / 1e6;
    m_bake.gpuMs += ms;
    const double perTile = ms / m_timerTiles;
    if (perTile > 0) m_tilesPerFrame = std::clamp(int(kBakeBudgetMs / perTile), 1, kMaxTilesPerFrame);
}

void SkyRenderer::BakeTiles(int count) {
    const Slot& slot = m_slots[m_bake.slot];
    const Program& program = slot.env == Environment::DayClouds ? m_bakeClouds : m_bakeSpace;
    program.Use();
    glUniform1i(program.Uniform("uTheme"), ThemeOf(slot.env));
    glUniformMatrix3fv(program.Uniform("uFace"), 1, GL_FALSE, kFaces[m_bake.face].m);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GLenum(GL_TEXTURE_CUBE_MAP_POSITIVE_X + m_bake.face),
                           slot.texture, 0);
    glEnable(GL_SCISSOR_TEST);
    const int last = std::min(m_bake.tile + count, kTilesPerSide * kTilesPerSide);
    for (; m_bake.tile < last; m_bake.tile++) {
        glScissor((m_bake.tile % kTilesPerSide) * kTileSize, (m_bake.tile / kTilesPerSide) * kTileSize, kTileSize,
                  kTileSize);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glDisable(GL_SCISSOR_TEST);
}

void SkyRenderer::BakeStars() {
    const Slot& slot = m_slots[m_bake.slot];
    m_bakeStars.Use();
    glUniform1i(m_bakeStars.Uniform("uTheme"), ThemeOf(slot.env));
    glUniformMatrix3fv(m_bakeStars.Uniform("uFace"), 1, GL_FALSE, kFaces[m_bake.face].m);
    glUniform1f(m_bakeStars.Uniform("uTexel"), 2.f / kFaceSize);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GLenum(GL_TEXTURE_CUBE_MAP_POSITIVE_X + m_bake.face),
                           slot.texture, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, kStarCount);
    glDisable(GL_BLEND);
}

void SkyRenderer::FinishBake() {
    Slot& slot = m_slots[m_bake.slot];
    glBindTexture(GL_TEXTURE_CUBE_MAP, slot.texture);
    glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    slot.ready = true;
    m_bake.active = false;
    RC_LOGI("Sky %d baked in %d frames, %.0f ms wall clock, %.1f ms measured GPU (%d tiles/frame)", int(slot.env),
            m_bake.frames, (NowSeconds() - m_bake.startSeconds) * 1000.0, m_bake.gpuMs, m_tilesPerFrame);
    Show({slot.env, m_bake.slot});
    GlCheck("sky bake");
}

void SkyRenderer::BakeSlice() {
    CollectTimerResult();
    m_bake.frames++;

    glBindFramebuffer(GL_FRAMEBUFFER, m_bakeFbo);
    glViewport(0, 0, kFaceSize, kFaceSize);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glBindVertexArray(EmptyVao());

    if (m_bake.face >= 6) {
        FinishBake();
    } else if (m_bake.stars) {
        BakeStars();
        m_bake.stars = false;
        m_bake.face++;
        m_bake.tile = 0;
    } else {
        const bool timed = m_timer && !m_timerPending;
        if (timed) glBeginQuery(GL_TIME_ELAPSED_EXT, m_timer);
        const int before = m_bake.tile;
        BakeTiles(m_tilesPerFrame);
        if (timed) {
            glEndQuery(GL_TIME_ELAPSED_EXT);
            m_timerPending = true;
            m_timerTiles = m_bake.tile - before;
        }
        if (m_bake.tile >= kTilesPerSide * kTilesPerSide) {
            if (m_slots[m_bake.slot].env != Environment::DayClouds) {
                m_bake.stars = true;
            } else {
                m_bake.face++;
                m_bake.tile = 0;
            }
        }
    }

    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glFlush();
}

void SkyRenderer::DrawLayer(const Layer& layer, const float ray[4], const float toWorld[9], float brightness,
                            float alpha) {
    if (layer.env == Environment::Passthrough || alpha <= 0) return;
    const bool baked = layer.slot >= 0;
    const Program& program = baked ? m_sky : m_gradient;
    program.Use();
    glUniform4fv(program.Uniform("uRay"), 1, ray);
    glUniformMatrix3fv(program.Uniform("uToWorld"), 1, GL_FALSE, toWorld);
    glUniform1f(program.Uniform("uBrightness"), brightness);
    glUniform1f(program.Uniform("uAlpha"), alpha);
    if (baked) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, m_slots[layer.slot].texture);
        glUniform1i(program.Uniform("uSky"), 0);
    } else {
        glUniform1i(program.Uniform("uTheme"), ThemeOf(layer.env));
    }
    if (alpha < 1) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (baked) glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
}

void SkyRenderer::DrawWarp(const Mat4& projection, const Quat& headRotation, const Quat& tunnelOrientation, float warp,
                           float warpPhase, float time) {
    // Only the integrated phase is passed in, so the rate is recovered here. Both
    // eyes of a frame share one time value.
    if (time != m_warpLastTime) {
        const float dt = time - m_warpLastTime;
        if (dt > 0 && dt < 0.25f) m_warpRate = Damp(m_warpRate, std::max(warpPhase - m_warpLastPhase, 0.f) / dt, 12.f, dt);
        m_warpLastTime = time;
        m_warpLastPhase = warpPhase;
    }

    const Mat4 tunnelToClip = projection * Mat4::FromPose({headRotation.Conjugate() * tunnelOrientation, {0, 0, 0}});

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    m_ring.Use();
    glUniformMatrix4fv(m_ring.Uniform("uTunnelToClip"), 1, GL_FALSE, tunnelToClip.m);
    glUniform1f(m_ring.Uniform("uPhase"), warpPhase);
    glUniform1f(m_ring.Uniform("uRate"), m_warpRate);
    glUniform1f(m_ring.Uniform("uFade"), warp);
    glUniform1i(m_ring.Uniform("uSegments"), kRingSegments);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, (kRingSegments + 1) * 2);

    m_streaks.Use();
    glUniformMatrix4fv(m_streaks.Uniform("uTunnelToClip"), 1, GL_FALSE, tunnelToClip.m);
    glUniform1f(m_streaks.Uniform("uPhase"), warpPhase);
    glUniform1f(m_streaks.Uniform("uRate"), m_warpRate);
    glUniform1f(m_streaks.Uniform("uFade"), warp);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, kStreakCount);
}

void SkyRenderer::Draw(const Mat4& projection, const Quat& headRotation, Environment env, float time, float brightness,
                       float warp, float warpPhase, Quat tunnelOrientation) {
    (void)env;  // Prepare already selected it, and baking can't start with an eye bound
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glBindVertexArray(EmptyVao());

    const float ray[4] = {projection(0, 0), projection(1, 1), projection(0, 2), projection(1, 2)};
    // Yaw only, and far too slow to see as motion: it can't tilt the horizon.
    float toWorld[9];
    FillToWorld(Quat::Yaw(m_drift) * headRotation, toWorld);

    if (m_fade >= 1) {
        DrawLayer(m_to, ray, toWorld, brightness, 1);
    } else if (m_to.env == Environment::Passthrough) {
        DrawLayer(m_from, ray, toWorld, brightness, 1 - m_fade);
    } else {
        DrawLayer(m_from, ray, toWorld, brightness, 1);
        DrawLayer(m_to, ray, toWorld, brightness, m_fade);
    }

    if (warp > 0.002f) DrawWarp(projection, headRotation, tunnelOrientation, warp, warpPhase, time);

    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glBindVertexArray(0);
    GlCheck("sky draw");
}

}  // namespace rv
