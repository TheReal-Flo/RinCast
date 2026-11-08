#include "keyboard.h"

#include <algorithm>
#include <cmath>

#include "nlohmann/json.hpp"
#include "platform/assets.h"
#include "rc/platform.h"
#include "ui/window.h"

namespace rv::kb {

namespace {

// Windows virtual-key codes for BetterFrame's key names
enum : uint16_t {
    VK_BACK = 0x08, VK_TAB = 0x09, VK_ENTER = 0x0D, VK_SHIFT = 0x10, VK_CTRL = 0x11, VK_ALT = 0x12, VK_PAUSE = 0x13,
    VK_ESC = 0x1B, VK_SPACE = 0x20, VK_PGUP = 0x21, VK_PGDN = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25,
    VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_PRTSC = 0x2C, VK_INS = 0x2D, VK_DEL = 0x2E, VK_WIN = 0x5B,
    VK_F1 = 0x70, VK_SCRLK = 0x91, VK_MUTE = 0xAD, VK_VOLDOWN = 0xAE, VK_VOLUP = 0xAF, VK_NEXT = 0xB0, VK_PREV = 0xB1,
    VK_PLAY = 0xB3,
};

uint16_t Named(const std::string& a) {
    static const std::map<std::string, uint16_t> m = {
        {"back", VK_BACK}, {"enter", VK_ENTER}, {"space", VK_SPACE}, {"tab", VK_TAB}, {"esc", VK_ESC}, {"del", VK_DEL},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN}, {"ins", VK_INS}, {"home", VK_HOME},
        {"end", VK_END}, {"pgup", VK_PGUP}, {"pgdn", VK_PGDN}, {"prtsc", VK_PRTSC}, {"scrlk", VK_SCRLK}, {"pause", VK_PAUSE}};
    if (auto it = m.find(a); it != m.end()) return it->second;
    if (a.size() >= 2 && a[0] == 'f' && std::all_of(a.begin() + 1, a.end(), ::isdigit)) return uint16_t(VK_F1 + std::stoi(a.substr(1)) - 1);
    return 0;
}

// Fn + key (BetterFrame's FN): what the key does instead
uint16_t FnKey(const std::string& act) {
    static const std::map<std::string, uint16_t> m = {
        {"f1", VK_MUTE}, {"f2", VK_VOLDOWN}, {"f3", VK_VOLUP}, {"f4", VK_PREV}, {"f5", VK_PLAY}, {"f6", VK_NEXT},
        {"f12", VK_PRTSC}, {"del", VK_INS}, {"left", VK_HOME}, {"right", VK_END}, {"up", VK_PGUP}, {"down", VK_PGDN}};
    auto it = m.find(act);
    return it == m.end() ? 0 : it->second;
}

// The numpad with Num Lock off
uint16_t NumpadKey(const std::string& np) {
    static const std::map<std::string, uint16_t> m = {
        {"HOME", VK_HOME}, {"UP", VK_UP}, {"PAGEUP", VK_PGUP}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"END", VK_END},
        {"DOWN", VK_DOWN}, {"PAGEDOWN", VK_PGDN}, {"INSERT", VK_INS}, {"DELETE", VK_DEL}};
    auto it = m.find(np);
    return it == m.end() ? 0 : it->second;
}

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

Color Rgb(const Data::Rgba& c, float a = -1) { return {c[0] / 255.f, c[1] / 255.f, c[2] / 255.f, a >= 0 ? a : c[3] / 255.f}; }

}  // namespace

const std::vector<std::string>& Keyboard::ThemeNames() { return Data::Get().ThemeOrder(); }

bool Keyboard::Init(const Assets& assets, Output out) {
    m_assets = &assets;
    m_out = std::move(out);
    if (!Data::Get().Load(assets)) return false;
    m_decoder = std::make_unique<Decoder>(assets);
    m_clicks.Load(assets);
    LoadImages(assets);
    ConfChanged();
    return true;
}

void Keyboard::ConfChanged() {
    Data& d = Data::Get();
    if (!d.HasLang(m_conf.lang)) m_conf.lang = "en-US";
    m_st.layout = m_conf.layout;
    m_st.lang = m_conf.lang;
    m_st.fmt = m_conf.fmt;
    m_st.barpos = m_conf.bar;
    m_st.bottomrow = m_conf.bottom;
    m_st.swipe = m_conf.swipe;
    m_st.words = m_conf.words;
    m_st.curved = m_conf.curved;
    m_st.panel = m_conf.panel;
    m_st.etone = m_conf.tone;
    m_layoutKey.clear();
}

void Keyboard::Save() {
    m_conf.layout = m_st.layout;
    m_conf.lang = m_st.lang;
    m_conf.fmt = m_st.fmt;
    m_conf.swipe = m_st.swipe;
    m_conf.words = m_st.words;
    m_conf.curved = m_st.curved;
    m_conf.panel = m_st.panel;
    m_conf.tone = m_st.etone;
    if (m_out.saved) m_out.saved();
}

void Keyboard::SetVisible(bool visible) {
    if (visible == m_visible) return;
    m_visible = visible;
    m_clicks.SetOpen(visible && m_conf.sound != "off");
    if (!visible) {
        for (int s = 0; s < 2; s++) Release(s, m_now);
        m_repeat.reset();
    }
}

Pose Keyboard::PlaceInFront(const Pose& head) const {
    const Data& d = Data::Get();
    Vec3 fwd = head.Forward();
    fwd.y = 0;
    fwd = Normalize(fwd);
    Pose p;
    p.position = head.position + fwd * d.distance;
    p.position.y -= d.drop;
    // upright and facing you, then turned to face the eyes from below and leaning back a little
    // more, like a laptop's keyboard (BetterFrame's tilt)
    const Quat upright = WindowManager::FaceTowards(p.position, {head.position.x, p.position.y, head.position.z});
    const float lean = std::atan2(d.drop, d.distance) + d.tilt * kPi / 180;
    // the way that leans its top away: the top edge (+y) moves away from the eyes
    const Quat a = upright * Quat::AxisAngle({1, 0, 0}, lean), b = upright * Quat::AxisAngle({1, 0, 0}, -lean);
    const Vec3 away = Normalize(Vec3{fwd.x, 0, fwd.z});
    p.orientation = Dot(a.Rotate({0, 1, 0}), away) > Dot(b.Rotate({0, 1, 0}), away) ? a : b;
    return p;
}

// ---------------------------------------------------------------------------------------
// Images: the icon sheet, and the emoji (loaded when they're first needed)
// ---------------------------------------------------------------------------------------

void Keyboard::LoadImages(const Assets& assets) {
    std::vector<uint8_t> file;
    int w = 0, h = 0;
    if (assets.Read("keyboard/icons.png", file)) {
        std::vector<uint8_t> px = DecodeImage(file, 4, w, h);
        // White icons: the clear pixels white too, so they don't darken the edges when scaled
        for (size_t i = 0; i < px.size(); i += 4) px[i] = px[i + 1] = px[i + 2] = px[i + 3] ? px[i] : 255;
        if (!px.empty()) UploadImageTexture(px.data(), w, h, 4, true, true, m_icons);
    }
    try {
        const auto j = nlohmann::json::parse(assets.ReadText("keyboard/icons.json"));
        m_iconsW = j["size"][0];
        m_iconsH = j["size"][1];
        m_iconSize = j["icon"];
        m_handleBox = j["handle"][0];
        m_handleMargin = j["handle"][1];
        for (auto& [k, v] : j["cells"].items()) m_iconCells[k] = {v[0], v[1], v[2], v[3]};
    } catch (const std::exception& e) {
        RC_LOGE("Keyboard: icons.json: %s", e.what());
    }
}

void Keyboard::LoadEmoji() {
    m_emojiLoaded = true;
    std::vector<uint8_t> color, alpha;
    int w = 0, h = 0, aw = 0, ah = 0;
    if (!m_assets->Read("keyboard/emoji-color.jpg", color) || !m_assets->Read("keyboard/emoji-alpha.png", alpha)) return;
    std::vector<uint8_t> px = DecodeImage(color, 4, w, h);
    const std::vector<uint8_t> a = DecodeImage(alpha, 1, aw, ah);
    if (px.empty() || a.size() != size_t(w) * h) return;
    // The colour was saved on black: back to its own colour where it shows
    for (size_t i = 0, n = size_t(w) * h; i < n; i++) {
        const int al = a[i];
        uint8_t* p = &px[i * 4];
        for (int c = 0; c < 3; c++) p[c] = al ? uint8_t(std::min(255, p[c] * 255 / al)) : 0;
        p[3] = uint8_t(al);
    }
    UploadImageTexture(px.data(), w, h, 4, true, true, m_emoji);
    m_emojiW = float(w);
    m_emojiH = float(h);
    try {
        const auto j = nlohmann::json::parse(m_assets->ReadText("keyboard/emoji.json"));
        for (auto& [k, v] : j["emoji"].items()) m_emojiCells[k] = {v[0], v[1], v[2], v[3]};
    } catch (const std::exception& e) {
        RC_LOGE("Keyboard: emoji.json: %s", e.what());
    }
}

// ---------------------------------------------------------------------------------------
// Layout (Renderer.layout)
// ---------------------------------------------------------------------------------------

Color Keyboard::Pal(const char* name, float alpha) const {
    auto it = m_pal.find(name);
    if (it == m_pal.end()) return {1, 1, 1, alpha >= 0 ? alpha : 1};
    return Rgb(it->second, alpha);
}

