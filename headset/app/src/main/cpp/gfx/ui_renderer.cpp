#include "ui_renderer.h"

#include <cmath>
#include <cstddef>

#include "../platform/assets.h"
#include "nlohmann/json.hpp"
#include "rc/platform.h"

namespace rv {

Color Mix(const Color& a, const Color& b, float t) {
    return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t), Lerp(a.a, b.a, t)};
}

uint32_t NextCodepoint(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> extra);
    while (extra-- > 0 && i < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    return cp;
}

// ---------------------------------------------------------------------------
// Font
// ---------------------------------------------------------------------------

bool Font::Load(Assets& assets) {
    std::vector<uint8_t> png;
    if (!assets.Read("ui/text.png", png) || !LoadImageTexture(png, 1, false, true, m_text)) return false;
    if (!assets.Read("ui/icons.png", png) || !LoadImageTexture(png, 1, false, true, m_icons)) return false;
    try {
        const auto text = nlohmann::json::parse(assets.ReadText("ui/text.json"));
        m_size = text.value("size", 56.f);
        m_padding = text.value("padding", 8.f);
        for (int f = 0; f < 2; f++) {
            const auto& face = text["faces"][f];
            m_ascent[f] = face.value("ascent", m_size * 0.8f);
            m_descent[f] = face.value("descent", -m_size * 0.2f);
            for (auto& [k, v] : face["kerning"].items()) m_kerning[f][uint32_t(std::stoul(k))] = v.get<float>();
        }
        for (auto& [k, v] : text["glyphs"].items()) {
            const uint32_t key = uint32_t(std::stoul(k));
            const int face = key >= 0x110000 ? 1 : 0;
            m_glyphs[face][key % 0x110000] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
        }
        // The keyboard's face, when it's there (a missing one falls back to Medium).
        if (assets.Read("keyboard/text.png", png) && LoadImageTexture(png, 1, false, true, m_keys)) {
            const auto keys = nlohmann::json::parse(assets.ReadText("keyboard/text.json"));
            m_keysSize = keys.value("size", 56.f);
            m_ascent[2] = keys.value("ascent", m_keysSize);
            m_descent[2] = keys.value("descent", -m_keysSize * 0.3f);
            for (auto& [k, v] : keys["glyphs"].items())
                m_glyphs[2][uint32_t(std::stoul(k))] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
        }
        const auto icons = nlohmann::json::parse(assets.ReadText("ui/icons.json"));
        m_iconSize = icons.value("size", 64.f);
        for (auto& [k, v] : icons["icons"].items()) m_iconMap[k] = {v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
    } catch (const std::exception& e) {
        RC_LOGE("Font metrics: %s", e.what());
        return false;
    }
    return true;
}

const GlyphInfo* Font::Glyph(uint32_t cp, Face face) const {
    if (face == Face::Keys && m_glyphs[2].empty()) face = Face::Medium;
    const auto& map = m_glyphs[int(face)];
    auto it = map.find(cp);
    if (it == map.end() && face == Face::Keys) return nullptr;  // (variation selectors and joiners: nothing)
    if (it == map.end()) it = map.find('?');
    return it == map.end() ? nullptr : &it->second;
}

const GlyphInfo* Font::Icon(const std::string& name) const {
    auto it = m_iconMap.find(name);
    return it == m_iconMap.end() ? nullptr : &it->second;
}

float Font::Kerning(uint32_t a, uint32_t b, Face face) const {
    if (a > 0xFFFF || b > 0xFFFF) return 0;
    const auto& map = m_kerning[int(face)];
    auto it = map.find(a * 65536 + b);
    return it == map.end() ? 0 : it->second;
}

float Font::Ascent(float size, Face face) const {
    if (face == Face::Keys && m_glyphs[2].empty()) face = Face::Medium;
    return m_ascent[int(face)] * size / BakeSize(face);
}

float Font::Descent(float size, Face face) const {
    if (face == Face::Keys && m_glyphs[2].empty()) face = Face::Medium;
    return m_descent[int(face)] * size / BakeSize(face);
}

float Font::Measure(const std::string& text, float size, Face face) const {
    const float scale = size / BakeSize(face);
    float w = 0;
    uint32_t prev = 0;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = NextCodepoint(text, i);
        if (const GlyphInfo* g = Glyph(cp, face)) w += (g->advance + (prev ? Kerning(prev, cp, face) : 0)) * scale;
        prev = cp;
    }
    return w;
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

static const char* kVs = R"(
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec2 aLocal;
layout(location = 3) in vec2 aSize;
layout(location = 4) in vec4 aParams;   // radius, border, kind, extra
layout(location = 5) in vec4 aC0;
layout(location = 6) in vec4 aC1;
layout(location = 7) in vec2 aArc;
uniform vec2 uViewport;
out vec2 vUv; out vec2 vLocal; flat out vec2 vSize; flat out vec4 vParams; out vec4 vC0; out vec4 vC1; flat out vec2 vArc;
void main() {
    vUv = aUv; vLocal = aLocal; vSize = aSize; vParams = aParams; vC0 = aC0; vC1 = aC1; vArc = aArc;
    gl_Position = vec4(aPos.x / uViewport.x * 2.0 - 1.0, 1.0 - aPos.y / uViewport.y * 2.0, 0.0, 1.0);
}
)";

