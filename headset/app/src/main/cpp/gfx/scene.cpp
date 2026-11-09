#include "scene.h"

#include <cstring>
#include <vector>

namespace rv {

// Panel: a strip grid with u in [0,1] across, v in [0,1] down. The vertex
// shader bends it around a vertical cylinder centred `radius` behind the
// panel when uRadius > 0 (arc length == width).
static const char* kPanelVs = R"(
layout(location = 0) in vec2 aUv;
uniform mat4 uMvp;
uniform vec2 uSize;     // meters
uniform float uRadius;  // meters, 0 = flat
out vec2 vUv;
out vec2 vLocal;
void main() {
    vec2 local = vec2((aUv.x - 0.5) * uSize.x, (0.5 - aUv.y) * uSize.y);
    vec3 p = vec3(local, 0.0);
    if (uRadius > 0.0) {
        float a = local.x / uRadius;
        p = vec3(sin(a) * uRadius, local.y, uRadius - cos(a) * uRadius);
    }
    vUv = aUv;
    vLocal = local;
    gl_Position = uMvp * vec4(p, 1.0);
}
)";

static const char* kPanelFsCommon = R"(
uniform float uOpacity;
uniform float uBrightness;
uniform vec2 uSize;
uniform float uCorner;
uniform int uHole;
in vec2 vUv;
in vec2 vLocal;
out vec4 oColor;
float cornerMask() {
    vec2 q = abs(vLocal) - uSize * 0.5 + uCorner;
    float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - uCorner;
    return clamp(0.5 - d / max(fwidth(d), 1e-5), 0.0, 1.0);
}
)";

static const char* kPanel2DFs = R"(
uniform sampler2D uTex;
void main() {
    float m = cornerMask();
    if (uHole == 1) { if (m < 0.5) discard; oColor = vec4(0.0); return; }
    // UI textures are y-up (rendered via FBO): flip v.
    vec4 c = texture(uTex, vec2(vUv.x, 1.0 - vUv.y));
    // Clears what's behind by the canvas's coverage (blended with ZERO, ONE_MINUS_SRC_ALPHA).
    if (uHole == 2) { if (c.a < 0.02) discard; oColor = vec4(0.0, 0.0, 0.0, c.a); return; }
    oColor = vec4(c.rgb * uBrightness, c.a) * uOpacity * m;
    // Clear parts (around the canvas's rounded card) mustn't hide what's behind them.
    if (oColor.a < 0.02) discard;
}
)";

static const char* kPanelExtFs = R"(
uniform samplerExternalOES uTex;
uniform vec4 uCrop;  // visible part of the decoder buffer: offset.xy, scale.zw
void main() {
    float m = cornerMask();
    if (uHole == 1) { if (m < 0.5) discard; oColor = vec4(0.0); return; }
    // Video is gamma encoded; linearize because eye buffers are sRGB targets.
    vec3 g = texture(uTex, uCrop.xy + vUv * uCrop.zw).rgb;
    vec4 c = vec4(mix(g / 12.92, pow((g + 0.055) / 1.055, vec3(2.4)), step(0.04045, g)), 1.0);
    oColor = vec4(c.rgb * uBrightness, 1.0) * uOpacity * m;
    if (oColor.a < 0.02) discard;
}
)";

static const char* kBeamVs = R"(
layout(location = 0) in vec4 aPos;   // xyz, t (0 at start, 1 at end)
layout(location = 1) in float aSide;
uniform mat4 uViewProj;
out float vT;
out float vSide;
void main() { vT = aPos.w; vSide = aSide; gl_Position = uViewProj * vec4(aPos.xyz, 1.0); }
)";
static const char* kBeamFs = R"(
uniform vec4 uStart;
uniform vec4 uEnd;
in float vT;
in float vSide;
out vec4 oColor;
void main() {
    vec4 c = mix(uStart, uEnd, vT);
    float edge = 1.0 - abs(vSide);
    c.a *= smoothstep(0.0, 0.6, edge);
    oColor = vec4(c.rgb * c.a, c.a);
}
)";