void Keyboard::Layout() {
    const Data& d = Data::Get();
    if (m_palFor != m_conf.theme) {
        m_pal = d.Theme(m_conf.theme);
        m_palFor = m_conf.theme;
    }
    m_st.barpos = m_conf.bar;
    m_st.bottomrow = m_conf.bottom;
    const bool mini = m_st.layout == "minimal";
    m_W = m_st.layout == "advanced" ? 2048 : 1600;  // the full-size keyboard is wider, kept sharp
    const float units = d.UnitsFor(m_st.layout, m_st.lang, m_st.fmt, m_st);
    const auto& pitch = mini ? d.pitchMinimal : m_st.layout == "advanced" ? d.pitchAdvanced : d.pitchFull;
    const float pw = pitch[0], ph = pitch[1];
    m_upx = m_W / (units + 0.4f);
    m_uph = m_upx * ph / pw;
    m_gap = m_upx * 0.1f;
    m_margin = m_upx * 0.2f;
    const std::vector<Row> abc = d.RowsFor(m_st.layout, "abc", m_st.lang, m_st.fmt, m_st);
    const int refRows = int(abc.size());
    const std::vector<Row> rows = m_st.page == "abc" ? abc : d.RowsFor(m_st.layout, m_st.page, m_st.lang, m_st.fmt, m_st);
    const float barH = m_uph * 0.6f;
    const float stripH = m_st.words ? m_uph * 0.62f : 0;
    // room above the keys for the letter bubbles, labels and messages (BetterFrame floats them above)
    m_top = m_uph * 1.25f;
    const float top = m_top + m_margin + stripH + (stripH > 0 ? m_gap : 0);
    m_stripY0 = m_top + m_margin;
    m_stripY1 = m_stripY0 + stripH;
    m_items.clear();
    float gridW = 0;
    for (const Row& r : abc) {
        float s = 0;
        for (const Key& k : r) s += k.w;
        gridW = std::max(gridW, s * m_upx);
    }
    m_gx0 = (m_W - gridW) / 2;
    m_gx1 = (m_W + gridW) / 2 - m_gap;
    const Row bar = m_st.barpos == "off" ? Row{} : d.BarItems(m_st);
    // every button as wide as it needs, the whole bar shrinking together if it's tight
    std::vector<float> need;
    for (const Key& k : bar) need.push_back(k.spacer ? barH * 0.35f : barH * (k.wide ? 1.9f : 1.12f) + m_gap);
    float total = 0;
    for (float n : need) total += n;
    const float sf = (m_gx1 - m_gx0 + m_gap) / std::max(1.f, total);
    m_barPx = barH * 0.4f * std::min(1.0f, sf);
    const bool below = m_st.barpos == "bottom";
    const float by = below ? top + refRows * m_uph + m_gap * 0.6f : top;
    float x = m_gx0;
    for (size_t i = 0; i < bar.size(); i++) {
        const float w = need[i] * sf;
        if (!bar[i].spacer) m_items.push_back({bar[i], x, by, x + w - m_gap, by + barH});
        x += w;
    }
    if (m_st.words && !m_st.mic) {  // the word bar: three spots to tap
        const float cw = (m_gx1 - m_gx0 + m_gap) / 3;
        for (int i = 0; i < 3; i++) {
            Key k;
            k.w = cw;
            k.bar = true;
            k.ghost = true;
            k.act = "sugg:" + std::to_string(i);
            const float sx = m_gx0 + i * cw;
            m_items.push_back({k, sx, m_stripY0, sx + cw - m_gap, m_stripY1});
        }
    }
    const float y0 = below || bar.empty() ? top : top + barH + m_gap * 1.6f;
    const float offset = m_st.page == "lang" || m_st.page == "fmt" ? (refRows - int(rows.size())) / 2.f
                                                                   : float(refRows - int(rows.size()));
    for (size_t i = 0; i < rows.size(); i++) {
        const float y = y0 + (float(i) + offset) * m_uph;
        float rw = 0;
        for (const Key& k : rows[i]) rw += k.w;
        float kx = (m_W - rw * m_upx) / 2;
        for (const Key& k : rows[i]) {
            const float w = k.w * m_upx;
            if (!k.spacer) {
                const float tall = k.tall ? m_uph : 0;  // numpad + and Enter span two rows
                m_items.push_back({k, kx, y, kx + w - m_gap, y + m_uph + tall - m_gap});
            }
            kx += w;
        }
    }
    const float gy = below && !bar.empty() ? by + barH + m_gap * 2.0f : y0 + refRows * m_uph + m_gap * 0.6f;
    // the bar you hold to move it: in the middle, under the keys; the corners resize and tilt it
    const float gcx = m_W / 2, rs = m_upx * 0.42f, gh = std::max(8.f, m_uph * 0.1f);
    const float gyc = gy - m_gap * 0.2f + (rs - gh) / 2;
    m_grab = {Key{}, gcx - m_upx * 1.6f, gyc, gcx + m_upx * 1.6f, gyc + gh};
    m_resize = {Key{}, m_gx1 - rs, gy - m_gap * 0.2f, m_gx1, gy - m_gap * 0.2f + rs};
    const float lx0 = m_W - m_gx1;
    m_rotate = {Key{}, lx0, gy - m_gap * 0.2f, lx0 + rs, gy - m_gap * 0.2f + rs};
    m_H = std::max(m_grab.y1, m_resize.y1) + m_margin * 0.6f;
    m_widthM = m_W / m_upx * pw * m_conf.scale;  // (resizing by hand changes the scale; see SizeWindow)
    // the backdrop: around every key and handle
    if (!m_items.empty()) {
        const float pad = std::max(10.f, m_items[0].H() * 0.28f);
        float x0 = 1e9f, yy0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
        auto take = [&](const Item& b) {
            x0 = std::min(x0, b.x0);
            yy0 = std::min(yy0, b.y0);
            x1 = std::max(x1, b.x1);
            y1 = std::max(y1, b.y1);
        };
        for (const Item& it : m_items) take(it);
        take(m_grab);
        take(m_resize);
        if (stripH > 0) take({Key{}, m_gx0, m_stripY0, m_gx1, m_stripY1});
        m_panel = {std::max(1.f, x0 - pad), std::max(m_top + 1, yy0 - pad), std::min(m_W - 2, x1 + pad),
                   std::min(m_H - 2, y1 + pad * 0.6f)};
    }
}

void Keyboard::SizeWindow(Window& w) {
    const int cw = int(m_W), ch = int(std::ceil(m_H));
    // the resize corner sets the scale; the layout sets the rest
    const Data& d = Data::Get();
    const auto& pitch = m_st.layout == "minimal" ? d.pitchMinimal : m_st.layout == "advanced" ? d.pitchAdvanced : d.pitchFull;
    const float natural = m_W / m_upx * pitch[0];
    static float lastSet = 0;
    if (lastSet > 0 && std::fabs(w.widthM - lastSet) > 1e-4f) {  // resized by hand
        m_conf.scale = std::clamp(w.widthM / natural, 0.25f, 2.0f);
        m_widthM = natural * m_conf.scale;
        m_scaleDirty = true;
    } else if (m_scaleDirty) {
        m_scaleDirty = false;
        Save();
    }
    if (w.canvasW != cw || w.canvasH != ch || std::fabs(w.widthM - m_widthM) > 1e-4f) {
        const float oldH = w.HeightM();
        w.canvasW = cw;
        w.canvasH = ch;
        w.aspect = float(ch) / float(cw);
        w.widthM = m_widthM;
        w.minWidthM = natural * 0.25f;
        w.maxWidthM = natural * 2.0f;
        // the top edge stays where it is
        w.pose.position = w.pose.position + w.pose.orientation.Rotate({0, (oldH - w.HeightM()) / 2, 0});
    }
    lastSet = w.widthM;
    w.rtScale = 1.0f;  // BetterFrame's own pixels
    w.curveRadius = m_st.curved ? 0.95f : 0;
}

HitZone Keyboard::ZoneAt(Vec2 p) const {
    auto in = [&](const Item& b, float pad) { return p.x >= b.x0 - pad && p.x <= b.x1 + pad && p.y >= b.y0 - pad && p.y <= b.y1 + pad; };
    const float pad = m_gap;
    if (in(m_grab, pad * 1.5f)) return HitZone::Handle;
    if (in(m_resize, pad)) return HitZone::ResizeRight;
    if (in(m_rotate, pad)) return HitZone::Tilt;
    if (p.x >= m_panel.x0 && p.x <= m_panel.x1 && p.y >= m_panel.y0 && p.y <= m_panel.y1) return HitZone::Content;
    return HitZone::None;  // the clear space around it: the ray goes on to what's behind
}

// ---------------------------------------------------------------------------------------
// Drawing (Renderer._draw, draw_keys and the overlays)
// ---------------------------------------------------------------------------------------

void Keyboard::DrawText(UiRenderer& R, const std::string& text, float x, float y, float px, const Color& c, bool left) {
    // Pillow's anchors: "mm" centred, "lm" left; middle is halfway between ascender and descender
    px = std::floor(px);
    const Font& f = R.Fonts();
    const float asc = f.Ascent(px, Face::Keys), desc = f.Descent(px, Face::Keys);
    const float topY = y + (desc - asc) / 2;
    if (left) R.Text(text, x, topY, px, c, Face::Keys, Align::Left);
    else R.Text(text, x, topY, px, c, Face::Keys, Align::Center);
}

float Keyboard::FitFont(UiRenderer& R, const std::string& text, float px, float maxW) {
    px = std::floor(px);
    while (px > 10 && R.Measure(text, px, Face::Keys) > maxW) px -= 2;
    return px;
}

void Keyboard::DrawIcon(UiRenderer& R, const std::string& kind, float cx, float cy, float s, const Color& c) {
    auto it = m_iconCells.find(kind);
    if (it == m_iconCells.end() || !m_icons.id) return;
    s = std::floor(s);
    const auto& [x, y, w, h] = it->second;
    R.ImageRegion(m_icons.id, {std::round(cx - s / 2), std::round(cy - s / 2), s, s * h / w}, x / m_iconsW, y / m_iconsH,
                  (x + w) / m_iconsW, (y + h) / m_iconsH, c);
}

void Keyboard::DrawEmoji(UiRenderer& R, const std::string& e, float cx, float cy, float size) {
    if (!m_emojiLoaded) LoadEmoji();
    auto it = m_emojiCells.find(e);
    if (it == m_emojiCells.end() || !m_emoji.id) {
        DrawText(R, e, cx, cy, size * 0.8f, Pal("text"));  // no picture: as text, like BetterFrame without the font
        return;
    }
    const auto& [x, y, w, h] = it->second;
    const float sc = size / std::max(w, h);
    const float dw = std::max(1.f, std::round(w * sc)), dh = std::max(1.f, std::round(h * sc));
    R.ImageRegion(m_emoji.id, {std::floor(cx - dw / 2), std::floor(cy - dh / 2), dw, dh}, x / m_emojiW, y / m_emojiH,
                  (x + w) / m_emojiW, (y + h) / m_emojiH);
}

void Keyboard::OpenPicker(const std::string& page) {
    const Data& d = Data::Get();
    m_st.alt = -1;
    if (page == "fmt") {
        if (d.CanReformat(m_st.lang)) m_st.page = "fmt";
        else Flash(d.Language(m_st.lang).name + " keeps its own layout");
        return;
    }
    m_st.page = "lang";
    m_st.lgroup.clear();
    const std::string cur = m_st.lang.substr(0, m_st.lang.find('-'));
    const auto& groups = d.Groups();
    for (size_t i = 0; i < groups.size(); i++)
        if (groups[i].key == cur) m_st.lpage = int(i) / (Data::kLangCols * Data::kLangRows);
}

void Keyboard::ResetPlace() {
    m_conf.scale = Data::Get().scale;
    m_layoutKey.clear();
    Save();
    if (m_out.recenter) m_out.recenter();
}