static const char* kFs = R"(
uniform sampler2D uTex;
in vec2 vUv; in vec2 vLocal; flat in vec2 vSize; flat in vec4 vParams; in vec4 vC0; in vec4 vC1; flat in vec2 vArc;
out vec4 oColor;
float sdRoundBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    float radius = vParams.x, border = vParams.y, kind = vParams.z, extra = vParams.w;
    vec2 p = vLocal - vSize * 0.5;
    float d = sdRoundBox(p, vSize * 0.5, min(radius, min(vSize.x, vSize.y) * 0.5));
    float aa = max(fwidth(d), 0.0001) * 0.85;
    vec4 col;
    if (kind < 0.5) {                    // shape
        vec4 fill = extra > 0.5 ? mix(vC0, vC1, clamp(vLocal.y / vSize.y, 0.0, 1.0)) : vC0;
        float a = clamp(0.5 - d / aa, 0.0, 1.0);
        if (border > 0.0) {
            float inner = clamp(0.5 - (d + border) / aa, 0.0, 1.0);
            col = mix(vC1, fill, inner);
        } else {
            col = fill;
        }
        col.a *= a;
    } else if (kind < 2.5) {             // text / icon (SDF)
        float s = texture(uTex, vUv).r;
        float w = max(fwidth(s), 0.0001) * 0.75;
        float a = smoothstep(0.5 - w, 0.5 + w, s);
        col = vec4(vC0.rgb, vC0.a * a);
    } else if (kind < 3.5) {             // image with rounded corners
        vec4 t = texture(uTex, vUv) * vC0;
        float a = clamp(0.5 - d / aa, 0.0, 1.0);
        col = vec4(t.rgb, t.a * a);
    } else if (kind < 4.5) {             // soft shadow
        float a = 1.0 - smoothstep(-extra * 0.5, extra, d);
        col = vec4(vC0.rgb, vC0.a * a * a);
    } else {                             // arc (spinner)
        float r = length(p);
        float ring = clamp(0.5 - (abs(r - (vSize.x * 0.5 - extra * 0.5)) - extra * 0.5) / max(fwidth(r), 0.0001), 0.0, 1.0);
        float ang = atan(p.y, p.x);
        float rel = mod(ang - vArc.x, 6.2831853);
        float inArc = rel <= vArc.y ? 1.0 : 0.0;
        float fade = rel / max(vArc.y, 0.001);
        col = vec4(vC0.rgb, vC0.a * ring * inArc * mix(0.15, 1.0, fade));
    }
    oColor = vec4(col.rgb * col.a, col.a);   // premultiplied
}
)";

bool UiRenderer::Init(Assets& assets) {
    if (!m_font.Load(assets)) {
        RC_LOGE("UI font atlases missing");
        return false;
    }
    if (!m_program.Build(kVs, kFs, "ui")) return false;
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    const GLsizei stride = sizeof(Vertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, lx));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, sw));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, radius));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, c0));
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, c1));
    glEnableVertexAttribArray(7);
    glVertexAttribPointer(7, 2, GL_FLOAT, GL_FALSE, stride, (void*)offsetof(Vertex, arc));
    glBindVertexArray(0);
    return true;
}

void UiRenderer::Begin(int width, int height, float scale) {
    m_scale = scale;
    m_width = width;
    m_height = height;
    m_vertices.clear();
    m_texture = 0;
    m_clips.clear();
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void UiRenderer::End() {
    Flush();
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
}

void UiRenderer::SetTexture(GLuint tex) {
    if (tex == m_texture) return;
    Flush();
    m_texture = tex;
}

void UiRenderer::Flush() {
    if (m_vertices.empty()) return;
    m_program.Use();
    glUniform2f(m_program.Uniform("uViewport"), float(m_width), float(m_height));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_texture ? m_texture : m_font.TextAtlas().id);
    glUniform1i(m_program.Uniform("uTex"), 0);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m_vertices.size() * sizeof(Vertex)), m_vertices.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(m_vertices.size()));
    glBindVertexArray(0);
    m_vertices.clear();
}

