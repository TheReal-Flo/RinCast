// Bakes signed-distance-field atlases for the headset UI:
//   ui/text.png  + ui/text.json    Inter (Medium + SemiBold), Latin-1 and UI punctuation
//   ui/icons.png + ui/icons.json   Material Symbols Rounded icons used by the app
//
//   assetbaker <fonts dir> <material symbols ttf> <codepoints file> <out dir>
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

static Glyph Bake(const stbtt_fontinfo& font, int codepoint, float pixelHeight, int padding, int key) {
    Glyph g{};
    g.key = key;
    const float scale = stbtt_ScaleForPixelHeight(&font, pixelHeight);
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

static void WriteAtlas(std::vector<Glyph>& glyphs, const std::string& png, int size) {
    std::vector<Glyph*> ptrs;
    for (auto& g : glyphs)
        if (!g.sdf.empty()) ptrs.push_back(&g);
    while (!Pack(ptrs, size, size)) size *= 2;
    std::vector<unsigned char> pixels(size_t(size) * size, 0);
    for (Glyph* g : ptrs)
        for (int y = 0; y < g->h; y++)
            std::copy_n(&g->sdf[size_t(y) * g->w], g->w, &pixels[size_t(g->y + y) * size + g->x]);
    stbi_write_png(png.c_str(), size, size, 1, pixels.data(), size);
    std::printf("%s: %dx%d, %zu glyphs\n", png.c_str(), size, size, ptrs.size());
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::printf("usage: assetbaker <inter ttf dir> <MaterialSymbols.ttf> <codepoints> <out dir>\n");
        return 1;
    }
    const std::string fontsDir = argv[1], iconFont = argv[2], codepointsPath = argv[3], outDir = argv[4];
    constexpr float kTextPx = 56.f;
    constexpr int kPad = 8;

    // ---- text ----
    const char* faces[] = {"Inter-Medium.ttf", "Inter-SemiBold.ttf"};
    std::vector<int> codepoints;
    for (int c = 32; c < 127; c++) codepoints.push_back(c);
    for (int c = 160; c < 256; c++) codepoints.push_back(c);
    for (int c : {0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2026, 0x2190, 0x2191, 0x2192, 0x2193,
                  0x00D7, 0x2212, 0x2122, 0x20AC})
        codepoints.push_back(c);

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