void Keyboard::Draw(UiRenderer& R, float now) {
    if (m_st.panel) {  // a slightly see-through back behind the keys
        const float pad = m_items.empty() ? 10 : std::max(10.f, m_items[0].H() * 0.28f);
        const float a = std::clamp(m_conf.backdrop / 100.f, 0.f, 1.f);
        const Rect r{m_panel.x0, m_panel.y0, m_panel.x1 - m_panel.x0, m_panel.y1 - m_panel.y0};
        R.RectBorder(r, Pal("panel", a), pad * 1.6f, Color(1, 1, 1, 30 / 255.f), 2);
    }
    // the language picker: one text size for every name, so the grid looks even
    std::vector<float> sizes;
    float hh = 1e9f, ww = 1e9f;
    for (const Item& it : m_items)
        if (it.k.pick) {
            hh = std::min(hh, it.H());
            ww = std::min(ww, it.W());
        }
    for (const Item& it : m_items)
        if (it.k.pick) sizes.push_back(FitFont(R, it.k.label, hh * 0.4f, ww - hh * 0.4f));
    std::sort(sizes.begin(), sizes.end());
    if (!sizes.empty()) m_pickCap = sizes[sizes.size() / 4];  // what nearly every name fits at
    DrawKeys(R);
    const Item& g = m_grab;
    R.RectFill({g.x0, g.y0, g.W(), g.H()}, Pal("grab"), g.H() / 2);
    // the corner handles: resize (bottom right) and tilt (bottom left)
    for (const Item* h : {&m_resize, &m_rotate}) {
        auto it = m_iconCells.find(h == &m_resize ? "resize" : "rotate");
        if (it == m_iconCells.end()) continue;
        const float s = h->W() / m_handleBox, m = m_handleMargin * s;
        const auto& [x, y, w, hh] = it->second;
        R.ImageRegion(m_icons.id, {h->x0 - m, h->y0 - m, h->W() + 2 * m, h->H() + 2 * m}, x / m_iconsW, y / m_iconsH,
                      (x + w) / m_iconsW, (y + hh) / m_iconsH, Pal("grab"));
    }
    if (m_stripY1 > m_stripY0)  // the word bar
        R.RectFill({m_gx0, m_stripY0, m_gx1 - m_gx0, m_stripY1 - m_stripY0}, Pal("strip"), (m_stripY1 - m_stripY0) * 0.35f);
    DrawOverlays(R, now);
}

void Keyboard::DrawKeys(UiRenderer& R) {
    const Data& d = Data::Get();
    const Lang& lang = d.Language(m_st.lang);
    const bool up = m_st.shift != m_st.caps;
    const int alt = m_st.alt;
    const auto& layer = LayerMap();
    // (Shift stands in for Caps Lock only on a layout with no Caps key of its own, like Minimal)
    bool layCaps = false;
    for (const Row& r : d.RowsFor(m_st.layout, "abc", m_st.lang, m_st.fmt, m_st))
        for (const Key& k : r) layCaps = layCaps || k.act == "caps";
    const bool capsOnShift = m_st.caps && !layCaps;
    for (const Item& it : m_items) {
        const Key& k = it.k;
        if (k.ghost) continue;
        const float x0 = it.x0, y0 = it.y0, x1 = it.x1, y1 = it.y1;
        float h = std::min(y1 - y0, (x1 - x0) * 1.25f);
        if (k.tall) h = std::min(h, m_uph - m_gap);  // tall keys keep normal-size text
        const bool lit = (k.act == "shift" && (m_st.shift || capsOnShift)) || (k.act == "caps" && m_st.caps) ||
                         (k.act == "ctrl" && m_st.ctrl) || (k.act == "alt-mod" && m_st.altm) || (k.act == "fn" && m_st.fn) ||
                         (k.act == "super" && m_st.meta) || (k.act == "numlock" && m_st.numlock) || (k.altkey && alt >= 0);
        const bool fnOn = m_st.fn && d.HasFn(k.act);
        const bool accented = (k.ch && layer.count(*k.ch)) || fnOn;
        const char* fill = k.rec ? "rec" : k.bar && k.on ? "on" : k.bar ? "bar" : k.go ? "go" : lit ? "on" : accented ? "accent"
                           : k.mod ? "mod" : "key";
        const Color fc = Pal(fill);
        const Rect box{x0, y0, x1 - x0, y1 - y0};
        if (m_pal.count("outline")) R.RectBorder(box, fc, h * 0.2f, Pal("outline"), std::max(1.f, std::floor(h * 0.04f)));
        else R.RectFill(box, fc, h * 0.2f);
        if (m_st.locks.count(k.act) || (k.act == "caps" && m_st.caps) || (k.act == "shift" && capsOnShift)) {
            // locked on (a double tap): a short bar along the bottom, like a laptop's Caps Lock light
            const float pad = h * 0.22f, by = y1 - h * 0.13f;
            R.RectFill({x0 + pad, by - h * 0.03f, x1 - x0 - 2 * pad, h * 0.06f}, Pal("text"), h * 0.03f);
        }
        std::string label = k.label, sub;
        if (k.ch) {
            auto lc = layer.find(*k.ch);
            const std::string c = lc != layer.end() ? lc->second : *k.ch;
            label = d.Show(up ? d.Shifted(c, lang) : c);
            if (lang.romaji) sub = d.RomajiHint(*k.ch);  // the sound each key starts
        } else if (k.altkey) {  // the accent key shows the set it switches to next
            const int nxt = alt + 1;
            const std::string sample = nxt >= int(lang.layerSample.size()) ? "abc" : lang.layerSample[nxt];
            label = sample == "abc" ? sample : up ? d.Upper(sample, lang) : sample;
        } else if (k.sym) {
            label = m_st.shift ? (*k.sym)[1] : (*k.sym)[0];
            sub = m_st.shift ? (*k.sym)[0] : (*k.sym)[1];
            if (m_st.caps && !m_st.shift && d.IsAlpha(label)) label = d.Upper(label, lang);
            label = d.Show(label);
            sub = d.Show(sub);
        }
        if (!k.np.empty() && !m_st.numlock) label = k.nplabel.empty() ? k.label : k.nplabel;
        if (k.act == "shift" && capsOnShift && !lang.kana) label = "Caps";
        const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
        const Color text = Pal("text");
        if (fnOn) {  // Fn is on: what this key does now
            const std::string name = d.FnName(k.act);
            DrawText(R, name, cx, cy, FitFont(R, name, h * 0.3f, (x1 - x0) * 0.86f), text);
        } else if (k.emoji) {
            DrawEmoji(R, k.label, cx, cy, h * (k.bar ? 0.7f : 0.74f));
        } else if (!k.icon.empty() && k.off && (k.icon == "left" || k.icon == "right")) {
            DrawIcon(R, k.icon, cx, cy, h * 0.5f, Pal("sub"));  // a page arrow with nowhere to go: dimmed
        } else if (!k.icon.empty()) {
            static const std::map<std::string, float> sizes = {
                {"mic", 0.72f}, {"sound", 0.64f}, {"panel", 0.74f}, {"globe", 0.64f}, {"preview", 0.68f}, {"words", 0.66f},
                {"search", 0.6f}, {"gear", 0.64f}, {"swipe", 0.7f}, {"curve", 0.7f}, {"emoji", 0.72f}, {"cut", 0.6f},
                {"copy", 0.6f}, {"paste", 0.6f}, {"undo", 0.6f}, {"selall", 0.72f}, {"recenter", 0.62f},
                {"lay-minimal", 1.6f}, {"lay-full", 1.6f}, {"lay-advanced", 1.6f}};
            auto sz = sizes.find(k.icon);
            const bool slashed = k.off && (k.icon == "curve" || k.icon == "panel");
            DrawIcon(R, slashed ? k.icon + "-off" : k.icon, cx, cy, h * (sz == sizes.end() ? 0.5f : sz->second), text);
        } else if (k.altkey) {
            DrawText(R, label, cx, cy, h * (label == "abc" ? 0.3f : 0.36f), text);
        } else if (k.pick) {  // most names share one size; a long one only shrinks itself
            DrawText(R, label, cx, cy, FitFont(R, label, m_pickCap, (x1 - x0) - h * 0.4f), text);
        } else if (k.bar && !k.sample.empty()) {  // the layout picker: its name, and its first letters underneath
            DrawText(R, label, cx, cy - h * 0.14f, FitFont(R, label, h * 0.34f, (x1 - x0) - h * 0.35f), text);
            DrawText(R, k.sample, cx, cy + h * 0.24f, FitFont(R, k.sample, h * 0.2f, (x1 - x0) * 0.8f),
                     k.on ? Color(205 / 255.f, 222 / 255.f, 240 / 255.f, 1) : Pal("sub"));
        } else if (k.bar) {
            const float px = k.fitlabel ? FitFont(R, label, h * 0.4f, (x1 - x0) - h * 0.35f) : m_barPx;
            DrawText(R, label, cx, cy, px, text);
        } else if ((k.mod || k.go) && !k.center) {
            DrawText(R, label, x0 + h * 0.24f, cy, h * 0.26f, text, true);
        } else if (k.center) {
            DrawText(R, label, cx, cy, FitFont(R, label, h * 0.32f, (x1 - x0) * 0.84f), text);
        } else {
            if (!sub.empty()) DrawText(R, sub, cx, cy - h * 0.22f, h * 0.19f, Pal("sub"));
            DrawText(R, label, cx, cy + h * (sub.empty() ? 0.03f : 0.12f), FitFont(R, label, h * (sub.empty() ? 0.35f : 0.3f), (x1 - x0) * 0.86f),
                     text);
        }
        if (k.fav && !fnOn) {  // one of your favorites: a small star in its corner
            auto st = m_iconCells.find("star");
            if (st != m_iconCells.end()) {
                const float r = h * 0.1f, s = r / 0.48f, sx = x1 - h * 0.17f, sy = y0 + h * 0.17f;
                const auto& [ix, iy, iw, ih] = st->second;
                R.ImageRegion(m_icons.id, {sx - s / 2, sy - s / 2, s, s}, ix / m_iconsW, iy / m_iconsH, (ix + iw) / m_iconsW,
                              (iy + ih) / m_iconsH, Color(1, 1, 1, 1));
            }
        }
    }
}