// UI colours are authored in sRGB; panels render into sRGB targets (linear blending).
static void ToLinear(const Color& c, float out[4]) {
    auto f = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    out[0] = f(c.r);
    out[1] = f(c.g);
    out[2] = f(c.b);
    out[3] = c.a;
}

void UiRenderer::Quad(const Rect& lr, float u0, float v0, float u1, float v1, float kind, const Color& c0,
                      const Color& c1, float radius, float border, float extra, float arcStart, float arcSweep) {
    if (lr.w <= 0 || lr.h <= 0 || (c0.a <= 0.001f && c1.a <= 0.001f)) return;
    // Logical px -> target px.
    const float s = m_scale;
    const Rect r{lr.x * s, lr.y * s, lr.w * s, lr.h * s};
    radius *= s;
    border *= s;
    if (kind > 3.5f) extra *= s;  // shadow blur / arc thickness are lengths
    Vertex base{};
    base.sw = r.w;
    base.sh = r.h;
    base.radius = radius;
    base.border = border;
    base.kind = kind;
    base.extra = extra;
    ToLinear(c0, base.c0);
    ToLinear(c1, base.c1);
    base.arc[0] = arcStart;
    base.arc[1] = arcSweep;
    auto vtx = [&](float fx, float fy) {
        Vertex v = base;
        v.x = r.x + fx * r.w;
        v.y = r.y + fy * r.h;
        v.u = Lerp(u0, u1, fx);
        v.v = Lerp(v0, v1, fy);
        v.lx = fx * r.w;
        v.ly = fy * r.h;
        return v;
    };
    const Vertex a = vtx(0, 0), b = vtx(1, 0), c = vtx(0, 1), d = vtx(1, 1);
    m_vertices.insert(m_vertices.end(), {a, b, c, b, d, c});
}

void UiRenderer::RectFill(const Rect& r, const Color& c, float radius) {
    SetTexture(m_texture);
    Quad(r, 0, 0, 0, 0, 0, c, c, radius, 0, 0);
}

void UiRenderer::RectBorder(const Rect& r, const Color& fill, float radius, const Color& border, float borderWidth) {
    Quad(r, 0, 0, 0, 0, 0, fill, border, radius, borderWidth, 0);
}

void UiRenderer::Gradient(const Rect& r, const Color& top, const Color& bottom, float radius) {
    Quad(r, 0, 0, 0, 0, 0, top, bottom, radius, 0, 1);
}

void UiRenderer::Shadow(const Rect& r, float radius, float blur, const Color& c) {
    Quad({r.x - blur, r.y - blur, r.w + blur * 2, r.h + blur * 2}, 0, 0, 0, 0, 4, c, c, radius + blur, 0, blur);
}

void UiRenderer::Arc(float cx, float cy, float radius, float thickness, float start, float sweep, const Color& c) {
    Quad({cx - radius, cy - radius, radius * 2, radius * 2}, 0, 0, 0, 0, 5, c, c, radius, 0, thickness, start, sweep);
}

void UiRenderer::Text(const std::string& text, float x, float y, float size, const Color& c, Face face, Align align) {
    SetTexture(m_font.TextAtlas(face).id);
    const float scale = size / m_font.BakeSize(face);
    if (align != Align::Left) {
        const float w = m_font.Measure(text, size, face);
        x -= align == Align::Center ? w / 2 : w;
    }
    const float baseline = y + m_font.Ascent(size, face);
    const float aw = float(m_font.TextAtlas(face).width), ah = float(m_font.TextAtlas(face).height);
    float pen = x;
    uint32_t prev = 0;
    for (size_t i = 0; i < text.size();) {
        const uint32_t cp = NextCodepoint(text, i);
        const GlyphInfo* g = m_font.Glyph(cp, face);
        if (!g) continue;
        if (prev) pen += m_font.Kerning(prev, cp, face) * scale;
        if (g->w > 0) {
            const Rect gr{pen + g->xoff * scale, baseline + g->yoff * scale, g->w * scale, g->h * scale};
            Quad(gr, g->x / aw, g->y / ah, (g->x + g->w) / aw, (g->y + g->h) / ah, 1, c, c, 0, 0, 0);
        }
        pen += g->advance * scale;
        prev = cp;
    }
}