static const char* kDotVs = R"(
uniform mat4 uViewProj;
uniform vec3 uCenter;
uniform vec3 uRight;
uniform vec3 uUp;
out vec2 vP;
void main() {
    vec2 c = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1)) * 2.0 - 1.0;
    vP = c;
    gl_Position = uViewProj * vec4(uCenter + uRight * c.x + uUp * c.y, 1.0);
}
)";
static const char* kDotFs = R"(
uniform vec4 uColor;
in vec2 vP;
out vec4 oColor;
void main() {
    float r = length(vP);
    float core = 1.0 - smoothstep(0.55, 0.65, r);
    float ring = (1.0 - smoothstep(0.85, 1.0, r)) * 0.35;
    float a = max(core, ring) * uColor.a;
    vec3 col = mix(uColor.rgb, vec3(1.0), core * 0.3);
    oColor = vec4(col * a, a);
}
)";

static const char* kBlitVs = R"(
uniform vec4 uRect;   // u0 v0 u1 v1
out vec2 vUv;
void main() {
    // Fullscreen triangle: c is (0,0), (2,0), (0,2); the screen spans c = 0..1,
    // so c itself is the 0..1 coordinate across the viewport.
    vec2 c = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = mix(uRect.xy, uRect.zw, c);
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
}
)";
// Foveated encoding (rc::VrFoveationMetadata): where a point of the full picture is in the
// squeezed frame. uv over both eyes side by side, y from the top.
static const char* kFoveateFs = R"(
uniform int uFoveated;
uniform vec4 uFoveaStart;   // left eye x, y, right eye x, y
uniform vec3 uFoveaLayout;  // size, density x, density y
float FoveaAxis(float u, float a, float s) {
    float F = uFoveaLayout.x;
    float e = u < a ? u * s : u < a + F ? a * s + (u - a) : a * s + F + (u - a - F) * s;
    return e / (F + (1.0 - F) * s);
}
vec2 Foveate(vec2 uv) {
    if (uFoveated == 0) return uv;
    float eye = uv.x < 0.5 ? 0.0 : 1.0;
    vec2 a = eye < 0.5 ? uFoveaStart.xy : uFoveaStart.zw;
    float x = FoveaAxis(uv.x * 2.0 - eye, a.x, uFoveaLayout.y);
    return vec2((x + eye) * 0.5, FoveaAxis(uv.y, a.y, uFoveaLayout.z));
}
)";
static const char* kBlitExtFs = R"(
uniform samplerExternalOES uTex;
uniform vec4 uCrop;  // visible part of the decoder buffer: offset.xy, scale.zw
in vec2 vUv;
out vec4 oColor;
// Layer swapchains are sRGB: linearize the gamma-encoded video first.
void main() {
    vec2 uv = Foveate(vec2(vUv.x, 1.0 - vUv.y));
    vec3 g = texture(uTex, uCrop.xy + uv * uCrop.zw).rgb;
    oColor = vec4(mix(g / 12.92, pow((g + 0.055) / 1.055, vec3(2.4)), step(0.04045, g)), 1.0);
}
)";
// The video as it is, gamma-encoded: the upscaler's input. External textures can't be gathered.
static const char* kBlitExtGammaFs = R"(
uniform samplerExternalOES uTex;
uniform vec4 uCrop;
in vec2 vUv;
out vec4 oColor;
void main() {
    vec2 uv = Foveate(vec2(vUv.x, 1.0 - vUv.y));
    oColor = vec4(texture(uTex, uCrop.xy + uv * uCrop.zw).rgb, 1.0);
}
)";
// Snapdragon Game Super Resolution 1, RGBA mode, from Qualcomm's sgsr1_shader_mobile.frag.
// Copyright (c) 2025, Qualcomm Innovation Center, Inc. SPDX-License-Identifier: BSD-3-Clause
// Adapted: names, and the result is linearized for the sRGB swapchain.
static const char* kGsrFs = R"(
precision mediump float;
uniform highp vec4 uViewport;  // 1/width, 1/height, width, height
uniform mediump sampler2D uTex;
in highp vec2 vUv;
out vec4 oColor;
float fastLanczos2(float x) {
    float wA = x - 4.0;
    float wB = x * wA - wA;
    wA *= wA;
    return wB * wA;
}
vec2 weightY(float dx, float dy, float c, float std) {
    float x = ((dx * dx) + (dy * dy)) * 0.55 + clamp(abs(c) * std, 0.0, 1.0);
    float w = fastLanczos2(x);
    return vec2(w, w * c);
}
void main() {
    const int mode = 1;
    const float edgeThreshold = 8.0 / 255.0;
    const float edgeSharpness = 2.0;
    vec4 color;
    color.xyz = textureLod(uTex, vUv, 0.0).xyz;
    highp vec2 imgCoord = (vUv * uViewport.zw) + vec2(-0.5, 0.5);
    highp vec2 imgCoordPixel = floor(imgCoord);
    highp vec2 coord = imgCoordPixel * uViewport.xy;
    vec2 pl = imgCoord - imgCoordPixel;
    vec4 left = textureGather(uTex, coord, mode);
    float edgeVote = abs(left.z - left.y) + abs(color[mode] - left.y) + abs(color[mode] - left.z);
    if (edgeVote > edgeThreshold) {
        coord.x += uViewport.x;
        vec4 right = textureGather(uTex, coord + vec2(uViewport.x, 0.0), mode);
        vec4 upDown;
        upDown.xy = textureGather(uTex, coord + vec2(0.0, -uViewport.y), mode).wz;
        upDown.zw = textureGather(uTex, coord + vec2(0.0, uViewport.y), mode).yx;
        float mean = (left.y + left.z + right.x + right.w) * 0.25;
        left = left - vec4(mean);
        right = right - vec4(mean);
        upDown = upDown - vec4(mean);
        color.w = color[mode] - mean;
        float sum = abs(left.x) + abs(left.y) + abs(left.z) + abs(left.w) + abs(right.x) + abs(right.y) + abs(right.z) +
                    abs(right.w) + abs(upDown.x) + abs(upDown.y) + abs(upDown.z) + abs(upDown.w);
        float std = 2.181818 / sum;
        vec2 aWY = weightY(pl.x, pl.y + 1.0, upDown.x, std);
        aWY += weightY(pl.x - 1.0, pl.y + 1.0, upDown.y, std);
        aWY += weightY(pl.x - 1.0, pl.y - 2.0, upDown.z, std);
        aWY += weightY(pl.x, pl.y - 2.0, upDown.w, std);
        aWY += weightY(pl.x + 1.0, pl.y - 1.0, left.x, std);
        aWY += weightY(pl.x, pl.y - 1.0, left.y, std);
        aWY += weightY(pl.x, pl.y, left.z, std);
        aWY += weightY(pl.x + 1.0, pl.y, left.w, std);
        aWY += weightY(pl.x - 1.0, pl.y - 1.0, right.x, std);
        aWY += weightY(pl.x - 2.0, pl.y - 1.0, right.y, std);
        aWY += weightY(pl.x - 2.0, pl.y, right.z, std);
        aWY += weightY(pl.x - 1.0, pl.y, right.w, std);
        float finalY = aWY.y / aWY.x;
        float maxY = max(max(left.y, left.z), max(right.x, right.w));
        float minY = min(min(left.y, left.z), min(right.x, right.w));
        finalY = clamp(edgeSharpness * finalY, minY, maxY);
        float deltaY = clamp(finalY - color.w, -23.0 / 255.0, 23.0 / 255.0);
        color.xyz = clamp(color.xyz + deltaY, 0.0, 1.0);
    }
    highp vec3 g = color.xyz;
    oColor = vec4(mix(g / 12.92, pow((g + 0.055) / 1.055, vec3(2.4)), step(0.04045, g)), 1.0);
}
)";
static const char* kKeptFs = R"(
uniform sampler2D uTex;
in vec2 vUv;
out vec4 oColor;
void main() {
    vec3 g = texture(uTex, vUv).rgb;
    oColor = vec4(mix(g / 12.92, pow((g + 0.055) / 1.055, vec3(2.4)), step(0.04045, g)), 1.0);
}
)";
// The kept picture has its top at v = 0; so does the motion field, whose y points down.
static const char* kKeptMovedFs = R"(
uniform sampler2D uTex;
uniform mediump sampler2D uMotion;
uniform vec2 uScale;
uniform vec2 uField;
in vec2 vUv;
out vec4 oColor;
void main() {
    vec2 uv = vec2(vUv.x, 1.0 - vUv.y);
    vec2 m = texture(uMotion, uv * uField).rg;
    uv -= m * uScale;
    float eye = step(0.5, vUv.x);
    uv.x = clamp(uv.x, eye * 0.5, 0.5 + eye * 0.5);
    vec3 g = texture(uTex, vec2(uv.x, 1.0 - uv.y)).rgb;
    oColor = vec4(mix(g / 12.92, pow((g + 0.055) / 1.055, vec3(2.4)), step(0.04045, g)), 1.0);
}
)";
static const char* kBlit2DFs = R"(
uniform sampler2D uTex;
in vec2 vUv;
out vec4 oColor;
void main() { oColor = texture(uTex, vUv); }
)";