void Keyboard::DrawOverlays(UiRenderer& R, float now) {
    const Data& d = Data::Get();
    // the swipe line: a glowing stroke fading toward its tail, the last moment of the swipe only
    for (Slot& s : m_slots) {
        if (!s.swipe || s.swipe->held || s.swipe->pts.size() < 2 || !DrawSwipe(*s.swipe)) continue;
        std::vector<Pt> pts = s.swipe->trail;
        const auto& tt = s.swipe->trailT;
        size_t cut = pts.size() >= 2 ? pts.size() - 2 : 0;
        for (size_t i = 0; i < tt.size(); i++)
            if (now - tt[i] < 0.35f) {
                cut = i;
                break;
            }
        float need = KeyW() * 2.0f, got = 0;
        size_t i = pts.size() - 1;
        while (i > 0 && got < need) {
            got += std::hypot(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y);
            i--;
        }
        cut = std::min(cut, i);
        pts.erase(pts.begin(), pts.begin() + std::min(cut, pts.size() - 2));
        const size_t n = pts.size();
        for (size_t j = 0; j + 1 < n; j++) {
            const float f = float(j + 1) / float(n - 1), v = 0.9f * std::pow(f, 1.5f);
            if (v < 6 / 255.f) continue;
            const Pt a = pts[j], b = pts[j + 1];
            const int steps = std::max(1, int(std::hypot(b.x - a.x, b.y - a.y) / 3));
            for (int k = 0; k <= steps; k++) {
                const float x = a.x + (b.x - a.x) * k / steps, y = a.y + (b.y - a.y) * k / steps;
                R.Circle(x, y, 7, Color(90 / 255.f, 160 / 255.f, 1, v * 0.12f));
                R.Circle(x, y, 3, Color(235 / 255.f, 244 / 255.f, 1, v * 0.45f));
            }
        }
    }
    // the word bar's words
    if (m_stripY1 > m_stripY0) {
        const float H = m_stripY1 - m_stripY0, cy = (m_stripY0 + m_stripY1) / 2, tw = m_gx1 - m_gx0;
        auto put = [&](std::string text, float cx, float cw, const Color& c, float size) {
            while (size > H * 0.25f && R.Measure(text, std::floor(size), Face::Keys) > cw * 0.9f) size *= 0.9f;
            while (Length(text) > 2 && R.Measure(text, std::floor(size), Face::Keys) > cw * 0.9f) {
                auto cs = Chars(text);
                cs.resize(cs.size() - 2);
                text.clear();
                for (auto& c2 : cs) text += c2;
                text += "…";
            }
            DrawText(R, text, cx, cy, size, c);
        };
        bool swiping = false;
        for (Slot& s : m_slots) swiping = swiping || (s.swipe && s.swipe->pts.size() > 1 && RealSwipe(*s.swipe));
        if (swiping) {
            if (!m_previewWord.empty()) {
                const float w = R.Measure(m_previewWord, std::floor(H * 0.5f), Face::Keys) + H * 0.9f, cx = m_gx0 + tw / 2;
                R.RectFill({cx - w / 2, m_stripY0 + H * 0.12f, w, H * 0.76f}, Pal("accent"), H * 0.38f);
                put(m_previewWord, cx, tw * 0.6f, Pal("text"), H * 0.5f);
            }
        } else {
            for (int i = 0; i < 3; i++) {
                const float x0 = m_gx0 + tw * i / 3, x1 = m_gx0 + tw * (i + 1) / 3;
                if (i && !m_wordList.empty())
                    R.RectFill({x0 - H * 0.05f, m_stripY0 + H * 0.28f, 1, H * 0.44f}, Pal("sub", 90 / 255.f));
                if (i < int(m_wordList.size())) {
                    if (m_wordMode == "swipe" && i == m_wordSel)
                        R.RectFill({x0 + H * 0.12f, m_stripY0 + H * 0.12f, x1 - x0 - H * 0.24f, H * 0.76f}, Pal("accent"), H * 0.38f);
                    const bool dim = m_wordMode == "type" && i == 0;
                    put(m_wordList[i], (x0 + x1) / 2, x1 - x0, Pal(dim ? "sub" : "text"), H * 0.48f);
                }
            }
        }
    }
    // under each hand: the key lights up, with its letter (or what an icon does) just above it
    for (int sl = 0; sl < 2; sl++) {
        Slot& s = m_slots[sl];
        if (s.hover < 0 || s.hover >= int(m_items.size())) continue;
        const Item& it = m_items[s.hover];
        if (it.k.ghost && m_wordList.empty()) continue;
        const bool pressed = now < s.pressedUntil;
        const float tint = pressed ? 0.55f : 1.0f;
        const float kh = it.H(), lw = std::max(2.f, std::floor(kh * 0.03f));
        const Color glow = Pal("glow");
        // (BetterFrame lays a white of 34/255 over the key; blended in linear light, as here, this looks the same)
        R.RectBorder({it.x0 + lw / 2, it.y0 + lw / 2, it.W() - lw, kh - lw}, Color(tint, tint, tint, 0.025f), kh * 0.2f,
                     Color(glow.r * tint, glow.g * tint, glow.b * tint, glow.a), lw);
        if (s.flick >= 0) continue;
        const Key& k = it.k;
        std::string text;
        if (k.ch || k.sym || k.ins) text = CharFor(k);
        if (!k.np.empty() && !m_st.numlock) text.clear();
        if (s.swipe || (k.emoji && k.tip.empty())) text.clear();  // the emoji itself is already big on the key
        const bool tip = !k.tip.empty();
        if (tip) text = m_conf.tips ? k.tip : "";
        else if (!m_conf.preview) text.clear();
        if (text.empty() || text == " ") continue;
        if (tip) {  // a small dark label above an icon
            const float th = kh * 0.85f, f = std::floor(th * 0.5f);
            const float w = R.Measure(text, f, Face::Keys) + th * 0.9f, cx = (it.x0 + it.x1) / 2;
            const float cy = it.y0 - (th / 2 + kh * 0.18f);
            R.RectBorder({cx - w / 2, cy - th / 2, w, th}, Color(12 / 255.f, 16 / 255.f, 22 / 255.f, 240 / 255.f), th * 0.45f,
                         Color(1, 1, 1, 50 / 255.f), std::max(1.f, th * 2 / 44));
            DrawText(R, text, cx, cy, f, Color(1, 1, 1, 1));
        } else {  // the letter in a bubble just above the key, pointing at it
            const float bh = std::min(kh, it.W() * 1.4f) * 1.05f, bw = bh * 96 / 112;
            const float cx = (it.x0 + it.x1) / 2, cyb = it.y0 - (bh / 2 + kh * 0.04f);
            const float bx = cx - bw / 2, byy = cyb - bh / 2;
            const float tail = bh * 0.10f, body = bh - tail, r = bw * 0.24f, pad = bw * 0.05f;
            R.RectFill({bx + pad, byy + pad + 1, bw - 2 * pad, body - 2 * pad}, Color(0, 0, 0, 70 / 255.f), r);
            Data::Rgba kc = m_pal.count("key") ? m_pal.at("key") : Data::Rgba{42, 55, 69, 255};
            const Color bg(std::min(255, kc[0] + 22) / 255.f, std::min(255, kc[1] + 22) / 255.f, std::min(255, kc[2] + 22) / 255.f, 1);
            auto tc = m_iconCells.find("tail");
            if (tc != m_iconCells.end()) {
                const auto& [ix, iy, iw, ih] = tc->second;
                const float tw = tail * 2.2f;
                R.ImageRegion(m_icons.id, {cx - tw / 2, byy + body - pad - 1, tw, bh - (body - pad - 1) - 1}, ix / m_iconsW,
                              iy / m_iconsH, (ix + iw) / m_iconsW, (iy + ih) / m_iconsH, Pal("glow"));
            }
            R.RectBorder({bx + pad, byy + pad, bw - 2 * pad, body - 2 * pad}, bg, r, Pal("glow"), 2);
            DrawText(R, text, cx, byy + body / 2, FitFont(R, text, body * 0.5f, (bw - 2 * pad) * 0.72f), Pal("text"));
        }
    }
    // the flick guide: the kana around the key, the one you'd get lit
    for (Slot& s : m_slots) {
        if (s.flick < 0 || s.flick >= int(m_items.size())) continue;
        const Item& it = m_items[s.flick];
        std::vector<std::string> chars = it.k.flick;
        if (m_st.shift != m_st.caps)
            for (auto& c : chars) c = d.Upper(c, d.Language(m_st.lang));
        const float cell = std::min(it.W(), it.H()) * 1.05f, cx = (it.x0 + it.x1) / 2, cy = (it.y0 + it.y1) / 2;
        const int spots[5][2] = {{1, 1}, {0, 1}, {1, 0}, {2, 1}, {1, 2}};
        for (size_t i = 0; i < chars.size() && i < 5; i++) {
            const float x = cx + (spots[i][0] - 1.5f) * cell + cell * 0.05f, y = cy + (spots[i][1] - 1.5f) * cell + cell * 0.05f;
            const float w = cell * 0.9f;
            R.RectFill({x + 2, y + 3, w, w}, Color(0, 0, 0, 90 / 255.f), cell * 0.18f);
            R.RectBorder({x, y, w, w}, Pal(int(i) == s.flickDir ? "go" : "key"), cell * 0.18f, Color(1, 1, 1, 60 / 255.f), 2);
            DrawText(R, chars[i], x + w / 2, y + w / 2, cell * 0.55f, Pal("text"));
        }
    }
    // a short message in a pop-up just above the keys
    if (!m_msg.empty() && now <= m_msgUntil && !m_items.empty()) {
        float top = 1e9f;
        for (const Item& it : m_items) top = std::min(top, it.y0);
        const float ph = m_uph * 0.62f, f = std::floor(ph * 0.42f), pad = ph * 0.42f;
        const float w = std::min(m_W * 0.82f, R.Measure(m_msg, f, Face::Keys) + pad * 2);
        const float cx = m_W / 2, cy = top - ph * 0.95f;
        R.RectBorder({cx - w / 2, cy - ph / 2, w, ph}, Color(16 / 255.f, 20 / 255.f, 27 / 255.f, 238 / 255.f), ph / 2,
                     Color(1, 1, 1, 40 / 255.f), std::max(1.f, ph * 2 / 96));
        DrawText(R, m_msg, cx - w / 2 + pad, cy, f, Pal("text"), true);
    }
}

// ---------------------------------------------------------------------------------------
// Every frame
// ---------------------------------------------------------------------------------------

void Keyboard::Build(UiRenderer& R, Window& w, float now) {
    m_now = now;
    Size(w);
    Input(w.hands, now);
    Timers(now);
    UpdateWords(now);
    Draw(R, now);
}

void Keyboard::Size(Window& w) {
    const std::string key = m_st.layout + "|" + m_st.page + "|" + m_st.lang + "|" + m_st.fmt + "|" + m_conf.bar + "|" +
                            m_conf.bottom + "|" + m_conf.theme + "|" + std::to_string(m_st.words) + "|" +
                            m_st.lgroup + std::to_string(m_st.lpage) + "|" + std::to_string(m_st.ecat) + "|" + std::to_string(m_st.epage) + "|" + std::to_string(m_st.etone) +
                            "|" + std::to_string(m_st.efav.size()) + std::to_string(m_st.erecent.size()) + "|" +
                            std::to_string(m_st.swipe) + std::to_string(m_st.curved) + std::to_string(m_st.panel);
    if (key != m_layoutKey) {
        // a new layout: no key is held across it
        for (Slot& s : m_slots) {
            s.hover = s.held = s.ehold = s.flick = -1;
            s.swipe.reset();
        }
        m_repeat.reset();
        m_shiftLock.reset();
        Layout();
        m_layoutKey = key;
    }
    SizeWindow(w);
}

int Keyboard::ItemAt(float x, float y) const {
    for (size_t i = 0; i < m_items.size(); i++) {
        const Item& it = m_items[i];
        if (x >= it.x0 && x <= it.x1 && y >= it.y0 && y <= it.y1) return int(i);
    }
    return -1;
}

