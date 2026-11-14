// Renders the headset's windows to PNGs on Windows, with the app's own screens, widgets and
// renderer, through ANGLE. No headset, emulator or PC connection needed. See build.mjs.
//   ui-preview <assets dir> <out dir> [scene name]
#include <EGL/egl.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>

#include "app/app.h"
#include "app/strings.h"
#include "gfx/scene.h"
#include "platform/assets.h"
#include "platform/jni_bridge.h"
#include "rc/platform.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

namespace rc {
void Log(LogLevel, const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
}
}  // namespace rc

namespace rv {

namespace screens {
void Build(App& app, WindowManager& wm);
void SetPage(int page);
}

std::string g_assets;
Assets::Assets() {}  // files come from g_assets
bool Assets::Read(const char* path, std::vector<uint8_t>& out) const {
    std::ifstream f(g_assets + "/" + path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
std::string Assets::ReadText(const char* path) const {
    std::vector<uint8_t> b;
    Read(path, b);
    return std::string(b.begin(), b.end());
}

const std::vector<EnvironmentInfo>& Environments() {
    static const std::vector<EnvironmentInfo> list = {
        {Environment::SpaceViolet, "space-violet", "Violet Nebula"}, {Environment::SpaceBlue, "space-blue", "Blue Nebula"},
        {Environment::SpaceCyan, "space-cyan", "Cyan Nebula"},       {Environment::SpaceGreen, "space-green", "Green Nebula"},
        {Environment::SpacePink, "space-pink", "Pink Nebula"},       {Environment::SpaceRed, "space-red", "Red Nebula"},
        {Environment::DayClouds, "day-clouds", "Day Clouds"},        {Environment::Void, "void", "Void"},
        {Environment::Passthrough, "passthrough", "Passthrough"},
    };
    return list;
}

void SceneRenderer::Panel(const PanelDraw&) {}

const Texture& App::EnvironmentThumbnail(const std::string& key) {
    static std::map<std::string, Texture> cache;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    Texture& t = cache[key];
    std::vector<uint8_t> bytes;
    Assets assets;
    if (assets.Read(("ui/env-" + key + ".png").c_str(), bytes)) LoadImageTexture(bytes, 4, true, true, t);
    return t;
}

}  // namespace rv

using namespace rv;

namespace {

struct Scene {
    const char* name;
    const char* window;
    int page;
    std::function<void(App&)> setup;
    float hoverX = -1, hoverY = -1;
    bool click = false;  // press there and let go, then keep rendering
};

void Lan(App& a) {
    a.lan.pcs = {{"a", "Studio PC", "NVIDIA GeForce RTX 5070 Ti", false}, {"b", "Living room", "AMD Radeon RX 7800 XT", true}};
    a.account.pcs = {{"c", "Office PC", "", true, false}, {"d", "Old laptop", "", false, false}};
    a.settings.lastPc = "a";
}
void Live(App& a) {
    a.mode = Mode::Desktop;
    a.client.serverName = "Studio PC";
    a.stats.fps = 90;
    a.stats.latencyMs = 38;
    a.stats.mbps = 182;
    a.stats.decodeMs = 4.2;
    a.stats.rttMs = 3.4;
    a.stats.codec = "HEVC";
    a.serverInfo["vrBitrateMbps"] = 200;
    a.stats.width = 2496;
    a.stats.height = 1312;
    // A VR frame's way through the pipeline, adding up to the 38 ms above.
    const double stages[kStageCount] = {1.7, 11.4, 0.6, 4.1, -1, 5.3, 4.2, 2.9, 7.8};
    std::copy(std::begin(stages), std::end(stages), a.stats.stageMs);
    a.open = {"dock", "home"};
    a.serverInfo.update({{"pcName", "Studio PC"}, {"gpu", "NVIDIA GeForce RTX 5070 Ti"}, {"version", "1.0.0"}, {"os", "Windows 11"}});
}
const char* const kGameNames[] = {"Half-Life: Alyx", "Beat Saber", "Boneworks", "Pavlov", "Assetto Corsa Competizione", "VRChat", "Blade & Sorcery", "No Man's Sky", "Elite Dangerous", "Skyrim VR"};

GLuint Art(int seed) {
    // Stand-in for Steam's library art: a flat color field with a darker lower half.
    static const uint32_t hues[] = {0xC0392B, 0x2980B9, 0x8E44AD, 0x27AE60, 0xD35400, 0x16A085, 0x2C3E50, 0xF39C12, 0x7F8C8D, 0x34495E};
    const uint32_t h = hues[seed % 10];
    std::vector<uint8_t> px(64 * 96 * 4);
    for (int y = 0; y < 96; y++)
        for (int x = 0; x < 64; x++) {
            const float f = 1.f - 0.65f * float(y) / 96.f;
            uint8_t* p = &px[size_t(y * 64 + x) * 4];
            p[0] = uint8_t(((h >> 16) & 255) * f);
            p[1] = uint8_t(((h >> 8) & 255) * f);
            p[2] = uint8_t((h & 255) * f);
            p[3] = 255;
        }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 96, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    return t;
}

const std::vector<Scene>& Scenes() {
    static const std::vector<Scene> scenes = {
        {"computers", "home", 0, [](App& a) { Lan(a); }},
        {"computers-hover", "home", 0, [](App& a) { Lan(a); }, 700, 170},
        {"computers-searching", "home", 0, [](App&) {}},
        {"computers-not-found", "home", 0, [](App& a) { a.noPcsFor = 30; }},
        {"computers-connecting", "home", 0, [](App& a) { a.mode = Mode::Connecting; a.client.serverName = "Studio PC"; a.client.status = ""; }},
        {"computers-approval", "home", 0, [](App& a) { a.mode = Mode::Connecting; a.client.state = StreamClient::State::WaitingApproval; a.client.serverName = "Studio PC"; }},
        {"computers-problem", "home", 0, [](App& a) { Lan(a); a.problem = "Studio PC stopped answering. Check that RinCast Streamer is still running."; }},
        {"computers-connected", "home", 0, Live},
        {"games", "home", 1, [](App& a) {
             Live(a);
             for (int i = 0; i < 10; i++) { GameInfo g; g.id = std::to_string(i); g.name = kGameNames[i]; g.image.id = Art(i); a.games.push_back(g); }
         }},
        {"games-hover", "home", 1, [](App& a) {
             Live(a);
             for (int i = 0; i < 10; i++) { GameInfo g; g.id = std::to_string(i); g.name = kGameNames[i]; g.image.id = Art(i); a.games.push_back(g); }
         }, 500, 200},
        {"games-empty", "home", 1, [](App& a) { Live(a); }},
        {"streaming", "home", 2, [](App& a) { a.settings.vrBitrateMbps = 200; a.settings.upscaling = true; }},
        {"streaming-connected", "home", 2, [](App& a) { Live(a); a.settings.frameGeneration = true; a.settings.vrBitrateMbps = 0; }},
        {"streaming-match", "home", 2, [](App& a) { Live(a); a.settings.matchSteamVr = true; }},
        {"streaming-pico", "home", 2, [](App& a) { a.device.pico = true; a.input.simultaneous = false; }},
        {"desktop", "home", 3, [](App&) {}},
        {"environment", "home", 4, [](App&) {}},
        {"settings", "home", 5, [](App&) {}},
        {"account-signed-out", "home", 6, [](App&) {}},
        {"account-code", "home", 6, [](App& a) { a.account.state = Cloud::SignInState::WaitingForApproval; a.account.code = "K7QM-2XPL"; a.account.url = "https://rincast.com/link"; }},
        {"account-signed-in", "home", 6, [](App& a) { a.account.state = Cloud::SignInState::SignedIn; a.account.user = "Sam"; }},
        {"help", "home", 7, [](App& a) { Live(a); }},
        {"computers-take-over", "home", 0, [](App& a) { a.lan.pcs = {{"b", "Living room", "", true}}; }},
        {"high-contrast", "home", 2, [](App& a) { theme::SetHighContrast(true); a.settings.highContrast = true; }},
        {"dock", "dock", 0, Live},
        {"dock-hover", "dock", 0, Live, 500, 56},
        // The keyboard: BetterFrame Keyboard's layouts (compare with its own pictures)
        {"keyboard", "keyboard", 0, Live},
        {"keyboard-standard", "keyboard", 0, [](App& a) { a.kb.Conf().layout = "full"; }},
        {"keyboard-full", "keyboard", 0, [](App& a) { a.kb.Conf().layout = "advanced"; }},
        {"keyboard-123", "keyboard", 0, [](App& a) { a.kb.PreviewState().page = "123"; }},
        {"keyboard-sym", "keyboard", 0, [](App& a) { a.kb.PreviewState().page = "sym"; }},
        {"keyboard-standard-123", "keyboard", 0, [](App& a) { a.kb.Conf().layout = "full"; a.kb.PreviewState().page = "123"; }},
        {"keyboard-shift", "keyboard", 0, [](App& a) { a.kb.PreviewState().shift = true; }},
        {"keyboard-emoji", "keyboard", 0, [](App& a) { a.kb.PreviewState().page = "emoji"; }},
        {"keyboard-standard-emoji", "keyboard", 0, [](App& a) { a.kb.Conf().layout = "full"; a.kb.PreviewState().page = "emoji"; }},
        {"keyboard-words", "keyboard", 0, [](App& a) { a.kb.Conf().words = true; }},
        {"keyboard-standard-de", "keyboard", 0, [](App& a) { a.kb.Conf().layout = "full"; a.kb.Conf().lang = "de-DE"; }},
        {"keyboard-de", "keyboard", 0, [](App& a) { a.kb.Conf().lang = "de-DE"; }},
        {"keyboard-fr", "keyboard", 0, [](App& a) { a.kb.Conf().lang = "fr-FR"; }},
        {"keyboard-ru", "keyboard", 0, [](App& a) { a.kb.Conf().lang = "ru-RU"; }},
        {"keyboard-ar", "keyboard", 0, [](App& a) { a.kb.Conf().lang = "ar"; }},
        {"keyboard-ja", "keyboard", 0, [](App& a) { a.kb.Conf().lang = "ja-JP"; }},
        {"keyboard-light", "keyboard", 0, [](App& a) { a.kb.Conf().theme = "light"; }},
        {"keyboard-contrast", "keyboard", 0, [](App& a) { a.kb.Conf().theme = "contrast"; }},
        {"keyboard-hover", "keyboard", 0, [](App&) {}, 640, 560},
        {"keyboard-lang", "keyboard", 0, [](App& a) { a.kb.PreviewState().page = "lang"; }},
        {"keyboard-fmt", "keyboard", 0, [](App& a) { a.kb.PreviewState().page = "fmt"; }},
        {"keyboard-tip", "keyboard", 0, [](App&) {}, 1040, 255},
        {"stats", "stats", 0, Live},
        {"notice-error", "notices", 0, [](App& a) { a.notices.push_back({"Couldn't start Half-Life: Alyx. Check that Steam is running on your PC.", "error", "", 0, 3600}); }},
        {"notice-action", "notices", 0, [](App& a) { a.notices.push_back({"SteamVR needs a restart to use the new resolution. The running game closes.", "warning", "vr.restartSteamVR", 0, 15}); }},
        {"vr-overlay", "vr-overlay", 0, [](App& a) { Live(a); a.mode = Mode::Vr; a.vrRunning = true; }},
        {"vr-overlay-desktop", "vr-overlay", 0, [](App& a) { Live(a); a.vrRunning = true; }},
        {"vr-launching", "launch", 0, [](App& a) { Live(a); a.vrState = "Starting SteamVR…"; }},
        {"notice-retry", "notices", 0, [](App& a) { a.notices.push_back({"VR couldn't start on your PC. Check that SteamVR opens there.", "error", "vr.retry", 0, 3600}); }},
        {"computers-one", "home", 0, [](App& a) { a.lan.pcs = {{"a", "Studio PC", "NVIDIA GeForce RTX 5070 Ti", false}}; }},
        {"computers-vpn", "home", 0, [](App& a) { a.lan.pcs = {{"a", "Studio PC", "", false, true}}; }},
        {"computers-connected-vpn", "home", 0, [](App& a) { Live(a); a.client.vpn = true; }},
        {"computers-reconnecting", "home", 0, [](App& a) { a.reconnecting = true; a.client.serverName = "Studio PC"; }},
        {"settings-hover-check", "home", 5, [](App&) {}, 400, 120},
        {"keyboard-symbols", "keyboard", 0, Live, 120, 520, true},
        {"computers-address", "home", 0, [](App&) {}, 380, 366, true},
    };
    return scenes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    g_assets = argv[1];
    const std::string out = argv[2];
    const std::string only = argc > 3 ? argv[3] : "";
    std::filesystem::create_directories(out);
    // RINCAST_LANG=<code> previews the UI in that language, e.g. zh-CN.
    if (const char* lang = getenv("RINCAST_LANG")) SetLanguage(lang);

    // The GPU if it's free, else Windows' software renderer: the pictures come out the same.
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, nullptr, nullptr)) {
        auto getPlatformDisplay = reinterpret_cast<EGLDisplay (*)(EGLenum, void*, const EGLint*)>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        const EGLint warp[] = {0x3203 /*PLATFORM_ANGLE_TYPE*/, 0x3208 /*D3D11*/, 0x3209 /*DEVICE_TYPE*/, 0x320B /*D3D_WARP*/, EGL_NONE};
        d = getPlatformDisplay(0x3202 /*PLATFORM_ANGLE*/, EGL_DEFAULT_DISPLAY, warp);
        if (!eglInitialize(d, nullptr, nullptr)) return fprintf(stderr, "no renderer"), 3;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfgAttr[] = {EGL_RENDERABLE_TYPE, 0x40 /*ES3*/, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, cfgAttr, &cfg, 1, &n) || !n) return fprintf(stderr, "no EGL config\n"), 3;
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(d, cfg, pb);
    const EGLint ctxAttr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (!eglMakeCurrent(d, surf, surf, ctx)) return fprintf(stderr, "no EGL context\n"), 3;

    Assets assets;
    UiRenderer renderer;
    if (!renderer.Init(assets)) return fprintf(stderr, "renderer init failed\n"), 4;
    const float kScale = 1.25f;
    for (const Scene& s : Scenes()) {
        if (!only.empty() && only != s.name) continue;
        theme::SetHighContrast(false);
        // Fresh widget state for each picture.
        WindowManager wm;
        wm.Init(&renderer, nullptr);
        App app;
        app.kb.Init(assets, {[](uint16_t, bool) {}, [](const std::string&) {}, [] {}, [] {}, [] {}});
        if (s.window == std::string("keyboard")) {
            s.setup(app);
            const kb::State st = app.kb.PreviewState();
            app.kb.ConfChanged();
            app.kb.PreviewState().page = st.page;
            app.kb.PreviewState().shift = st.shift;
            app.kb.SetVisible(true);
        } else {
            s.setup(app);
        }
        screens::Build(app, wm);
        screens::SetPage(s.page);
        Window* w = wm.Find(s.window);
        if (!w) continue;
        if (s.window == std::string("keyboard")) app.kb.Size(*w);
        const int rw = int(w->canvasW * kScale), rh = int(w->canvasH * kScale);
        RenderTarget rt;
        rt.Create(rw, rh, GL_SRGB8_ALPHA8, false, false);
        for (int frame = 0; frame < 8; frame++) {
            glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
            glViewport(0, 0, rw, rh);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.Begin(rw, rh, kScale);
            PointerState p;
            p.hovering = s.hoverX >= 0;
            if (s.click && frame == 2) p.pressed = p.down = true;
            if (s.click && frame == 3) p.released = true;
            p.pos = {s.hoverX, s.hoverY};
            w->hands[1] = p;
            Ui& ui = wm.UiContext();
            ui.Begin(w->id, p, 0.05f, 12.f);
            w->build(ui, Rect{0, 0, float(w->canvasW), float(w->canvasH)});
            ui.End();
            renderer.End();
        }
        std::vector<uint8_t> px(size_t(rw) * rh * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        // Over a dark sky color, as it floats in the headset, with a margin. GL's rows run bottom-up.
        const int m = 24, ow = rw + m * 2, oh = rh + m * 2;
        std::vector<uint8_t> img(size_t(ow) * oh * 3);
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) {
                uint8_t* o = &img[size_t(y * ow + x) * 3];
                o[0] = 12; o[1] = 12; o[2] = 40;
                if (x < m || y < m || x >= m + rw || y >= m + rh) continue;
                const uint8_t* q = &px[size_t((rh - 1 - (y - m)) * rw + (x - m)) * 4];
                const float a = q[3] / 255.f;
                for (int c = 0; c < 3; c++) o[c] = uint8_t(std::min(255.f, q[c] + o[c] * (1 - a)));
            }
        stbi_write_png((out + "/" + s.name + ".png").c_str(), ow, oh, 3, img.data(), ow * 3);
        printf("%s\n", s.name);
        rt.Destroy();
    }
    return 0;
}
