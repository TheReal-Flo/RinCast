// Bakes signed-distance-field atlases for the headset UI:
//   ui/text.png  + ui/text.json    Inter (Medium + SemiBold), Latin-1 and UI punctuation,
//                                  plus the characters of a charset file from Noto Sans SC
//   ui/icons.png + ui/icons.json   Material Symbols Rounded icons used by the app
//
//   assetbaker <fonts dir> <material symbols ttf> <codepoints file> <out dir> [charset file]
//
// And one face on its own, for the keyboard (assets/keyboard/text.*):
//
//   assetbaker --face <out dir> <charset file> <font> [fallback font...]
//
// It's measured like Pillow measures text, which the keyboard it's a port of was drawn with: sizes
// are em sizes, and there's no kerning. Each character comes from the first font that has it.
//
// The optional charset file is UTF-8 text; every character in it that Inter doesn't
// already cover is baked from NotoSansSC-{Medium,SemiBold}.ttf in the fonts dir. It holds
// the non-ASCII characters the UI's translations use (app/strings.cpp), so Chinese and
// Japanese render while the atlas stays small.
//
// SDF glyphs stay sharp at any distance in VR; the shader reconstructs edges
// with screen-space derivatives.
#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "stb/stb_image_write.h"
#include "stb/stb_truetype.h"

using nlohmann::json;

static std::vector<unsigned char> ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f), {});
}

// Decodes the next UTF-8 code point, advancing `i` past it. Lenient: bad bytes
// count as one code point each, which the font simply won't have.
static uint32_t NextCodepoint(const std::string& s, size_t& i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    auto cont = [&](size_t k) { return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80; };
    i++;
    if (b0 < 0x80) return b0;
    if ((b0 & 0xE0) == 0xC0 && cont(i)) {
        const uint32_t cp = ((b0 & 0x1Fu) << 6) | (s[i] & 0x3Fu);
        i++;
        return cp >= 0x80 ? cp : b0;
    }
    if ((b0 & 0xF0) == 0xE0 && cont(i) && cont(i + 1)) {
        const uint32_t cp = ((b0 & 0x0Fu) << 12) | ((s[i] & 0x3Fu) << 6) | (s[i + 1] & 0x3Fu);
        i += 2;
        return cp >= 0x800 ? cp : b0;
    }
    if ((b0 & 0xF8) == 0xF0 && cont(i) && cont(i + 1) && cont(i + 2)) {
        const uint32_t cp = ((b0 & 0x07u) << 18) | ((s[i] & 0x3Fu) << 12) | ((s[i + 1] & 0x3Fu) << 6) | (s[i + 2] & 0x3Fu);
        i += 3;
        return cp >= 0x10000 ? cp : b0;
    }
    return b0;
}

struct Glyph {
    int key;        // codepoint (+ face offset)
    int w, h;       // bitmap size
    int xoff, yoff; // bitmap offset from pen position (pixels at bake size)
    float advance;
    std::vector<unsigned char> sdf;
    int x = 0, y = 0;  // atlas placement
};

struct Atlas {
    int width, height;
    std::vector<unsigned char> pixels;
};

// Simple shelf packer.
static bool Pack(std::vector<Glyph*>& glyphs, int width, int height) {
    std::sort(glyphs.begin(), glyphs.end(), [](Glyph* a, Glyph* b) { return a->h > b->h; });
    int x = 1, y = 1, shelf = 0;
    for (Glyph* g : glyphs) {
        if (x + g->w + 1 > width) {
            x = 1;
            y += shelf + 1;
            shelf = 0;
        }
        if (y + g->h + 1 > height) return false;
        g->x = x;
        g->y = y;
        x += g->w + 1;
        shelf = std::max(shelf, g->h);
    }
    return true;
}

static Glyph BakeScaled(const stbtt_fontinfo& font, int codepoint, float scale, int padding, int key) {
    Glyph g{};
    g.key = key;
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font, codepoint, &advance, &lsb);
    g.advance = advance * scale;
    unsigned char* sdf = stbtt_GetCodepointSDF(&font, scale, codepoint, padding, 128, 128.f / padding, &g.w, &g.h,
                                               &g.xoff, &g.yoff);
    if (sdf) {
        g.sdf.assign(sdf, sdf + g.w * g.h);
        stbtt_FreeSDF(sdf, nullptr);
    }
    return g;
}

static Glyph Bake(const stbtt_fontinfo& font, int codepoint, float pixelHeight, int padding, int key) {
    return BakeScaled(font, codepoint, stbtt_ScaleForPixelHeight(&font, pixelHeight), padding, key);
}