void Keyboard::Input(const PointerState* hands, float now) {
    for (int sl = 0; sl < 2; sl++) {
        const PointerState& p = hands[sl];
        Slot& s = m_slots[sl];
        const int h = p.hovering ? ItemAt(p.pos.x, p.pos.y) : -1;
        if (p.hovering && !p.pressed) {
            if (s.flick >= 0) {  // flicking: where the ray goes picks the kana
                const Item& it = m_items[s.flick];
                const float dx = p.pos.x - s.flickAt.x, dy = p.pos.y - s.flickAt.y;
                int dir = 0;
                if (std::hypot(dx, dy) >= std::max(it.W(), it.H()) * 0.22f) dir = std::fabs(dx) > std::fabs(dy) ? (dx < 0 ? 1 : 3) : (dy < 0 ? 2 : 4);
                if (dir >= int(it.k.flick.size())) dir = 0;
                s.flickDir = dir;
            } else if (s.swipe) {  // swiping: follow the ray
                Swipe& sw = *s.swipe;
                sw.pts.push_back({p.pos.x, p.pos.y});
                const Pt last = sw.trail.back();  // a steadied copy for drawing: no hand-shake zigzag
                sw.trail.push_back({last.x + (p.pos.x - last.x) * 0.45f, last.y + (p.pos.y - last.y) * 0.45f});
                sw.trailT.push_back(now);
                if (h >= 0 && m_items[h].k.ch && std::find(sw.keys.begin(), sw.keys.end(), h) == sw.keys.end()) sw.keys.push_back(h);
                s.hover = h;
            } else {
                s.hover = h;
            }
        }
        if (!p.hovering && s.held < 0 && !s.swipe) s.hover = -1;
        if (p.pressed) Pressed(sl, h, now);
        if (p.released) Release(sl, now);
    }
}

void Keyboard::Pressed(int sl, int h, float now) {
    Slot& s = m_slots[sl];
    if (s.swipe || s.ehold >= 0 || s.flick >= 0) Release(sl, now);  // the last press never let go: finish it first
    if (m_repeat && m_repeat->slot != sl) {
        // a new key pressed: the old one stops repeating, like a real keyboard
        const int o = m_repeat->slot;
        if (m_slots[o].swipe && !m_slots[o].swipe->held) m_repeat.reset();
        else Release(o, now);
    }
    if (h < 0) return;
    const Key k = m_items[h].k;
    s.hover = s.held = h;
    s.pressedUntil = now + 0.09f;
    if (k.isFlick) {  // nothing types yet: where the ray goes before letting go picks the kana
        s.flick = h;
        s.flickDir = 0;
        const Item& it = m_items[h];
        s.flickAt = {(it.x0 + it.x1) / 2, (it.y0 + it.y1) / 2};
        Sound(false);
    } else if (SwipeReady(k)) {
        // swipe typing: let go without sweeping and the letter types then; sweep and only the word is typed
        auto sw = std::make_unique<Swipe>();
        const Item& it = m_items[h];
        const Pt at{(it.x0 + it.x1) / 2, (it.y0 + it.y1) / 2};
        sw->start = h;
        sw->pts = {at};
        sw->trail = {at};
        sw->trailT = {now};
        sw->keys = {h};
        sw->t = now;
        sw->shift = m_st.shift;
        sw->caps = m_st.caps;
        sw->after = m_afterLetter;
        s.swipe = std::move(sw);
        Sound(false);
        // held still on this one letter a little longer than usual (0.6 s): that's holding the key
        const std::string text = RepeatText(k);
        if (!text.empty()) m_repeat.reset(new Repeat{sl, h, text, "", now + 0.6f, -1, true});
    } else if (m_st.page == "emoji" && k.emoji && k.ins) {
        // an emoji: let go and it types; hold it and it goes in (or out of) Favorites
        s.ehold = h;
        s.eholdT = now;
        s.eholdDone = false;
        Sound(false);
    } else {
        const std::string text = RepeatText(k);
        const uint16_t fnKey = m_st.fn ? FnKey(k.act) : 0;
        Press(k);
        if (k.act == "shift" && m_st.shift) m_shiftLock.reset(new ShiftLock{sl, h, now + 0.6f});  // hold Shift: Caps Lock
        if (Repeatable(k) || fnKey == VK_HOME || fnKey == VK_END || fnKey == VK_PGUP || fnKey == VK_PGDN || fnKey == VK_VOLUP ||
            fnKey == VK_VOLDOWN)
            m_repeat.reset(new Repeat{sl, h, text, fnKey ? std::to_string(fnKey) : "", now + 0.5f});
    }
}

void Keyboard::Release(int sl, float now) {
    Slot& s = m_slots[sl];
    if (s.ehold >= 0 && !s.eholdDone && s.ehold < int(m_items.size())) {  // an emoji tapped (not held): it types now
        const Key k = m_items[s.ehold].k;
        s.ehold = -1;
        Press(k, true);
    }
    s.ehold = -1;
    if (s.held >= 0 && s.held < int(m_items.size()) && m_items[s.held].k.act == "shift" && m_shiftTyped) {
        // let go of a Shift held while typing: it turns off, like a real keyboard
        m_shiftTyped = false;
        m_st.shift = false;
    }
    s.held = -1;
    if (s.flick >= 0) FinishFlick(sl);
    if (s.swipe) FinishSwipe(sl, now);
    if (m_repeat && m_repeat->slot == sl) m_repeat.reset();
}

void Keyboard::Timers(float now) {
    Repeat* r = m_repeat.get();
    Swipe* sw = r ? m_slots[r->slot].swipe.get() : nullptr;
    if (sw && !sw->held && RealSwipe(*sw)) {
        m_repeat.reset();  // it turned into a swipe: no repeating
        r = nullptr;
    }
    if (r && sw && !sw->held && now >= r->next && !HeldStill(*sw, r->item)) {
        m_repeat.reset();  // the ray has left the first letter: a slow swipe, not a held key
        r = nullptr;
    }
    if (r && now >= r->next) {  // hold a key to repeat it
        if (sw) sw->held = true;  // held still on one letter: that's repeating, not a swipe
        const Slot& s = m_slots[r->slot];
        if (s.held == r->item && s.hover == r->item && r->item < int(m_items.size())) {
            const Key k = m_items[r->item].k;
            if (!r->fn.empty()) {
                const uint16_t vk = uint16_t(std::stoi(r->fn));
                m_out.key(vk, true);
                m_out.key(vk, false);
            } else if (!r->text.empty() && m_conf.hold == "shift") {
                // held: the capital (or the key's other character) instead, once
                std::string alt = HeldVariant(k, r->text);
                if (alt.empty()) alt = r->text;
                if (r->first) {
                    TypeText(alt);
                    Sound(false);
                    m_lastPress = now;
                    if (m_st.shift && !ShiftHeld()) m_st.shift = false;
                } else if (alt != r->text) {
                    Tap(VK_BACK, false, false);
                    TypeText(alt);
                    Sound(false);
                }
                m_repeat.reset();
                r = nullptr;
            } else if (!r->text.empty()) {
                Sound(false);
                TypeText(r->text);
                if (r->first) {  // a swipe-typing letter: its first one now, repeats after a moment
                    r->first = false;
                    r->next = now + 0.3f;
                    m_lastPress = now;
                    if (m_st.shift && !ShiftHeld()) m_st.shift = false;
                }
            } else {
                Press(k);
            }
            if (m_repeat && r && r->next <= now) r->next = now + 0.05f;
        } else {
            m_repeat.reset();
        }
    }
    for (Slot& s : m_slots) {  // an emoji held for half a second: Favorites
        if (s.ehold < 0 || s.eholdDone || now - s.eholdT < 0.55f) continue;
        s.eholdDone = true;
        if (s.hover != s.ehold || s.ehold >= int(m_items.size())) continue;  // (slid off it: nothing)
        const std::string v = m_items[s.ehold].k.ins.value_or("");
        if (v.empty()) continue;
        auto& favs = m_conf.fav;
        auto it = std::find(favs.begin(), favs.end(), v);
        const bool added = it == favs.end();
        if (added) {
            favs.insert(favs.begin(), v);
            if (favs.size() > 60) favs.resize(60);
        } else {
            favs.erase(it);
        }
        m_st.efav = favs;
        Save();
        Sound(true);
        Flash(added ? "Added to Favorites" : "Removed from Favorites", 1.5f);
    }
    if (m_shiftLock && now >= m_shiftLock->at) {
        const ShiftLock sl = *m_shiftLock;
        m_shiftLock.reset();
        if (m_slots[sl.slot].held == sl.item && m_slots[sl.slot].hover == sl.item && m_st.shift && !m_st.caps && !m_shiftTyped) {
            m_st.caps = true;  // Shift held: Caps Lock
            m_st.shift = false;
        }
    }
}

// ---------------------------------------------------------------------------------------
// What keys do (_press)
// ---------------------------------------------------------------------------------------

const std::map<std::string, std::string>& Keyboard::LayerMap() const {
    static const std::map<std::string, std::string> none;
    const Lang& lang = Data::Get().Language(m_st.lang);
    return m_st.alt >= 0 && m_st.alt < int(lang.layers.size()) ? lang.layers[m_st.alt] : none;
}

std::string Keyboard::CharFor(const Key& k) const {
    const Data& d = Data::Get();
    const Lang& lang = d.Language(m_st.lang);
    if (k.ch) {
        const auto& layer = LayerMap();
        auto it = layer.find(*k.ch);
        std::string c = it != layer.end() ? it->second : *k.ch;
        if (m_st.shift != m_st.caps) c = d.Shifted(c, lang);
        return d.Types(c);
    }
    if (k.sym) {
        if (m_st.shift) return d.Types((*k.sym)[1]);
        const std::string& a = (*k.sym)[0];
        return m_st.caps && d.IsAlpha(a) ? d.Upper(a, lang) : a;
    }
    return k.ins.value_or("");
}

bool Keyboard::Repeatable(const Key& k) const {
    return ((k.ch || k.sym || k.ins) && !k.emoji) || k.act == "back" || k.act == "del" || k.act == "left" || k.act == "right" ||
           k.act == "up" || k.act == "down" || k.act == "space" || k.act == "pgup" || k.act == "pgdn";
}

std::string Keyboard::HeldVariant(const Key& k, const std::string& typed) const {
    const Data& d = Data::Get();
    if (k.sym) {
        const std::string other = typed == (*k.sym)[0] ? (*k.sym)[1] : (*k.sym)[0];
        return d.Types(other);
    }
    if (Length(typed) == 1 && d.IsAlpha(typed)) {
        const std::string up = d.Shifted(typed, d.Language(m_st.lang));
        return up != typed ? up : d.LowerWord(typed);
    }
    return "";
}

std::string Keyboard::RepeatText(const Key& k) const {
    const Lang& lang = Data::Get().Language(m_st.lang);
    if (m_st.ctrl || m_st.altm || lang.romaji || !lang.ime.empty()) return "";  // (a held key presses again instead)
    return CharFor(k);
}

bool Keyboard::LockTap(const std::string& name, bool on, float now) {
    // one tap is for the next key; a quick second tap locks it on; a tap while locked lets it go
    const bool quick = now - m_lockT[name] < 0.4f;
    m_lockT[name] = now;
    if (m_st.locks.count(name)) {
        m_st.locks.erase(name);
        return false;
    }
    if (on && quick) {
        m_st.locks.insert(name);
        return true;
    }
    return !on;
}