std::string UiRenderer::Ellipsize(const std::string& text, float maxWidth, float size, Face face) const {
    if (m_font.Measure(text, size, face) <= maxWidth) return text;
    std::string s = text;
    while (!s.empty()) {
        // Drop one UTF-8 code point from the end.
        size_t cut = s.size() - 1;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
        s.resize(cut);
        if (m_font.Measure(s + "…", size, face) <= maxWidth) return s + "…";
    }
    return "…";
}

void UiRenderer::TextIn(const std::string& text, const Rect& r, float size, const Color& c, Face face, Align align) {
    const std::string s = Ellipsize(text, r.w, size, face);
    const float x = align == Align::Left ? r.x : align == Align::Center ? r.x + r.w / 2 : r.Right();
    const float y = r.y + r.h / 2 - m_font.Ascent(size, face) + size * 0.36f;
    Text(s, x, y, size, c, face, align);
}

float UiRenderer::WrapText(const std::string& text, const Rect& r, float size, const Color& c, Face face,
                           float lineHeight, Align align) {
    float y = r.y;
    std::string line;
    size_t i = 0;
    auto flush = [&] {
        const float x = align == Align::Left ? r.x : align == Align::Center ? r.x + r.w / 2 : r.Right();
        Text(line, x, y, size, c, face, align);
        y += lineHeight;
        line.clear();
    };
    // Chinese and Japanese have no spaces: any CJK character may end a line. One such
    // character counts as a word of its own, joined without a space. A Latin word that
    // directly follows CJK still gets its natural space, which reads right in both.
    auto isCjk = [](uint32_t cp) {
        return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
               (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFD);
    };
    while (i < text.size()) {
        if (text[i] == '\n') {
            flush();
            i++;
            continue;
        }
        // One word: up to the next space or newline, or a single CJK character.
        const size_t start = i;
        const uint32_t cp = NextCodepoint(text, i);
        const bool cjk = isCjk(cp);
        size_t end = i;
        if (!cjk) {
            end = text.find_first_of(" \n", i);
            if (end == std::string::npos) end = text.size();
            i = end;
        }
        const std::string word = text.substr(start, end - start);
        const std::string candidate = (cjk || line.empty()) ? line + word : line + " " + word;
        if (!line.empty() && m_font.Measure(candidate, size, face) > r.w) {
            flush();
            line = word;
        } else {
            line = candidate;
        }
        if (i < text.size() && text[i] == ' ') i++;
    }
    if (!line.empty()) flush();
    return y - r.y;
}

void UiRenderer::Icon(const std::string& name, float cx, float cy, float size, const Color& c) {
    const GlyphInfo* g = m_font.Icon(name);
    if (!g) return;
    SetTexture(m_font.IconAtlas().id);
    const float scale = size / m_font.IconBakeSize();
    const float aw = float(m_font.IconAtlas().width), ah = float(m_font.IconAtlas().height);
    // Icons are square glyphs; centre the SDF bitmap (which includes padding).
    const float w = g->w * scale, h = g->h * scale;
    Quad({cx - w / 2, cy - h / 2, w, h}, g->x / aw, g->y / ah, (g->x + g->w) / aw, (g->y + g->h) / ah, 2, c, c, 0, 0,
         0);
}

void UiRenderer::Image(GLuint texture, const Rect& r, float radius, const Color& tint) {
    SetTexture(texture);
    Quad(r, 0, 0, 1, 1, 3, tint, tint, radius, 0, 0);
}

void UiRenderer::ImageRegion(GLuint texture, const Rect& r, float u0, float v0, float u1, float v1, const Color& tint) {
    SetTexture(texture);
    Quad(r, u0, v0, u1, v1, 3, tint, tint, 0, 0, 0);
}

void UiRenderer::PushClip(const Rect& r) {
    Flush();
    Rect c = r;
    if (!m_clips.empty()) {
        const Rect& p = m_clips.back();
        const float x0 = std::max(c.x, p.x), y0 = std::max(c.y, p.y);
        const float x1 = std::min(c.Right(), p.Right()), y1 = std::min(c.Bottom(), p.Bottom());
        c = {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
    m_clips.push_back(c);
    glEnable(GL_SCISSOR_TEST);
    const float s = m_scale;
    glScissor(GLint(c.x * s), GLint(m_height - c.Bottom() * s), GLsizei(c.w * s), GLsizei(c.h * s));
}

void UiRenderer::PopClip() {
    Flush();
    m_clips.pop_back();
    if (m_clips.empty()) {
        glDisable(GL_SCISSOR_TEST);
    } else {
        const Rect& c = m_clips.back();
        const float s = m_scale;
        glScissor(GLint(c.x * s), GLint(m_height - c.Bottom() * s), GLsizei(c.w * s), GLsizei(c.h * s));
    }
}

}  // namespace rv