static void WriteAtlas(std::vector<Glyph>& glyphs, const std::string& png, int size) {
    std::vector<Glyph*> ptrs;
    for (auto& g : glyphs)
        if (!g.sdf.empty()) ptrs.push_back(&g);
    // Twice as tall before twice as wide: a square only just too small doesn't make four times the pixels.
    int w = size, h = size;
    while (!Pack(ptrs, w, h)) {
        if (h == w) h *= 2;
        else w *= 2;
    }
    std::vector<unsigned char> pixels(size_t(w) * h, 0);
    for (Glyph* g : ptrs)
        for (int y = 0; y < g->h; y++)
            std::copy_n(&g->sdf[size_t(y) * g->w], g->w, &pixels[size_t(g->y + y) * w + g->x]);
    stbi_write_png(png.c_str(), w, h, 1, pixels.data(), w);
    std::printf("%s: %dx%d, %zu glyphs\n", png.c_str(), w, h, ptrs.size());
}

static int BakeFace(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: assetbaker --face <out dir> <charset file> <font> [fallback font...]\n");
        return 1;
    }
    const std::string outDir = argv[2];
    constexpr float kEm = 56.f;
    constexpr int kPad = 8;
    std::vector<std::vector<unsigned char>> data;
    std::vector<stbtt_fontinfo> fonts;
    for (int i = 4; i < argc; i++) data.push_back(ReadFile(argv[i]));
    for (auto& d : data) {
        stbtt_fontinfo info{};
        if (d.empty() || !stbtt_InitFont(&info, d.data(), stbtt_GetFontOffsetForIndex(d.data(), 0))) {
            std::printf("can't read a font\n");
            return 1;
        }
        fonts.push_back(info);
    }
    std::vector<int> codepoints;
    for (int c = 32; c < 127; c++) codepoints.push_back(c);
    const std::vector<unsigned char> raw = ReadFile(argv[3]);
    const std::string charset(raw.begin(), raw.end());
    for (size_t i = 0; i < charset.size();) {
        const int cp = int(NextCodepoint(charset, i));
        if (cp >= 32 && std::find(codepoints.begin(), codepoints.end(), cp) == codepoints.end()) codepoints.push_back(cp);
    }
    std::vector<Glyph> glyphs;
    std::string missing;
    for (int cp : codepoints) {
        size_t f = 0;
        while (f < fonts.size() && !stbtt_FindGlyphIndex(&fonts[f], cp)) f++;
        if (f == fonts.size()) {
            missing += " U+" + [&] { char b[8]; std::snprintf(b, sizeof b, "%04X", cp); return std::string(b); }();
            continue;
        }
        glyphs.push_back(BakeScaled(fonts[f], cp, stbtt_ScaleForMappingEmToPixels(&fonts[f], kEm), kPad, cp));
    }
    if (!missing.empty()) std::printf("not in any font:%s\n", missing.c_str());
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&fonts[0], &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForMappingEmToPixels(&fonts[0], kEm);
    WriteAtlas(glyphs, outDir + "/text.png", 1024);
    json out{{"size", kEm}, {"padding", kPad}, {"ascent", ascent * scale}, {"descent", descent * scale}};
    json map = json::object();
    for (const auto& g : glyphs) map[std::to_string(g.key)] = {g.x, g.y, g.w, g.h, g.xoff, g.yoff, g.advance};
    out["glyphs"] = map;
    std::ofstream(outDir + "/text.json") << out.dump();
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--face") return BakeFace(argc, argv);
    if (argc < 5) {
        std::printf("usage: assetbaker <inter ttf dir> <MaterialSymbols.ttf> <codepoints> <out dir> [charset file]\n");
        return 1;
    }
    const std::string fontsDir = argv[1], iconFont = argv[2], codepointsPath = argv[3], outDir = argv[4];
    const std::string charsetPath = argc > 5 ? argv[5] : "";
    constexpr float kTextPx = 56.f;
    constexpr int kPad = 8;

    // ---- text ----
    const char* faces[] = {"Inter-Medium.ttf", "Inter-SemiBold.ttf"};
    const char* cjkFaces[] = {"NotoSansSC-Medium.ttf", "NotoSansSC-SemiBold.ttf"};
    std::vector<int> codepoints;
    for (int c = 32; c < 127; c++) codepoints.push_back(c);
    for (int c = 160; c < 256; c++) codepoints.push_back(c);
    for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193,
                  0x00D7, 0x2212, 0x2122, 0x20AC})
        codepoints.push_back(c);

    // Characters of the translations that Inter doesn't cover, from the charset file.
    // One codepoint goes to exactly one font: the Inter list wins, the rest are CJK.
    std::string charset;
    if (!charsetPath.empty()) {
        const std::vector<unsigned char> raw = ReadFile(charsetPath);
        charset.assign(raw.begin(), raw.end());
    }
    std::vector<int> cjkCodepoints;
    if (!charset.empty()) {
        std::vector<int> extra;
        for (size_t i = 0; i < charset.size();) extra.push_back(NextCodepoint(charset, i));
        for (int cp : extra)
            if (std::find(codepoints.begin(), codepoints.end(), cp) == codepoints.end() &&
                std::find(cjkCodepoints.begin(), cjkCodepoints.end(), cp) == cjkCodepoints.end())
                cjkCodepoints.push_back(cp);
    }
    if (!cjkCodepoints.empty())
        std::printf("%zu characters beyond Inter, baked from %s\n", cjkCodepoints.size(), cjkFaces[0]);

    std::vector<Glyph> textGlyphs;
    json textJson{{"size", kTextPx}, {"padding", kPad}, {"faces", json::array()}};
    std::vector<std::vector<unsigned char>> fontData;
    for (int f = 0; f < 2; f++) {
        fontData.push_back(ReadFile(fontsDir + "/" + faces[f]));
        stbtt_fontinfo info;
        stbtt_InitFont(&info, fontData.back().data(), 0);
        const float scale = stbtt_ScaleForPixelHeight(&info, kTextPx);
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
        json face{{"name", faces[f]}, {"ascent", ascent * scale}, {"descent", descent * scale},
                  {"lineGap", gap * scale}, {"kerning", json::object()}};
        for (int c : codepoints) textGlyphs.push_back(Bake(info, c, kTextPx, kPad, f * 0x110000 + c));
        // Kerning for common pairs (ASCII letters/digits/punctuation).
        for (int a = 32; a < 127; a++)
            for (int b = 32; b < 127; b++) {
                const int k = stbtt_GetCodepointKernAdvance(&info, a, b);
                if (k) face["kerning"][std::to_string(a * 65536 + b)] = k * scale;
            }
        // The CJK glyphs of this face come from Noto Sans SC, without kerning.
        if (!cjkCodepoints.empty()) {
            const std::string cjkPath = fontsDir + "/" + cjkFaces[f];
            std::vector<unsigned char> cjkData = ReadFile(cjkPath);
            if (cjkData.empty()) {
                std::printf("missing %s\n", cjkPath.c_str());
                return 1;
            }
            stbtt_fontinfo cjk;
            stbtt_InitFont(&cjk, cjkData.data(), 0);
            for (int c : cjkCodepoints) textGlyphs.push_back(Bake(cjk, c, kTextPx, kPad, f * 0x110000 + c));
        }
        textJson["faces"].push_back(face);
    }
    WriteAtlas(textGlyphs, outDir + "/text.png", 1024);
    json glyphs = json::object();
    for (const auto& g : textGlyphs)
        glyphs[std::to_string(g.key)] = {g.x, g.y, g.w, g.h, g.xoff, g.yoff, g.advance};
    textJson["glyphs"] = glyphs;
    std::ofstream(outDir + "/text.json") << textJson.dump();

    // ---- icons ----
    std::map<std::string, int> codes;
    {
        std::ifstream f(codepointsPath);
        std::string name, hex;
        while (f >> name >> hex) codes[name] = std::stoi(hex, nullptr, 16);
    }
    const char* iconNames[] = {
        "computer", "desktop_windows", "settings", "sports_esports", "rocket_launch", "keyboard",
        "center_focus_strong", "logout", "power_settings_new", "wifi", "lock", "cloud", "person", "link",
        "refresh", "close", "check", "arrow_back", "add", "remove", "volume_up", "mic", "mic_off", "monitor",
        "view_in_ar", "tune", "speed", "visibility", "open_in_full", "drag_indicator", "landscape", "palette",
        "info", "error", "warning", "play_arrow", "stop", "expand_more", "chevron_right", "chevron_left", "search",
        "backspace", "keyboard_return", "arrow_upward", "arrow_downward", "arrow_forward", "space_bar", "keyboard_tab", "devices", "headset_mic",
        "graphic_eq", "timer", "network_check", "high_quality", "sync", "account_circle", "laptop_windows",
        "panorama_photosphere", "fullscreen", "push_pin", "bolt", "hourglass_top", "mouse", "apps", "home",
        "wifi_off", "cloud_off", "vpn_lock", "open_with", "screen_share", "auto_awesome", "dark_mode", "light_mode",
        "language"};
    auto iconData = ReadFile(iconFont);
    stbtt_fontinfo iconInfo;
    stbtt_InitFont(&iconInfo, iconData.data(), stbtt_GetFontOffsetForIndex(iconData.data(), 0));
    constexpr float kIconPx = 64.f;
    std::vector<Glyph> iconGlyphs;
    json iconJson{{"size", kIconPx}, {"padding", kPad}, {"icons", json::object()}};
    for (const char* name : iconNames) {
        auto it = codes.find(name);
        if (it == codes.end()) {
            std::printf("missing icon %s\n", name);
            continue;
        }
        iconGlyphs.push_back(Bake(iconInfo, it->second, kIconPx, kPad, it->second));
    }
    WriteAtlas(iconGlyphs, outDir + "/icons.png", 512);
    for (size_t i = 0, j = 0; i < std::size(iconNames); i++) {
        auto it = codes.find(iconNames[i]);
        if (it == codes.end()) continue;
        for (const auto& g : iconGlyphs)
            if (g.key == it->second) iconJson["icons"][iconNames[i]] = {g.x, g.y, g.w, g.h, g.xoff, g.yoff, g.advance};
        (void)j;
    }
    std::ofstream(outDir + "/icons.json") << iconJson.dump();
    return 0;
}