void Keyboard::ModsUsed() {
    if (!m_st.locks.count("fn")) m_st.fn = false;
    if (!m_st.locks.count("ctrl")) m_st.ctrl = false;
    if (!m_st.locks.count("alt-mod")) m_st.altm = false;
    if (!m_st.locks.count("super")) m_st.meta = false;
    if (!m_st.caps) m_st.shift = false;
}

bool Keyboard::ShiftHeld() const {
    // a hand holding Shift down right now, while still pointing at it
    for (const Slot& s : m_slots)
        if (s.held >= 0 && s.held < int(m_items.size()) && m_items[s.held].k.act == "shift" && s.hover == s.held) return true;
    return false;
}

void Keyboard::Flash(const std::string& msg, float secs) {
    m_msg = msg;
    m_msgUntil = m_now + secs;
}

void Keyboard::Sound(bool big) {
    if (m_conf.sound != "off") m_clicks.Play(m_conf.sound, big);
}

void Keyboard::Tap(uint16_t vk, bool withMods, bool withShift) {
    std::vector<uint16_t> mods;
    if (withMods) {
        if (m_st.ctrl) mods.push_back(VK_CTRL);
        if (m_st.altm) mods.push_back(VK_ALT);
        if (m_st.meta) mods.push_back(VK_WIN);
    }
    if (withShift && m_st.shift) mods.push_back(VK_SHIFT);
    for (uint16_t m : mods) m_out.key(m, true);
    m_out.key(vk, true);
    m_out.key(vk, false);
    for (auto it = mods.rbegin(); it != mods.rend(); ++it) m_out.key(*it, false);
}

void Keyboard::Combo(std::initializer_list<uint16_t> keys) {
    for (uint16_t k : keys) m_out.key(k, true);
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) m_out.key(*it, false);
}

void Keyboard::TypeText(const std::string& s) {
    if (!s.empty()) m_out.text(s);
}

void Keyboard::ReplaceTyped(size_t n, const std::string& text) {
    for (size_t i = 0; i < n; i++) Tap(VK_BACK, false, false);
    TypeText(text);
}

void Keyboard::Press(const Key& k, bool quiet) {
    Data& d = Data::Get();
    const Lang& lang = d.Language(m_st.lang);
    m_prevAct = m_curAct;
    m_curAct = k.act;
    m_lastPress = m_now;
    if (!quiet) Sound(k.act == "space" || k.act == "enter");
    const bool mods = m_st.ctrl || m_st.altm || m_st.meta;
    if (!lang.ime.empty() && ComposePress(k)) return;
    if (!StartsWith(k.act, "sugg:")) {
        NoteWord(k);
        if (m_autoSpace) {
            m_autoSpace = false;
            const std::string c = (k.ch || k.sym || k.ins) ? CharFor(k) : "";
            if (!c.empty() && std::string(".,!?;:").find(c) != std::string::npos && c.size() == 1) {  // "word." not "word ."
                Tap(VK_BACK, false, false);
                TypeText(c + " ");
                return;
            }
        }
    }
    if (!k.np.empty() && !m_st.numlock) {  // the numpad with Num Lock off: 7 Home, 8 up, ...
        if (const uint16_t vk = NumpadKey(k.np)) Tap(vk, true, false);
        return;
    }
    if (m_st.fn && d.HasFn(k.act)) {  // Fn + key: Home / End / PgUp / PgDn / volume / media / Insert
        Tap(FnKey(k.act), true, true);
        ModsUsed();
        return;
    }
    if (m_st.fn && m_st.layout == "minimal" && k.ch && k.ch->size() == 1 && isdigit(static_cast<unsigned char>((*k.ch)[0]))) {
        const int n = (*k.ch)[0] - '0';  // Fn + a number on Minimal: F1 to F10
        Tap(uint16_t(VK_F1 + (n ? n : 10) - 1), true, true);
        ModsUsed();
        return;
    }
    if (k.ch || k.sym || k.ins) {
        const std::string c = CharFor(k);
        if (k.ch && m_st.alt >= 0) m_st.alt = -1;  // an accent was picked: back to plain letters
        if (lang.romaji && !mods) {
            if (k.ch && c.size() == 1 && c[0] >= 'a' && c[0] <= 'z') {
                TypeText(c);
                auto [n, kana] = RomajiFeed(c);
                if (n) ReplaceTyped(n, kana);
            } else {
                auto [n, kana] = RomajiFlush();
                if (n) ReplaceTyped(n, kana);
                TypeText(d.JaPunct(c));
            }
        } else if (mods && c.size() == 1 && isalnum(static_cast<unsigned char>(c[0]))) {
            Tap(uint16_t(toupper(static_cast<unsigned char>(c[0]))), true, true);  // Ctrl + letter: a shortcut
        } else {
            TypeText(c);
        }
        m_lastChar = Length(c) == 1 ? c : "";
        // a word or sentence just ended right here: a swiped word next gets a space first
        m_afterLetter = Length(c) == 1 && (d.IsAlpha(c) || d.IsDigit(c) || std::string(".,!?;:)").find(c) != std::string::npos);
        if (k.emoji && !c.empty()) {  // for the Recent tab
            auto& rec = m_conf.recent;
            rec.erase(std::remove(rec.begin(), rec.end(), c), rec.end());
            rec.insert(rec.begin(), c);
            if (rec.size() > 30) rec.resize(30);
            Save();
        }
    } else {
        const std::string& a = k.act;
        if (lang.romaji) {
            if (a == "back") {
                if (!m_romajiBuf.empty()) m_romajiBuf.pop_back();
            } else if (a == "space" || a == "enter" || a == "tab") {
                auto [n, kana] = RomajiFlush();
                if (n) ReplaceTyped(n, kana);
            } else {
                m_romajiBuf.clear();
            }
        }
        if (const uint16_t vk = Named(a)) {
            m_afterLetter = false;
            Tap(vk, true, a != "space");
            if (m_st.shift && a != "space" && !m_st.locks.count("shift")) m_st.shift = false;  // (Shift is for one key)
        } else if (a == "shift") {  // tap twice quickly for Caps Lock
            m_shiftTyped = false;
            if (m_st.caps) {
                m_st.caps = m_st.shift = false;
            } else if (m_now - m_lastShift < 0.35f && m_st.shift && m_prevAct == "shift") {
                m_st.caps = true;
                m_st.shift = false;
            } else {
                m_st.shift = !m_st.shift;
            }
            m_lastShift = m_now;
        } else if (a == "caps") {
            m_st.caps = !m_st.caps;
            m_st.shift = false;
        } else if (a == "ctrl") {
            m_st.ctrl = LockTap("ctrl", m_st.ctrl, m_now);
        } else if (a == "swipe") {
            m_st.swipe = !m_st.swipe;
            Save();
            if (m_st.swipe && SwipeLang().empty()) Flash("Swipe typing isn't available for " + lang.name + " yet");
        } else if (StartsWith(a, "sugg:")) {
            PickSuggestion(std::stoi(a.substr(5)));
            return;
        } else if (a == "dakuten" || a == "small" || a == "flickmod") {  // change the kana just typed: か → が
            const std::string n = a == "dakuten" ? d.Dakuten(m_lastChar) : a == "small" ? d.Small(m_lastChar) : d.FlickMod(m_lastChar);
            if (!m_lastChar.empty() && !n.empty()) {
                ReplaceTyped(1, n);
                m_lastChar = n;
            }
        } else if (a == "fn") {
            m_st.fn = LockTap("fn", m_st.fn, m_now);
        } else if (a == "start") {  // the Windows key
            Tap(VK_WIN, false, false);
        } else if (a == "numlock") {
            m_st.numlock = !m_st.numlock;
        } else if (a == "alt-mod") {
            m_st.altm = LockTap("alt-mod", m_st.altm, m_now);
        } else if (a == "alt") {  // the accent key: the next set of accented letters, then back to plain
            const int n = int(lang.layers.size());
            m_st.alt = m_st.alt + 1 < n ? m_st.alt + 1 : -1;
        } else if (StartsWith(a, "lang:")) {
            m_st.lang = a.substr(5);
            m_st.page = "abc";
            m_st.alt = -1;
            m_romajiBuf.clear();
            m_units.clear();
            m_composed.clear();
            Save();
        } else if (StartsWith(a, "lgroup:")) {
            m_st.lgroup = a.substr(7);
        } else if (a == "lback") {
            m_st.lgroup.clear();
        } else if (StartsWith(a, "lpage:")) {
            m_st.lpage = std::max(0, m_st.lpage + std::stoi(a.substr(6)));
        } else if (StartsWith(a, "fmt:")) {
            m_st.fmt = a.substr(4);
            m_st.page = "abc";
            m_st.alt = -1;
            Save();
        } else if (a == "p123") {
            m_st.page = "123";
        } else if (a == "psym") {
            m_st.page = "sym";
        } else if (a == "abc") {
            m_st.page = "abc";
        } else if (a == "undo" || a == "cut" || a == "copy" || a == "paste" || a == "selall") {
            const uint16_t letter = a == "undo" ? 'Z' : a == "cut" ? 'X' : a == "copy" ? 'C' : a == "paste" ? 'V' : 'A';
            Combo({VK_CTRL, letter});
        } else if (a == "etone") {  // skin tone: none, then light to dark
            m_st.etone = m_st.etone < 4 ? m_st.etone + 1 : -1;
            Save();
        } else if (a == "emoji" && m_st.page == "emoji") {  // the emoji button again: back to the letters
            m_st.page = "abc";
        } else if (a == "emoji") {
            m_st.page = "emoji";
            m_st.epage = 0;
            m_st.erecent = m_conf.recent;
            m_st.efav = m_conf.fav;
            // Recent first once it has a row's worth, then Favorites if you have any, otherwise the faces
            m_st.ecat = int(m_st.erecent.size()) >= (m_st.layout == "minimal" ? 10 : 14) ? 0 : !m_st.efav.empty() ? Data::kFavTab : 1;
        } else if (StartsWith(a, "ecat:")) {
            m_st.ecat = std::stoi(a.substr(5));
            m_st.epage = 0;
            m_st.erecent = m_conf.recent;
            m_st.efav = m_conf.fav;
        } else if (StartsWith(a, "epage:")) {
            m_st.epage = std::max(0, std::min(m_st.epage, d.EmojiPages() - 1) + std::stoi(a.substr(6)));
        } else if (a == "curve") {
            m_st.curved = !m_st.curved;
            Save();
        } else if (a == "panel") {
            m_st.panel = !m_st.panel;
            Save();
        } else if (StartsWith(a, "lay-")) {
            m_st.layout = a.substr(4);
            m_st.page = "abc";
            Save();
        } else if (a == "recenter") {
            if (m_out.recenter) m_out.recenter();
        } else if (a == "hide") {
            if (m_out.hide) m_out.hide();
        }
    }
    if ((k.ch || k.sym) && m_st.shift) {
        if (ShiftHeld()) m_shiftTyped = true;  // Shift held down with the other hand: on until it's let go
        else m_st.shift = false;
    }
    if (m_st.fn && k.act != "fn" && k.act != "ctrl" && k.act != "alt-mod" && k.act != "shift" && k.act != "caps" &&
        !m_st.locks.count("fn"))
        m_st.fn = false;  // Fn is for one key
    if ((m_st.ctrl || m_st.altm || m_st.meta) && k.act != "ctrl" && k.act != "alt-mod" && k.act != "super" && k.act != "shift" &&
        k.act != "caps" && k.act != "fn") {
        // Ctrl / Alt / Super apply to the next key, then switch off (unless locked on with a double tap)
        if (!m_st.locks.count("ctrl")) m_st.ctrl = false;
        if (!m_st.locks.count("alt-mod")) m_st.altm = false;
        if (!m_st.locks.count("super")) m_st.meta = false;
    }
}

