// Batched 2D renderer for UI panels (rendered into per-window textures).
// Coordinates are panel pixels, origin top-left.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "gl.h"
#include "math.h"

namespace rv {

class Assets;

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
    static constexpr Color Hex(uint32_t rgb, float a = 1) {
        return {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a};
    }
    Color WithAlpha(float na) const { return {r, g, b, a * na}; }
};
Color Mix(const Color& a, const Color& b, float t);

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(Vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
    Rect Inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect Inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
};

enum class Face { Medium = 0, SemiBold = 1 };
enum class Align { Left, Center, Right };

struct GlyphInfo {
    float x, y, w, h, xoff, yoff, advance;
};

class Font {
   public:
    bool Load(Assets& assets);
    float Measure(const std::string& text, float size, Face face) const;
    float Ascent(float size, Face face) const;
    const GlyphInfo* Glyph(uint32_t codepoint, Face face) const;
    const GlyphInfo* Icon(const std::string& name) const;
    float Kerning(uint32_t a, uint32_t b, Face face) const;
    float BakeSize() const { return m_size; }
    float IconBakeSize() const { return m_iconSize; }
    float Padding() const { return m_padding; }
    Texture& TextAtlas() { return m_text; }
    Texture& IconAtlas() { return m_icons; }

   private:
    Texture m_text, m_icons;
    float m_size = 56, m_iconSize = 64, m_padding = 8;
    float m_ascent[2] = {}, m_descent[2] = {};
    std::map<uint32_t, GlyphInfo> m_glyphs[2];
    std::map<uint32_t, float> m_kerning[2];
    std::map<std::string, GlyphInfo> m_iconMap;
};

// Decodes the next UTF-8 code point.
uint32_t NextCodepoint(const std::string& s, size_t& i);

class UiRenderer {
   public:
    bool Init(Assets& assets);
    Font& GetFont() { return m_font; }

    // `scale` maps logical px (what callers use) to target px (supersampling).
    void Begin(int width, int height, float scale = 1.f);
    void End();  // flushes

    void RectFill(const Rect& r, const Color& c, float radius = 0);
    void RectBorder(const Rect& r, const Color& fill, float radius, const Color& border, float borderWidth);
    void Shadow(const Rect& r, float radius, float blur, const Color& c);
    void Gradient(const Rect& r, const Color& top, const Color& bottom, float radius = 0);
    void Text(const std::string& text, float x, float y, float size, const Color& c, Face face = Face::Medium,
              Align align = Align::Left);
    // Draws text vertically centred in `r`, truncated with an ellipsis to fit.
    void TextIn(const std::string& text, const Rect& r, float size, const Color& c, Face face = Face::Medium,
                Align align = Align::Left);
    float WrapText(const std::string& text, const Rect& r, float size, const Color& c, Face face, float lineHeight,
                   Align align = Align::Left);
    void Icon(const std::string& name, float cx, float cy, float size, const Color& c);
    void Image(GLuint texture, const Rect& r, float radius, const Color& tint = {});
    void Circle(float cx, float cy, float radius, const Color& c) { RectFill({cx - radius, cy - radius, radius * 2, radius * 2}, c, radius); }
    void Arc(float cx, float cy, float radius, float thickness, float start, float sweep, const Color& c);

    void PushClip(const Rect& r);
    void PopClip();

    float Measure(const std::string& text, float size, Face face = Face::Medium) const {
        return m_font.Measure(text, size, face);
    }
    std::string Ellipsize(const std::string& text, float maxWidth, float size, Face face) const;

   private:
    struct Vertex {
        float x, y;        // panel px
        float u, v;        // atlas / image uv
        float lx, ly;      // local position within the shape
        float sw, sh;      // shape size
        float radius, border;
        float kind;        // 0 shape, 1 text, 2 icon, 3 image, 4 shadow, 5 arc
        float extra;       // shadow blur / sdf scale / arc thickness
        float c0[4];       // fill
        float c1[4];       // border / gradient bottom
        float arc[2];      // start, sweep
    };
    void Quad(const Rect& r, float u0, float v0, float u1, float v1, float kind, const Color& c0, const Color& c1,
              float radius, float border, float extra, float arcStart = 0, float arcSweep = 0);
    void Flush();
    void SetTexture(GLuint tex);

    Font m_font;
    Program m_program;
    GLuint m_vao = 0, m_vbo = 0;
    std::vector<Vertex> m_vertices;
    GLuint m_texture = 0;
    int m_width = 0, m_height = 0;
    float m_scale = 1.f;
    std::vector<Rect> m_clips;
};

}  // namespace rv