static std::string WithExt(const char* fs) {
    return std::string("#extension GL_OES_EGL_image_external_essl3 : require\n") + kPanelFsCommon + fs;
}

bool SceneRenderer::Init() {
    bool ok = m_panel2D.Build(kPanelVs, (std::string(kPanelFsCommon) + kPanel2DFs).c_str(), "panel2d");
    // GLSL requires #extension before any other statement: build the source with it first.
    const std::string extFs = WithExt(kPanelExtFs);
    ok &= m_panelExt.Build(kPanelVs, extFs.c_str(), "panelExt");
    ok &= m_beam.Build(kBeamVs, kBeamFs, "beam");
    ok &= m_dot.Build(kDotVs, kDotFs, "dot");
    const std::string blitExt = std::string("#extension GL_OES_EGL_image_external_essl3 : require\n") + kFoveateFs + kBlitExtFs;
    const std::string blitExtGamma = std::string("#extension GL_OES_EGL_image_external_essl3 : require\n") + kFoveateFs + kBlitExtGammaFs;
    // Optional: without it, upscaling just stays off.
    m_upscalerReady = m_blitExtGamma.Build(kBlitVs, blitExtGamma.c_str(), "blitExtGamma");
    m_upscalerReady = m_gsr.Build(kBlitVs, kGsrFs, "gsr") && m_upscalerReady;
    m_upscalerReady = m_kept.Build(kBlitVs, kKeptFs, "kept") && m_upscalerReady;
    m_upscalerReady = m_keptMoved.Build(kBlitVs, kKeptMovedFs, "keptMoved") && m_upscalerReady;
    ok &= m_blitExt.Build(kBlitVs, blitExt.c_str(), "blitExt");
    ok &= m_blit2D.Build(kBlitVs, kBlit2DFs, "blit2d");

    // Grid: (segments + 1) x 2 vertices as a triangle strip.
    std::vector<float> verts;
    for (int i = 0; i <= m_gridSegments; i++) {
        const float u = float(i) / m_gridSegments;
        verts.insert(verts.end(), {u, 0.f, u, 1.f});
    }
    glGenVertexArrays(1, &m_gridVao);
    glGenBuffers(1, &m_gridVbo);
    glBindVertexArray(m_gridVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    return ok;
}

void SceneRenderer::Panel(const PanelDraw& p) {
    if (p.opacity <= 0.001f && !p.holePunch) return;
    const Program& prog = p.source == PanelSource::External ? m_panelExt : m_panel2D;
    prog.Use();
    const Mat4 mvp = m_viewProj * Mat4::FromPose(p.pose);
    glUniformMatrix4fv(prog.Uniform("uMvp"), 1, GL_FALSE, mvp.m);
    glUniform2f(prog.Uniform("uSize"), p.width, p.height);
    glUniform1f(prog.Uniform("uRadius"), p.radiusM);
    glUniform1f(prog.Uniform("uOpacity"), p.opacity);
    glUniform1f(prog.Uniform("uBrightness"), p.brightness);
    glUniform1f(prog.Uniform("uCorner"), p.cornerRadius);
    glUniform1i(prog.Uniform("uHole"), !p.holePunch ? 0 : p.holeFromAlpha ? 2 : 1);
    if (p.source == PanelSource::External) glUniform4fv(prog.Uniform("uCrop"), 1, p.crop);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(p.source == PanelSource::External ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D, p.texture);
    glUniform1i(prog.Uniform("uTex"), 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (p.holePunch) glBlendFunc(p.holeFromAlpha ? GL_ZERO : GL_ONE, p.holeFromAlpha ? GL_ONE_MINUS_SRC_ALPHA : GL_ZERO);
    // Panels hide each other per pixel, whatever order they come in. One fading in or out
    // doesn't hide what's drawn after it.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(p.opacity >= 0.98f || p.holePunch ? GL_TRUE : GL_FALSE);
    glBindVertexArray(m_gridVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, (m_gridSegments + 1) * 2);
    glBindVertexArray(0);
}

void SceneRenderer::Beam(const Vec3& from, const Vec3& to, float width, const Color& start, const Color& end) {
    const Vec3 dir = Normalize(to - from);
    const Vec3 toEye = Normalize(m_eye - (from + to) * 0.5f);
    const Vec3 side = Normalize(Cross(dir, toEye)) * (width * 0.5f);
    const float verts[] = {
        from.x - side.x, from.y - side.y, from.z - side.z, 0, -1,
        from.x + side.x, from.y + side.y, from.z + side.z, 0, 1,
        to.x - side.x,   to.y - side.y,   to.z - side.z,   1, -1,
        to.x + side.x,   to.y + side.y,   to.z + side.z,   1, 1,
    };
    static GLuint vao = 0, vbo = 0;
    if (!vao) {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 5 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(4 * sizeof(float)));
    }
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
    m_beam.Use();
    glUniformMatrix4fv(m_beam.Uniform("uViewProj"), 1, GL_FALSE, m_viewProj.m);
    glUniform4f(m_beam.Uniform("uStart"), start.r, start.g, start.b, start.a);
    glUniform4f(m_beam.Uniform("uEnd"), end.r, end.g, end.b, end.a);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void SceneRenderer::Dot(const Vec3& position, const Vec3& normal, float radius, const Color& c) {
    const Vec3 n = Normalize(normal);
    const Vec3 ref = std::fabs(n.y) > 0.9f ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
    const Vec3 right = Normalize(Cross(ref, n)) * radius;
    const Vec3 up = Normalize(Cross(n, right)) * radius;
    m_dot.Use();
    glUniformMatrix4fv(m_dot.Uniform("uViewProj"), 1, GL_FALSE, m_viewProj.m);
    const Vec3 center = position + n * 0.002f;
    glUniform3f(m_dot.Uniform("uCenter"), center.x, center.y, center.z);
    glUniform3f(m_dot.Uniform("uRight"), right.x, right.y, right.z);
    glUniform3f(m_dot.Uniform("uUp"), up.x, up.y, up.z);
    glUniform4f(m_dot.Uniform("uColor"), c.r, c.g, c.b, c.a);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void SceneRenderer::ControllerOrb(const Pose& aim, const Color& c) {
    const Vec3 toEye = Normalize(m_eye - aim.position);
    Dot(aim.position, toEye, 0.012f, c);
}

// The squeeze of a foveated VR frame, or none.
static void SetFoveation(const Program& p, const VideoFoveation* f) {
    glUniform1i(p.Uniform("uFoveated"), f ? 1 : 0);
    if (!f) return;
    glUniform4f(p.Uniform("uFoveaStart"), f->start[0][0], f->start[0][1], f->start[1][0], f->start[1][1]);
    glUniform3f(p.Uniform("uFoveaLayout"), f->size, f->density[0], f->density[1]);
}

void SceneRenderer::BlitExternal(GLuint texture, int width, int height, const float* crop, float u0, float v0, float u1,
                                 float v1, const VideoFoveation* foveation) {
    static const float kFull[4] = {0, 0, 1, 1};
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    m_blitExt.Use();
    glUniform4f(m_blitExt.Uniform("uRect"), u0, v0, u1, v1);
    glUniform4fv(m_blitExt.Uniform("uCrop"), 1, crop ? crop : kFull);
    SetFoveation(m_blitExt, foveation);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glUniform1i(m_blitExt.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SceneRenderer::KeepVideo(GLuint texture, const float* crop, int width, int height, const VideoFoveation* foveation) {
    static const float kFull[4] = {0, 0, 1, 1};
    if (width != m_gsrWidth || height != m_gsrHeight) {
        if (m_gsrFbo) glDeleteFramebuffers(1, &m_gsrFbo);
        if (m_gsrTexture) glDeleteTextures(1, &m_gsrTexture);
        glGenTextures(1, &m_gsrTexture);
        glBindTexture(GL_TEXTURE_2D, m_gsrTexture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &m_gsrFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_gsrFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gsrTexture, 0);
        m_gsrWidth = width;
        m_gsrHeight = height;
    }
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(EmptyVao());
    glBindFramebuffer(GL_FRAMEBUFFER, m_gsrFbo);
    glViewport(0, 0, width, height);
    m_blitExtGamma.Use();
    glUniform4f(m_blitExtGamma.Uniform("uRect"), 0, 0, 1, 1);
    glUniform4fv(m_blitExtGamma.Uniform("uCrop"), 1, crop ? crop : kFull);
    SetFoveation(m_blitExtGamma, foveation);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glUniform1i(m_blitExtGamma.Uniform("uTex"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SceneRenderer::DrawKeptUpscaled(GLuint dstFbo, int dstW, int dstH) {
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, dstW, dstH);
    m_gsr.Use();
    glUniform4f(m_gsr.Uniform("uRect"), 0, 0, 1, 1);
    glUniform4f(m_gsr.Uniform("uViewport"), 1.f / m_gsrWidth, 1.f / m_gsrHeight, float(m_gsrWidth), float(m_gsrHeight));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gsrTexture);
    glUniform1i(m_gsr.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SceneRenderer::DrawKept(GLuint dstFbo, int dstW, int dstH) {
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, dstW, dstH);
    m_kept.Use();
    glUniform4f(m_kept.Uniform("uRect"), 0, 0, 1, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gsrTexture);
    glUniform1i(m_kept.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void SceneRenderer::DrawKeptMoved(GLuint dstFbo, int dstW, int dstH, float scaleX, float scaleY, float fieldU, float fieldV) {
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, dstW, dstH);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    m_keptMoved.Use();
    glUniform4f(m_keptMoved.Uniform("uRect"), 0, 0, 1, 1);
    glUniform2f(m_keptMoved.Uniform("uScale"), scaleX, scaleY);
    glUniform2f(m_keptMoved.Uniform("uField"), fieldU, fieldV);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gsrTexture);
    glUniform1i(m_keptMoved.Uniform("uTex"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_motionTexture);
    glUniform1i(m_keptMoved.Uniform("uMotion"), 1);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE0);
}

void SceneRenderer::UploadMotion(const int8_t* grid, int width, int height) {
    if (width != m_motionWidth || height != m_motionHeight) {
        if (m_motionTexture) glDeleteTextures(1, &m_motionTexture);
        glGenTextures(1, &m_motionTexture);
        glBindTexture(GL_TEXTURE_2D, m_motionTexture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG8_SNORM, width, height);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_motionWidth = width;
        m_motionHeight = height;
    }
    glBindTexture(GL_TEXTURE_2D, m_motionTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RG, GL_BYTE, grid);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
}

// The same, faded: premultiplied canvas times `alpha` over cleared black, for a window that is
// a compositor layer while it fades in or out.
void SceneRenderer::Blit2DAlpha(GLuint texture, int width, int height, float alpha) {
    glViewport(0, 0, width, height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendColor(0, 0, 0, alpha);
    glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
    m_blit2D.Use();
    glUniform4f(m_blit2D.Uniform("uRect"), 0, 0, 1, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(m_blit2D.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

void SceneRenderer::Blit2D(GLuint texture, int width, int height, float u0, float v0, float u1, float v1) {
    glViewport(0, 0, width, height);
    glDisable(GL_BLEND);
    m_blit2D.Use();
    glUniform4f(m_blit2D.Uniform("uRect"), u0, v0, u1, v1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(m_blit2D.Uniform("uTex"), 0);
    glBindVertexArray(EmptyVao());
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace rv