// ---------------------------------------------------------------------------------------
// Japanese typed in English letters (languages.Romaji)
// ---------------------------------------------------------------------------------------

std::pair<size_t, std::string> Keyboard::RomajiFeed(const std::string& c) {
    const auto& kana = Data::Get().Romaji();
    static std::set<std::string> prefixes;
    if (prefixes.empty()) {
        for (const auto& [k, v] : kana)
            for (size_t i = 1; i <= k.size(); i++) prefixes.insert(k.substr(0, i));
        prefixes.insert("tc");
    }
    m_romajiBuf += char(tolower(static_cast<unsigned char>(c[0])));
    const size_t onScreen = m_romajiBuf.size();
    std::string out;
    while (!m_romajiBuf.empty()) {
        const std::string b = m_romajiBuf;
        bool longer = false;
        for (const auto& [k, v] : kana) longer = longer || (k != b && k.rfind(b, 0) == 0);
        const auto vowel = [](char x) { return std::string("aiueo").find(x) != std::string::npos; };
        if (kana.count(b) && !longer) {
            out += kana.at(b);
            m_romajiBuf.clear();
        } else if ((b.size() >= 2 && b[0] == b[1] && std::string("aiueon").find(b[0]) == std::string::npos && isalpha(b[0])) ||
                   b.rfind("tch", 0) == 0) {
            out += "っ";  // double consonant: small tsu
            m_romajiBuf = b.substr(1);
        } else if (b.rfind("nn", 0) == 0) {  // nn: ん (a lone nn waits)
            if (b.size() == 2) break;
            if (b[2] == '\'') {
                out += "ん";
                m_romajiBuf = b.substr(3);
                continue;
            }
            const bool v = vowel(b[2]) || b[2] == 'y';
            out += "ん";
            m_romajiBuf = v ? b.substr(1) : b.substr(2);
        } else if (b[0] == 'n' && b.size() >= 2 && !vowel(b[1]) && b[1] != 'y' && b[1] != '\'') {  // n + consonant: ん
            out += "ん";
            m_romajiBuf = b.substr(1);
        } else if (prefixes.count(b)) {
            if (kana.count(b) && b.size() == 1) {  // a lone vowel never starts anything longer
                out += kana.at(b);
                m_romajiBuf.clear();
            }
            break;  // wait for more letters
        } else {  // no kana starts like this: the first letter stays as typed
            out += b[0];
            m_romajiBuf = b.substr(1);
        }
    }
    if (out.empty()) return {0, ""};
    return {onScreen, out + m_romajiBuf};
}

std::pair<size_t, std::string> Keyboard::RomajiFlush() {
    if (m_romajiBuf == "n" || m_romajiBuf == "nn") {
        const size_t n = m_romajiBuf.size();
        m_romajiBuf.clear();
        return {n, "ん"};
    }
    m_romajiBuf.clear();
    return {0, ""};
}

// ---------------------------------------------------------------------------------------
// Korean and Vietnamese: composed as you type (ime.hangul / ime.telex). BetterFrame shows the
// word being composed in its word bar until it's done; here it's in the app typed into, kept
// up to date by replacing it.
// ---------------------------------------------------------------------------------------

std::string Keyboard::Compose() const {
    const Data& d = Data::Get();
    const Lang& lang = d.Language(m_st.lang);
    if (lang.ime == "ko") {
        const auto& t = d.HangulTables();
        auto index = [](const std::vector<std::string>& v, const std::string& c) {
            auto it = std::find(v.begin(), v.end(), c);
            return it == v.end() ? -1 : int(it - v.begin());
        };
        std::string out, cho, jung, jong;
        auto flush = [&] {
            const int ci = index(t.cho, cho), ji = index(t.jung, jung), ki = index(t.jong, jong);
            if (!cho.empty() && !jung.empty() && ci >= 0 && ji >= 0 && ki >= 0) {
                const uint32_t cp = 0xAC00 + (ci * 21 + ji) * 28 + ki;
                out += {char(0xE0 | (cp >> 12)), char(0x80 | ((cp >> 6) & 0x3F)), char(0x80 | (cp & 0x3F))};
            } else {
                out += cho + jung + jong;
            }
            cho.clear();
            jung.clear();
            jong.clear();
        };
        std::map<std::string, std::pair<std::string, std::string>> split;
        for (const auto& [ab, v] : t.cc) {
            const auto cs = Chars(ab);
            split[v] = {cs[0], cs[1]};
        }
        for (const std::string& c : m_units) {
            if (index(t.jung, c) >= 0) {  // a vowel
                if (!jong.empty()) {  // the last consonant moves on to start the new syllable
                    std::string keep, move = jong;
                    if (auto s = split.find(jong); s != split.end()) {
                        keep = s->second.first;
                        move = s->second.second;
                    }
                    jong = keep;
                    flush();
                    cho = move;
                    jung = c;
                } else if (!jung.empty() && t.vv.count(jung + c)) {
                    jung = t.vv.at(jung + c);
                } else if (!cho.empty() && jung.empty()) {
                    jung = c;
                } else {
                    flush();
                    jung = c;
                }
            } else if (index(t.cho, c) >= 0 || index(t.jong, c) >= 0) {  // a consonant
                if (!cho.empty() && !jung.empty() && jong.empty() && index(t.jong, c) >= 0) {
                    jong = c;
                } else if (!cho.empty() && !jung.empty() && !jong.empty() && t.cc.count(jong + c)) {
                    jong = t.cc.at(jong + c);
                } else {
                    if (!cho.empty() || !jung.empty() || !jong.empty()) flush();
                    cho = c;
                }
            } else {
                if (!cho.empty() || !jung.empty() || !jong.empty()) flush();
                out += c;
            }
        }
        if (!cho.empty() || !jung.empty() || !jong.empty()) flush();
        return out;
    }
    // Vietnamese, the Telex way: aa â, aw ă, ee ê, oo ô, ow ơ, uw ư, dd đ; s f r x j the tones
    const std::vector<std::string> vowels = Chars(d.ViVowels());
    const auto isVowel = [&](const std::string& c) { return std::find(vowels.begin(), vowels.end(), d.LowerWord(c)) != vowels.end(); };
    const Lang& en = d.Language("en-US");
    std::vector<std::string> out;
    std::string tone;
    for (const std::string& k : m_units) {
        const std::string lk = d.LowerWord(k);
        const bool up = k != lk;
        const std::string prev = out.empty() ? "" : out.back(), lp = d.LowerWord(prev);
        const bool prevUp = !prev.empty() && prev != lp;
        if (std::string("sfrxj").find(lk) != std::string::npos && lk.size() == 1 && std::any_of(out.begin(), out.end(), isVowel)) {
            if (tone == lk) {  // twice: the letter itself, no tone
                tone.clear();
                out.push_back(k);
            } else {
                tone = lk;
            }
            continue;
        }
        if (lk == "z" && !tone.empty()) {
            tone.clear();
            continue;
        }
        if (lk == "d" && lp == "d") {
            out.back() = prevUp ? "Đ" : "đ";
            continue;
        }
        if ((lk == "a" || lk == "e" || lk == "o") && lp == lk) {
            const std::string r = lk == "a" ? "â" : lk == "e" ? "ê" : "ô";
            out.back() = prevUp ? d.Upper(r, en) : r;
            continue;
        }
        if (lk == "w") {  // uo + w: ươ; a: ă; o: ơ; u: ư; nothing to change: ư
            std::vector<std::string> low;
            for (const auto& c : out) low.push_back(d.LowerWord(c));
            int j = -1, i = -1;
            for (int p = int(low.size()) - 1; p >= 0 && j < 0; p--)
                if (low[p] == "a" || low[p] == "o" || low[p] == "u") j = p;
            for (int p = int(low.size()) - 2; p >= 0 && i < 0; p--)
                if (low[p] == "u" && low[p + 1] == "o") i = p;
            auto noVowelAfter = [&](int from) {
                for (size_t p = size_t(from); p < low.size(); p++)
                    if (isVowel(low[p])) return false;
                return true;
            };
            if (i >= 0 && noVowelAfter(i + 2)) {
                out[i] = out[i] != low[i] ? "Ư" : "ư";
                out[i + 1] = out[i + 1] != low[i + 1] ? "Ơ" : "ơ";
                continue;
            }
            if (j >= 0 && noVowelAfter(j + 1)) {
                const std::string r = low[j] == "a" ? "ă" : low[j] == "o" ? "ơ" : "ư";
                out[j] = out[j] != low[j] ? d.Upper(r, en) : r;
                continue;
            }
            out.push_back(up ? "Ư" : "ư");
            continue;
        }
        out.push_back(k);
    }
    if (!tone.empty()) {  // the tone mark on the right vowel (the modern rule)
        std::vector<std::string> low;
        for (const auto& c : out) low.push_back(d.LowerWord(c));
        int start = -1;
        for (size_t p = 0; p < low.size() && start < 0; p++)
            if (isVowel(low[p])) start = int(p);
        if (start >= 0) {
            int end = start;
            while (end + 1 < int(low.size()) && isVowel(low[end + 1])) end++;
            if (((low[start] == "u" && start > 0 && low[start - 1] == "q") || (low[start] == "i" && start > 0 && low[start - 1] == "g")) &&
                end > start)
                start++;
            static const std::string special = "ăâêôơư";
            int at = -1;
            for (int p = start; p <= end; p++)
                if (special.find(low[p]) != std::string::npos) at = p;
            if (at < 0) {
                std::string cl;
                for (int p = start; p <= end; p++) cl += low[p];
                if (start == end) at = start;
                else if (end + 1 < int(low.size())) at = end;
                else if (end - start == 1) at = (cl == "oa" || cl == "oe" || cl == "uy") ? start + 1 : start;
                else at = start + 1;
            }
            const std::string t = d.ViTone(out[at], tone);
            if (!t.empty()) out[at] = t;
        }
    }
    std::string word;
    for (const auto& c : out) word += c;
    return word;
}

bool Keyboard::ComposePress(const Key& k) {
    const Lang& lang = Data::Get().Language(m_st.lang);
    if (lang.ime != "ko" && lang.ime != "vi") return false;
    const bool mods = m_st.ctrl || m_st.altm || m_st.meta;
    auto update = [&] {
        const std::string now = Compose();
        ReplaceTyped(Length(m_composed), now);
        m_composed = now;
    };
    if (k.ch && !mods && (lang.ime == "ko" || Data::Get().IsAlpha(CharFor(k)))) {
        m_units.push_back(CharFor(k));
        update();
        if (m_st.shift) {
            if (ShiftHeld()) m_shiftTyped = true;
            else m_st.shift = false;
        }
        return true;
    }
    if (k.act == "back" && !m_units.empty()) {
        m_units.pop_back();
        update();
        if (m_units.empty()) m_composed.clear();
        return true;
    }
    if (k.act != "shift" && k.act != "caps") {  // anything else ends the word
        m_units.clear();
        m_composed.clear();
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Swipe typing and the word bar
// ---------------------------------------------------------------------------------------

std::string Keyboard::SwipeLang() const {
    const Lang& lang = Data::Get().Language(m_st.lang);
    if (lang.kana || lang.romaji || !lang.ime.empty()) return "";
    const std::string code = lang.code.substr(0, lang.code.find('-'));
    return m_decoder && m_decoder->Available(code) ? code : "";
}

bool Keyboard::SwipeReady(const Key& k) const {
    return m_st.swipe && k.ch && m_st.page == "abc" && m_st.alt < 0 && !(m_st.ctrl || m_st.altm || m_st.meta) && !SwipeLang().empty();
}

float Keyboard::KeyW() const {
    float s = 0;
    int n = 0;
    for (const Item& it : m_items)
        if (it.k.ch) {
            s += it.W();
            n++;
        }
    return n ? s / n : 60;
}

bool Keyboard::RealSwipe(Swipe& sw) const {
    if (sw.real) return true;
    float length = 0;
    for (size_t i = 1; i < sw.pts.size(); i++) length += std::hypot(sw.pts[i].x - sw.pts[i - 1].x, sw.pts[i].y - sw.pts[i - 1].y);
    if (sw.keys.size() >= 2 && length >= KeyW() * 1.3f) sw.real = true;
    return sw.real;
}

bool Keyboard::DrawSwipe(Swipe& sw) const {
    if (RealSwipe(sw)) return true;
    if (sw.keys.size() < 2) return false;
    float length = 0;
    for (size_t i = 1; i < sw.pts.size(); i++) length += std::hypot(sw.pts[i].x - sw.pts[i - 1].x, sw.pts[i].y - sw.pts[i - 1].y);
    return length >= KeyW() * 0.8f;
}

bool Keyboard::HeldStill(const Swipe& sw, int item) const {
    if (item >= int(m_items.size())) return false;
    const Item& it = m_items[item];
    const float pad = it.W() * 0.15f;
    for (size_t i = sw.pts.size() > 12 ? sw.pts.size() - 12 : 0; i < sw.pts.size(); i++) {
        const Pt& p = sw.pts[i];
        if (p.x < it.x0 - pad || p.x > it.x1 + pad || p.y < it.y0 - pad || p.y > it.y1 + pad) return false;
    }
    return sw.keys.size() < 2;
}

std::map<std::string, Pt> Keyboard::SwipeKeys(float* kw) const {
    std::map<std::string, Pt> keys;
    float s = 0;
    int n = 0;
    for (const Item& it : m_items)
        if (it.k.ch) {
            keys[*it.k.ch] = {(it.x0 + it.x1) / 2, (it.y0 + it.y1) / 2};
            s += it.W();
            n++;
        }
    if (kw) *kw = n ? s / n : 60;
    return keys;
}

void Keyboard::FinishSwipe(int sl, float now) {
    std::unique_ptr<Swipe> sw = std::move(m_slots[sl].swipe);
    if (!sw || sw->held) return;  // (a held, repeating letter: already typed)
    float kw = 60;
    const auto keys = SwipeKeys(&kw);
    float length = 0;
    for (size_t i = 1; i < sw->pts.size(); i++) length += std::hypot(sw->pts[i].x - sw->pts[i - 1].x, sw->pts[i].y - sw->pts[i - 1].y);
    const bool hasStart = sw->start < int(m_items.size());
    const Key start = hasStart ? m_items[sw->start].k : Key{};
    // a tap where the ray drifted a little is still a tap: only a real sweep counts as a swipe
    if (sw->keys.size() < 2 || length < kw * 1.3f || (sw->keys.size() < 3 && length < kw * 2.0f)) {
        if (hasStart) {
            m_slots[sl].pressedUntil = now + 0.09f;
            Press(start, true);  // (its click already played when it was pressed)
        }
        return;
    }
    const std::string lang2 = SwipeLang();
    if (lang2.empty()) {
        if (hasStart) Press(start, true);
        return;
    }
    std::vector<std::string> words;
    {
        std::lock_guard<std::mutex> l(m_decoder->Lock());
        words = m_decoder->Decode(lang2, sw->pts, keys, kw);
    }
    m_previewWord.clear();
    if (words.empty()) {
        Flash("Didn't catch that word");
        return;
    }
    const Data& d = Data::Get();
    const Lang& lang = d.Language(m_st.lang);
    for (std::string& w : words) {  // Shift / Caps Lock apply to swiped words too
        if (sw->caps) {
            w = d.UpperWord(w, lang);
        } else if (sw->shift) {
            auto cs = Chars(w);
            cs[0] = d.Upper(cs[0], lang);
            w.clear();
            for (auto& c : cs) w += c;
        }
    }
    // a word typed by tapping just before: a space between it and the swiped one
    TypeText(std::string(sw->after ? " " : "") + words[0] + " ");
    m_afterLetter = false;
    m_lastSwipe = words[0] + " ";
    m_autoSpace = true;
    if (m_st.shift) {
        if (ShiftHeld()) m_shiftTyped = true;
        else m_st.shift = false;
    }
    // with the word bar on, the next best guesses wait there in case it picked the wrong one
    m_wordList.assign(words.begin(), words.begin() + std::min<size_t>(3, words.size()));
    m_wordMode = "swipe";
    m_wordSel = 0;
    m_curWord.clear();
    m_lastPress = now;
}

void Keyboard::PickSuggestion(int i) {
    if (!m_st.words || i < 0 || i >= int(m_wordList.size())) return;
    Sound(false);
    const std::string w = m_wordList[i];
    if (m_wordMode == "swipe") {  // swap the swiped word for this one
        if (m_lastSwipe.empty() || i == m_wordSel) return;
        ReplaceTyped(Length(m_lastSwipe), w + " ");
        m_lastSwipe = w + " ";
        m_wordSel = i;
        m_autoSpace = true;
    } else {  // finish the word being typed with this one
        ReplaceTyped(Length(m_curWord), w + " ");
        m_lastSwipe = w + " ";
        m_autoSpace = true;
        m_wordList.clear();
        m_curWord.clear();
    }
    if (m_st.shift && !m_st.caps) m_st.shift = false;
}

void Keyboard::NoteWord(const Key& k) {
    if (!m_st.words) return;
    const Data& d = Data::Get();
    const std::string c = k.ch ? CharFor(k) : "";
    if (Length(c) == 1 && (d.IsAlpha(c) || c == "'") && !(m_st.ctrl || m_st.altm || m_st.meta)) {
        m_curWord += c;
    } else if (k.act == "back") {
        auto cs = Chars(m_curWord);
        if (!cs.empty()) cs.pop_back();
        m_curWord.clear();
        for (auto& x : cs) m_curWord += x;
    } else if (k.act == "shift" || k.act == "caps" || k.act == "panel" || k.act == "curve" || k.act == "swipe") {
        return;  // (doesn't end the word)
    } else {
        m_curWord.clear();
    }
    m_wordMode = "type";
    m_wordSel = -1;
    m_wordsDue = true;
}

void Keyboard::UpdateWords(float now) {
    if (!m_st.words) return;
    Swipe* sw = nullptr;
    for (Slot& s : m_slots)
        if (s.swipe) sw = s.swipe.get();
    const bool swiping = sw && sw->pts.size() > 1 && RealSwipe(*sw);
    if (swiping) {  // the word the swipe is spelling, as it goes
        if (now - m_previewT > 0.12f && sw->pts.size() != m_previewN) {
            const std::string lang2 = SwipeLang();
            if (!lang2.empty()) {
                float kw;
                const auto keys = SwipeKeys(&kw);
                m_previewT = now;
                m_previewN = sw->pts.size();
                std::vector<std::string> ws;
                {
                    std::lock_guard<std::mutex> l(m_decoder->Lock());
                    ws = m_decoder->Decode(lang2, sw->pts, keys, kw, 1);
                }
                std::string w = ws.empty() ? "" : ws[0];
                const Data& d = Data::Get();
                const Lang& lang = d.Language(m_st.lang);
                if (!w.empty() && sw->caps) w = d.UpperWord(w, lang);
                else if (!w.empty() && sw->shift) {
                    auto cs = Chars(w);
                    cs[0] = d.Upper(cs[0], lang);
                    w.clear();
                    for (auto& c : cs) w += c;
                }
                m_previewWord = w;
            }
        }
    } else {
        m_previewWord.clear();
    }
    if (m_wordsDue && m_wordMode == "type") {
        m_wordsDue = false;
        const std::string lang2 = SwipeLang();
        std::vector<std::string> comps;
        if (!lang2.empty() && !m_curWord.empty()) {
            const Data& d = Data::Get();
            const Lang& lang = d.Language(m_st.lang);
            const std::string low = d.LowerWord(m_curWord);
            const bool allUpper = Length(m_curWord) > 1 && d.UpperWord(m_curWord, lang) == m_curWord && low != m_curWord;
            const std::string first = Chars(m_curWord)[0];
            const bool firstUpper = d.LowerWord(first) != first;
            for (std::string w : m_decoder->Completions(lang2, low, 2)) {
                if (allUpper) {
                    w = d.UpperWord(w, lang);
                } else if (firstUpper) {
                    auto cs = Chars(w);
                    cs[0] = d.Upper(cs[0], lang);
                    w.clear();
                    for (auto& c : cs) w += c;
                }
                if (w != m_curWord && std::find(comps.begin(), comps.end(), w) == comps.end()) comps.push_back(w);
            }
            if (comps.size() > 2) comps.resize(2);
        }
        m_wordList.clear();
        if (!m_curWord.empty()) {
            m_wordList.push_back(m_curWord);
            m_wordList.insert(m_wordList.end(), comps.begin(), comps.end());
        }
    }
}

void Keyboard::FinishFlick(int sl) {
    Slot& s = m_slots[sl];
    const int item = s.flick;
    s.flick = -1;
    if (item < 0 || item >= int(m_items.size())) return;
    const auto& chars = m_items[item].k.flick;
    Key k;
    k.ch = s.flickDir < int(chars.size()) ? chars[s.flickDir] : chars[0];
    k.label = *k.ch;
    Press(k, true);
}

}  // namespace rv::kb
