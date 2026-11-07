// The headset's windows: the main panel (computers, games, settings), dock, keyboard,
// statistics, notices, the VR launch panel and the in-VR overlay.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

#include "../gfx/sky.h"
#include "media/music_tracks.h"
#include "../platform/jni_bridge.h"
#include "app.h"
#include "rc/project.h"
#include "strings.h"

namespace rv::screens {

using namespace theme;

namespace {

App* g_app = nullptr;

constexpr const char* kVersion = RINCAST_VERSION_STRING;

// ---- shared drawing helpers -------------------------------------------------

void Background(Ui& ui, const Rect& r) { ui.R().RectBorder(r.Inset(1), kPanel, kPanelRadius, kPanelBorder, 2.f); }

// Brand mark: ring, core and the broadcast arc.
void Logo(Ui& ui, float cx, float cy, float s) {
    UiRenderer& R = ui.R();
    R.Arc(cx, cy, s * 0.30f, s * 0.085f, 0.f, 6.2832f, kAccent);
    R.Circle(cx, cy, s * 0.095f, kAccent);
    R.Arc(cx, cy, s * 0.43f, s * 0.07f, -1.35f, 1.13f, kAccent);
}

std::string FormatMs(double v) {
    char b[32];
    snprintf(b, sizeof(b), v >= 10 ? "%.0f" : "%.1f", v);
    return b;
}

std::string Percent(float v) { return std::to_string(int(std::round(v * 100))) + "%"; }

std::string PixelSize(int w, int h) { return std::to_string(w) + " × " + std::to_string(h); }

// Height of wrapped text, without drawing it.
float WrapHeight(Ui& ui, const std::string& text, float w, float size, float lineHeight) {
    return ui.R().WrapText(text, {0, 0, w, 0}, size, Color(0, 0, 0, 0), Face::Medium, lineHeight);
}

// A button as wide as its label needs.
float ButtonWidth(Ui& ui, const std::string& label) { return std::max(150.f, ui.R().Measure(label, 21, Face::SemiBold) + 56); }

// Lays out buttons from the left, on one line.
struct ButtonRow {
    float x, y;
    bool Add(Ui& ui, const char* id, const std::string& label, ButtonStyle style = ButtonStyle::Secondary, bool enabled = true) {
        const float w = ButtonWidth(ui, label);
        const bool clicked = ui.Button(id, {x, y, w, 54}, label, nullptr, style, enabled);
        x += w + 10;
        return clicked;
    }
};

// A column of settings, laid out from the top down.
struct Column {
    float x, y, w;
    bool started = false;
};

// Starts a group of settings.
void Section(Ui& ui, Column& c, const std::string& text) {
    if (c.started) c.y += 24;
    c.started = true;
    ui.R().Text(text, c.x, c.y, 28, kText, Face::SemiBold);
    c.y += 46;
}

// Names the control under it.
void Label(Ui& ui, Column& c, const std::string& text) {
    ui.R().Text(text, c.x, c.y, 21, kMuted, Face::Medium);
    c.y += 30;
}

// hint only where the name can't say it: a cost, or something it turns off.
bool CheckRow(Ui& ui, Column& c, const char* id, const std::string& label, bool& value, const std::string& hint = {}, bool enabled = true) {
    const float textW = c.w - 44;
    const float hintH = hint.empty() ? 0 : WrapHeight(ui, hint, textW, 20, 26) + 4;
    ui.NextLabel(hint.empty() ? label : label + ", " + hint);
    // The box lines up with the text above it; the hover area reaches a little past it.
    const bool changed = ui.Check(id, {c.x - 8, c.y, c.w + 8, 48 + hintH}, label, value, enabled);
    if (!hint.empty()) ui.R().WrapText(hint, {c.x + 44, c.y + 42, textW, hintH}, 20, kMuted, Face::Medium, 26);
    c.y += 50 + hintH;
    return changed;
}

// Options in a tidy grid, for when they don't fit on one line.
bool RadioGrid(Ui& ui, Column& c, const char* id, const std::string& name, const std::vector<std::string>& options, int& selected, int cols) {
    bool changed = false;
    const float w = c.w / cols;
    for (size_t i = 0; i < options.size(); i++) {
        const std::string cid = std::string(id) + "#" + std::to_string(i);
        ui.NextLabel(name);
        if (ui.Radio(cid.c_str(), {c.x - 8 + (i % cols) * w, c.y + (i / cols) * 46, w - 8, 46}, options[i], selected == int(i))) {
            selected = int(i);
            changed = true;
        }
    }
    c.y += ((options.size() + cols - 1) / cols) * 46 + 6;
    return changed;
}

// Options side by side, wrapping to the next line.
bool RadioLine(Ui& ui, Column& c, const char* id, const std::string& name, const std::vector<std::string>& options, int& selected) {
    bool changed = false;
    float x = c.x;
    for (size_t i = 0; i < options.size(); i++) {
        const float w = 50 + ui.R().Measure(options[i], 21, Face::Medium) + 28;
        if (x + w - 28 > c.x + c.w) {
            x = c.x;
            c.y += 48;
        }
        const std::string cid = std::string(id) + "#" + std::to_string(i);
        ui.NextLabel(name);
        if (ui.Radio(cid.c_str(), {x - 8, c.y, w - 20, 46}, options[i], selected == int(i))) {
            selected = int(i);
            changed = true;
        }
        x += w;
    }
    c.y += 52;
    return changed;
}

bool BarRow(Ui& ui, Column& c, const char* id, const std::string& name, float& value, float min, float max, float step, const std::string& text, bool enabled = true, float mark = -1) {
    ui.R().Text(name, c.x, c.y, 21, kMuted, Face::Medium);
    ui.R().Text(text, c.x + c.w, c.y, 21, enabled ? kText : kMuted, Face::SemiBold, Align::Right);
    ui.NextLabel(name);
    const bool changed = ui.Bar(id, {c.x, c.y + 22, c.w, 50}, value, min, max, step, text, enabled, mark);
    c.y += 84;
    return changed;
}

// A pair of lines: a small label over its value.
void Stat(Ui& ui, float x, float y, const std::string& label, const std::string& value, Align align = Align::Left, bool poor = false) {
    ui.R().Text(label, x, y, 20, kMuted, Face::Medium, align);
    ui.R().Text(value, x, y + 26, 21, poor ? kWarn : kText, Face::SemiBold, align);
}

// The same lines the PC draws.
bool PoorLatency(const App::Stats& s) { return s.latencyMs >= 80; }
bool PoorFrameRate(const App::Stats& s, float wanted) { return s.fps > 0 && wanted > 0 && s.fps < wanted * 0.9f; }

// ---- Pages of the main panel ----------------------------------------------------

enum Page { kComputers, kGames, kVrGames, kDesktop, kEnvironment, kGeneral, kAccount, kHelp, kPageCount };

int g_page = kComputers;

bool Connected(App& app) { return app.CurrentMode() == Mode::Desktop || app.CurrentMode() == Mode::LaunchingVr || app.CurrentMode() == Mode::Vr; }

void ShowPage(App& app, int page) {
    if (page == kGames && Connected(app)) app.RequestGames();
    g_page = page;
}

struct PcEntry {
    std::string id;
    std::string name;
    std::string detail;
    bool lan = false;
    bool online = false;
    bool busy = false;
    DiscoveredPc lanInfo;
    CloudPc cloudInfo;
};

// One line of big type, then what to do about it, at the top left of the page. Returns where
// the page can go on.
float Message(Ui& ui, const Rect& b, const std::string& title, const std::string& text = {}, const Color& titleColor = kText, bool busy = false) {
    UiRenderer& R = ui.R();
    const float top = b.y;
    R.Text(title, b.x, top, 28, titleColor, Face::SemiBold);
    if (busy) ui.Spinner(b.x + R.Measure(title, 28, Face::SemiBold) + 34, top + 16, 16, kText);
    float y = top + 48;
    // A line is kept for the text either way, so a button below doesn't move when it changes.
    y += text.empty() ? 30 : R.WrapText(text, {b.x, y, std::min(b.w, 800.f), 200}, 21, kMuted, Face::Medium, 30);
    return y + 20;
}

// One computer in a list: every state uses the same positions, so nothing jumps between them.
constexpr float kPcRowH = 104;
Rect PcAction(const Rect& row, float w = 170) { return {row.Right() - 8 - w, row.y + 25, w, 54}; }

void PcText(Ui& ui, const Rect& row, const char* icon, const std::string& name, const std::string& detail, const Color& detailColor, bool enabled = true) {
    UiRenderer& R = ui.R();
    R.Icon(icon, row.x + 30, row.y + kPcRowH / 2, 56, enabled ? kText : kFaint);
    R.TextIn(name, {row.x + 76, row.y + 18, row.w - 300, 36}, 26, enabled ? kText : kMuted, Face::SemiBold);
    R.TextIn(detail, {row.x + 76, row.y + 56, row.w - 300, 30}, 21, detailColor);
}

float g_disconnectArmed = -100;

void PageComputers(Ui& ui, const Rect& body) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    const auto state = app.Account().State();
    const auto clientState = app.Client().GetState();

    // ---- connected ----
    if (Connected(app)) {
        const Rect row{body.x - 8, body.y - 8, body.w + 16, kPcRowH};
        const bool slowLink = PoorLatency(app.CurrentStats());
        PcText(ui, row, "desktop_windows", app.Client().ServerName(),
               slowLink ? (app.Client().IsVpn() ? Tr("net.slowVpn") : app.Client().IsRemote() ? Tr("net.slowInternet") : Tr("net.slowLocal"))
                        : app.Client().IsVpn() ? Tr("net.throughVpn") : app.Client().IsRemote() ? Tr("net.overInternet") : Tr("net.connected"),
               slowLink ? kWarn : kGood);
        const bool armed = ui.Time() - g_disconnectArmed < 4.f;
        if (ui.Button("pc-disconnect", PcAction(row, armed ? 320 : 170), armed ? Tr("btn.disconnectAgain") : Tr("btn.disconnect"), nullptr, armed ? ButtonStyle::Danger : ButtonStyle::Secondary)) {
            if (armed) app.Disconnect();
            else g_disconnectArmed = ui.Time();
        }
        ButtonRow b{body.x, row.Bottom() + 16};
        if (b.Add(ui, "pc-games", Tr("btn.chooseGame"), ButtonStyle::Primary)) ShowPage(app, kGames);
        if (b.Add(ui, "pc-steamvr", app.VrRunning() ? Tr("btn.backToVr") : Tr("btn.startSteamvr"))) app.LaunchSteamVr();
        const auto st = app.CurrentStats();
        Column c{body.x, row.Bottom() + 100, body.w};
        struct Line {
            const char* name;
            std::string value;
        };
        const Line lines[] = {{Tr("stat.network"), st.rttMs > 0 ? Trf("unit.roundTrip", {{"ms", FormatMs(st.rttMs)}}) : "–"},
                              {Tr("stat.decoding"), st.decodeMs > 0 ? FormatMs(st.decodeMs) + " ms" : "–"},
                              {Tr("stat.video"), st.codec.empty() ? "–" : st.codec + ", " + PixelSize(st.width, st.height)}};
        for (const Line& l : lines) {
            ui.Divider(c.x, c.y, c.w);
            R.Text(l.name, c.x, c.y + 12, 21, kMuted);
            R.Text(l.value, c.x + 200, c.y + 12, 21, kText, Face::Medium);
            c.y += 52;
        }
        ui.Divider(c.x, c.y, c.w);
        return;
    }

    // ---- connecting / approval ----
    if (app.CurrentMode() == Mode::Connecting || app.Reconnecting()) {
        const bool approval = clientState == StreamClient::State::WaitingApproval;
        const std::string title = approval ? Tr("computers.waitingPc")
                                  : app.Reconnecting() ? Tr("computers.reconnecting")
                                                       : Trf("computers.connectingTo", {{"name", app.Client().ServerName()}});
        const std::string text = approval ? Tr("computers.approvalHint")
                                 : app.Reconnecting() ? Tr("computers.reconnectingText")
                                                      : app.Client().StatusText();
        const float y = Message(ui, body, title, text, kText, true);
        ButtonRow b{body.x, y};
        if (b.Add(ui, "cancel", Tr("btn.cancel"))) {
            app.DismissProblem();
            app.Disconnect();
        }
        return;
    }

    Rect list = body;
    // What went wrong with the last connection stays until it's dealt with.
    if (!app.ConnectionProblem().empty()) {
        R.Text(Tr("computers.couldntConnect"), body.x, body.y, 28, kDanger, Face::SemiBold);
        const float h = R.WrapText(app.ConnectionProblem(), {body.x, body.y + 44, body.w - 330, 60}, 21, kText, Face::Medium, 28);
        const float right = body.Right();
        if (ui.Button("dismiss", {right - 130, body.y + 6, 130, 54}, Tr("btn.dismiss"))) app.DismissProblem();
        if (app.CanRetry() && ui.Button("retry", {right - 130 - 10 - 160, body.y + 6, 160, 54}, Tr("btn.tryAgain"), nullptr, ButtonStyle::Primary)) app.RetryConnection();
        const float used = std::max(70.f, 44 + h) + 28;
        ui.Divider(body.x, body.y + used - 14, body.w);
        list.y += used;
        list.h -= used;
    }

    // ---- PC list ----
    std::vector<PcEntry> entries;
    for (const auto& pc : app.Lan().List()) {
        PcEntry e;
        e.id = pc.serverId;
        e.name = pc.name;
        e.lan = true;
        e.online = true;
        e.busy = pc.busy;
        e.detail = pc.viaUsb ? Tr("pcDetail.usb") : pc.viaVpn ? Tr("pcDetail.vpn") : Tr("pcDetail.lan");
        e.lanInfo = pc;
        entries.push_back(e);
    }
    for (const auto& pc : app.Account().Pcs()) {
        bool dup = false;
        for (auto& e : entries)
            if (e.id == pc.id) dup = true;
        if (dup) continue;
        PcEntry e;
        e.id = pc.id;
        e.name = pc.name;
        e.online = pc.online;
        e.busy = pc.busy;
        e.detail = pc.online ? Tr("pcDetail.remote") : Tr("pcDetail.offlineHint");
        e.cloudInfo = pc;
        entries.push_back(e);
    }

    const float searching = app.TimeOnHomeWithoutPcs(entries.empty());
    static bool enteringAddress = false, addressValid = true;
    static std::string address;
    static std::string lookingAt;
    static float lookingSince = 0;
    static size_t entriesWhenLooking = 0;
    auto openAddress = [&] {
        enteringAddress = true;
        addressValid = true;
        address = app.Settings().manualPc;
    };
    if (enteringAddress) {
        // Number pad for networks that block discovery (guest Wi-Fi, some mesh systems).
        const float y = Message(ui, list, Tr("computers.enterAddressTitle"), Tr("computers.enterAddressText"));
        const float kw = 124, kh = 56, padW = 3 * kw + 16;
        const Rect field{list.x, y, padW, 58};
        R.RectBorder(field, Color(0, 0, 0, 1), kRadius, kEdge, 2.f);
        R.TextIn(address.empty() ? " " : address, field.Inset(16, 0), 28, kText, Face::SemiBold);
        static const char* keys[4][3] = {{"1", "2", "3"}, {"4", "5", "6"}, {"7", "8", "9"}, {".", "0", "<"}};
        for (int row = 0; row < 4; row++) {
            for (int col = 0; col < 3; col++) {
                const std::string k = keys[row][col];
                const Rect kr{list.x + col * (kw + 8), y + 68 + row * (kh + 6), kw, kh};
                const std::string id = "pad-" + k;
                const bool back = k == "<";
                if (back) ui.NextLabel(Tr("a11y.backspace"));
                if (ui.Button(id.c_str(), kr, back ? "" : k, back ? "backspace" : nullptr, ButtonStyle::Secondary, true, 30)) {
                    if (back) { if (!address.empty()) address.pop_back(); }
                    else if (address.size() < 15) address += k;
                }
            }
        }
        // The actions stay beside the pad, where they fit whatever came above it.
        const float ax = list.x + padW + 24;
        if (ui.Button("pad-ok", {ax, y, 240, 58}, Tr("btn.lookForPc"), nullptr, ButtonStyle::Primary, !address.empty())) {
            // Four numbers from 0 to 255; anything else can't be a PC on this network.
            int a = -1, b = -1, c = -1, d = -1;
            char rest = 0;
            const int parts = std::sscanf(address.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &rest);
            addressValid = parts == 4 && a >= 0 && a <= 255 && b >= 0 && b <= 255 && c >= 0 && c <= 255 && d >= 0 && d <= 255;
            if (addressValid) {
                app.SetManualPc(address);
                enteringAddress = false;
                lookingAt = address;
                lookingSince = app.Time();
                entriesWhenLooking = entries.size();
            }
        }
        if (ui.Button("pad-cancel", {ax, y + 68, 240, 56}, Tr("btn.cancel"))) {
            enteringAddress = false;
            addressValid = true;
        }
        if (!addressValid) R.WrapText(Tr("computers.badAddress"), {ax, y + 140, list.Right() - ax, 90}, 20, kWarn, Face::Medium, 27);
        return;
    }
    if (entries.empty() && !lookingAt.empty()) {
        if (app.Time() - lookingSince < 8.f) {
            Message(ui, list, Trf("computers.lookingAt", {{"address", lookingAt}}), {}, kText, true);
        } else {
            const float y = Message(ui, list, Trf("computers.noAnswer", {{"address", lookingAt}}), Tr("computers.noAnswerHint"), kWarn);
            ButtonRow b{list.x, y};
            if (b.Add(ui, "manual-again", Tr("btn.changeAddress"))) {
                openAddress();
                lookingAt.clear();
            }
            if (b.Add(ui, "manual-done", Tr("btn.back"))) lookingAt.clear();
        }
        return;
    }
    if (entries.size() > entriesWhenLooking) lookingAt.clear();  // it turned up
    if (entries.empty()) {
        float y;
        if (searching > 15.f) {
            y = Message(ui, list, Tr("computers.stillLooking"), {}, kText, true) - 12;
            const std::string getIt = Trf("computers.getIt", {{"site", rc::kDownloadSite}});
            const std::string checks[] = {getIt, Tr("computers.check1"), Tr("computers.check2")};
            for (int i = 0; i < 3; i++) {
                R.Text(std::to_string(i + 1), list.x, y, 21, kText, Face::SemiBold);
                y += R.WrapText(checks[i], {list.x + 34, y, std::min(list.w - 34, 660.f), 90}, 21, kMuted, Face::Medium, 30) + 12;
            }
            y += 10;
        } else {
            y = Message(ui, list, Tr("computers.looking"), Tr("computers.lookingHint"), kText, true);
        }
        ButtonRow b{list.x, y};
        if (b.Add(ui, "manual", Tr("btn.enterAddress"))) openAddress();
        if (state != Cloud::SignInState::SignedIn && b.Add(ui, "signin-empty", Tr("btn.signinOtherNetworks"))) ShowPage(app, kAccount);
        return;
    }

    // The address someone typed gets its answer here, above what's already listed.
    if (!lookingAt.empty()) {
        const bool still = app.Time() - lookingSince < 8.f;
        const std::string lookTitle = still ? Trf("computers.lookingAt", {{"address", lookingAt}})
                                            : Trf("computers.noAnswer", {{"address", lookingAt}});
        R.TextIn(lookTitle, {list.x, list.y, list.w - 330, 54}, 22, still ? kText : kWarn, Face::SemiBold);
        if (still) {
            ui.Spinner(list.x + R.Measure(lookTitle, 22, Face::SemiBold) + 30, list.y + 27, 13, kText);
        } else {
            if (ui.Button("look-again", {list.Right() - 240, list.y, 180 + 60, 54}, Tr("btn.changeAddress"))) {
                openAddress();
                lookingAt.clear();
            }
        }
        ui.Divider(list.x, list.y + 66, list.w);
        list.y += 80;
        list.h -= 80;
    }
    // The one you'd most likely want stands out: the last one used, or else the first free one.
    std::string likeliest;
    for (const auto& e : entries) {
        if (!e.online || e.busy) continue;
        if (likeliest.empty() || e.id == app.Settings().lastPc) likeliest = e.id;
    }
    auto connect = [&](const PcEntry& e) {
        if (e.lan) app.ConnectLan(e.lanInfo);
        else app.ConnectCloud(e.cloudInfo);
    };
    // Auto connect happens once, when RinCast opens: never after you've disconnected yourself.
    static bool triedAutoConnect = false;
    if (!triedAutoConnect && app.Settings().autoConnect && app.ConnectionProblem().empty()) {
        for (const auto& e : entries) {
            if (e.id != app.Settings().lastPc || !e.online || e.busy) continue;
            triedAutoConnect = true;
            connect(e);
            return;
        }
    }
    // Someone's using a PC: connecting ends their session, so it takes a second press.
    static std::string takeOverId;
    static float takeOverAt = -100;
    const float scroll = ui.BeginScroll("pcs", {list.x - 8, list.y - 8, list.w + 16, list.h + 8 - 70}, entries.size() * (kPcRowH + 8));
    float y = list.y - 8 - scroll;
    for (const auto& e : entries) {
        const Rect row{list.x - 8, y, list.w + 16, kPcRowH};
        const std::string id = "pc-" + e.id;
        const bool armed = e.busy && takeOverId == e.id && ui.Time() - takeOverAt < 4.f;
        const std::string detail = e.busy ? Tr("pcDetail.busy") : e.detail;
        ui.NextLabel(e.name + ", " + detail);
        if (ui.Hit(id.c_str(), row, e.online)) {
            if (e.busy && !armed) {
                takeOverId = e.id;
                takeOverAt = ui.Time();
            } else {
                connect(e);
            }
        }
        ui.HoverFill(row, ui.IsHot(id.c_str()) ? 1.f : 0.f);
        PcText(ui, row, "desktop_windows", e.name, detail, e.busy ? kWarn : kMuted, e.online);
        // Looks like a button; the whole row takes the click, which is easier to hit.
        const Rect action = PcAction(row, armed ? 220 : 170);
        if (e.online) {
            // While Tr("btn.tryAgain") is on show, that's the main action.
            const bool primary = !armed && e.id == likeliest && app.ConnectionProblem().empty();
            const Color fill = armed ? kDangerFill : primary ? kAccent : kControl;
            R.RectFill(action, fill, kRadius);
            R.TextIn(armed ? Tr("btn.pressTakeOver") : e.busy ? Tr("btn.takeOver") : Tr("btn.connect"), action, 21, primary ? kOnAccent : kText, Face::SemiBold, Align::Center);
        } else {
            R.TextIn(Tr("pcDetail.offline"), action, 21, kMuted, Face::SemiBold, Align::Right);
        }
        y += kPcRowH + 8;
    }
    ui.EndScroll();
    // A network that blocks discovery can hide the one you're after.
    ButtonRow more{list.x, list.Bottom() - 54};
    if (more.Add(ui, "manual", Tr("btn.enterAddress"))) openAddress();
}

// Up to two lines, then an ellipsis.
void TwoLines(Ui& ui, const std::string& text, const Rect& r, float size, float lineHeight) {
    UiRenderer& R = ui.R();
    if (R.Measure(text, size, Face::SemiBold) <= r.w) {
        R.Text(text, r.x, r.y, size, kText, Face::SemiBold);
        return;
    }
    size_t cut = 0;
    for (size_t i = text.find(' '); i != std::string::npos; i = text.find(' ', i + 1)) {
        if (R.Measure(text.substr(0, i), size, Face::SemiBold) > r.w) break;
        cut = i;
    }
    if (cut == 0) {
        R.Text(R.Ellipsize(text, r.w, size, Face::SemiBold), r.x, r.y, size, kText, Face::SemiBold);
        return;
    }
    R.Text(text.substr(0, cut), r.x, r.y, size, kText, Face::SemiBold);
    R.Text(R.Ellipsize(text.substr(cut + 1), r.w, size, Face::SemiBold), r.x, r.y + lineHeight, size, kText, Face::SemiBold);
}

void PageGames(Ui& ui, const Rect& body) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    if (!Connected(app)) {
        const float y = Message(ui, body, Tr("games.connectFirst"), Tr("games.connectFirstHint"));
        ButtonRow b{body.x, y};
        if (b.Add(ui, "to-computers", Tr("btn.choosePc"), ButtonStyle::Primary)) ShowPage(app, kComputers);
        return;
    }
    auto& games = app.Games();
    if (games.empty() && !app.GamesAnswered() && app.GamesRequestedAgo() >= 5.f) {
        const float y = Message(ui, body, Tr("games.noAnswer"), Tr("games.noAnswerHint"), kWarn);
        ButtonRow b{body.x, y};
        if (b.Add(ui, "refresh", Tr("btn.tryAgain"), ButtonStyle::Primary)) app.RequestGames();
        return;
    }
    if (games.empty() && !app.GamesAnswered()) {
        Message(ui, body, Tr("games.looking"), {}, kText, true);
        return;
    }
    if (games.empty()) {
        const float y = Message(ui, body, Tr("games.none"), Tr("games.noneHint"));
        ButtonRow b{body.x, y};
        if (b.Add(ui, "refresh", Tr("btn.lookAgain"))) app.RequestGames();
        return;
    }
    const int cols = 5;
    const float gap = 16;
    const float cardW = (body.w - gap * (cols - 1)) / cols;
    const float cardH = cardW * 1.5f + 66;
    const int rows = int((games.size() + cols - 1) / cols);
    const float scroll = ui.BeginScroll("games", {body.x - 6, body.y, body.w + 16, body.h}, rows * (cardH + gap));
    for (size_t i = 0; i < games.size(); i++) {
        GameInfo& g = games[i];
        const Rect card{body.x + (i % cols) * (cardW + gap), body.y + (i / cols) * (cardH + gap) - scroll, cardW, cardH};
        if (card.Bottom() < body.y || card.y > body.Bottom()) continue;
        app.RequestGameImage(g);
        const std::string id = "game-" + g.id;
        const Rect art{card.x, card.y, cardW, cardW * 1.5f};
        ui.NextLabel(g.name);
        if (ui.Hit(id.c_str(), card)) app.LaunchGame(g.id);
        if (g.image.id) {
            R.Image(g.image.id, art, kRadius);
        } else {
            R.RectFill(art, kControl, kRadius);
            R.Icon("sports_esports", art.x + art.w / 2, art.y + art.h / 2, 56, kFaint);
        }
        if (ui.IsHeld(id.c_str())) R.RectFill(art, Color(0, 0, 0, 0.4f), kRadius);
        ui.HoverLighten(art, ui.IsHot(id.c_str()) ? 1.f : 0.f);
        if (app.VrRunning() && app.VrStateText() == g.name) {
            const Rect tag{art.x + 8, art.y + 8, 104, 34};
            R.RectFill(tag, kAccent, kRadius);
            R.TextIn(Tr("games.running"), tag, 20, kOnAccent, Face::SemiBold, Align::Center);
        }
        TwoLines(ui, g.name, {card.x, art.Bottom() + 8, cardW, 54}, 20, 25);
    }
    ui.EndScroll();
}

bool PageVrQuality(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    bool changed = false;
    const float gap = 44, w = (body.w - gap) / 2;
    Column l{body.x, body.y, w};
    Section(ui, l, Tr("vr.imageSection"));
    Label(ui, l, Tr("vr.qualityLabel"));
    static const char* qn[] = {"quality.low", "quality.medium", "quality.high", "quality.ultra", "quality.max"};
    static const char* qk[] = {"low", "medium", "high", "ultra", "godlike"};
    static const float qs[] = {0.6f, 0.75f, 0.9f, 1.f, 1.2f};
    for (int i = 0; i < 5; i++) {
        const std::string id = std::string("quality#") + qk[i];
        const std::string res = PixelSize(int(app.Xr().RecommendedWidth() * qs[i]), int(app.Xr().RecommendedHeight() * qs[i]));
        ui.NextLabel(Tr("vr.qualityA11y"));
        if (ui.Radio(id.c_str(), {l.x - 8, l.y, l.w + 8, 44}, Tr(qn[i]), s.vrQuality == qk[i], res)) {
            s.vrQuality = qk[i];
            changed = true;
        }
        l.y += 45;
    }
    l.y += 14;
    Label(ui, l, Tr("vr.fpsLabel"));
    std::vector<std::string> rates;
    int ri = 0;
    const auto available = app.Xr().RefreshRates();
    for (size_t i = 0; i < available.size(); i++) {
        rates.push_back(std::to_string(int(std::round(available[i]))));
        if (std::fabs(available[i] - s.refreshRate) < 1) ri = int(i);
    }
    if (rates.size() > 1) {
        if (RadioLine(ui, l, "rate", Tr("vr.fpsA11y"), rates, ri)) {
            s.refreshRate = available[size_t(ri)];
            changed = true;
        }
    } else if (!rates.empty()) {
        ui.R().Text(rates[0] + " fps", l.x, l.y + 6, 21, kText);
        l.y += 46;
    }
    l.y += 14;
    Label(ui, l, Tr("vr.codecLabel"));
    static const std::vector<std::string> codecs = {Tr("codec.auto"), "HEVC", "AV1", "H.264"};
    static const char* ck[] = {"auto", "hevc", "av1", "h264"};
    int ci = 0;
    for (int i = 0; i < 4; i++)
        if (s.codec == ck[i]) ci = i;
    if (RadioGrid(ui, l, "codec", Tr("vr.codecLabel"), codecs, ci, 2)) {
        s.codec = ck[ci];
        changed = true;
    }
    changed |= CheckRow(ui, l, "10bit", Tr("vr.tenBit"), s.tenBit, ci == 3 ? Tr("vr.needsHevcAv1") : "", ci != 3);

    Column c{body.x + w + gap, body.y, w};
    Section(ui, c, Tr("vr.bitrateSection"));
    const bool usePc = s.vrBitrateMbps == 0;
    // The PC tells what its own limit is once connected.
    const int pcLimit = app.ServerInfo().value("vrBitrateMbps", 0);
    bool followPc = usePc;
    if (CheckRow(ui, c, "bitrate-pc", Tr("vr.usePcLimit"), followPc)) {
        s.vrBitrateMbps = followPc ? 0 : pcLimit > 0 ? pcLimit : 150;
        changed = true;
    }
    float bitrate = usePc ? float(std::max(20, pcLimit)) : float(s.vrBitrateMbps);
    const std::string bitrateText = !usePc ? std::to_string(int(bitrate)) + " Mbps" : pcLimit > 0 ? std::to_string(pcLimit) + " Mbps" : Tr("vr.setOnPc");
    if (BarRow(ui, c, "bitrate", Tr("vr.bitrateLabel"), bitrate, 20, 500, 20, bitrateText, !usePc)) {
        s.vrBitrateMbps = int(bitrate);
        changed = true;
    }
    changed |= CheckRow(ui, c, "adaptive", Tr("vr.adaptive"), s.adaptiveBitrate);
    Section(ui, c, Tr("vr.enhancements"));
    changed |= CheckRow(ui, c, "upscaling", Tr("vr.upscaling"), s.upscaling, Tr("vr.upscalingHint"));
    changed |= CheckRow(ui, c, "framegen", Tr("vr.frameGen"), s.frameGeneration, Tr("vr.frameGenHint"));
    Section(ui, c, Tr("vr.tracking"));
    // The Quest can't run body and finger tracking together, so there each turns the other off.
    Headset& device = app.Device();
    const bool exclusive = device.BodyExcludesFingers();
    if (device.BodySupported(app.Xr())) {
        const std::string status = device.BodyStatus();
        const char* hint = device.BodyHint();
        if (CheckRow(ui, c, "body", Tr("vr.bodyTracking"), s.bodyTracking, !status.empty() ? status : hint ? hint : "")) {
            changed = true;
            if (s.bodyTracking && exclusive) s.fingersWithControllers = false;
        }
        if (const char* action = device.BodyAction()) {
            ButtonRow b{c.x + 44, c.y};
            if (b.Add(ui, "body-action", action)) device.RunBodyAction(app.Xr());
            c.y += 62;
        }
    }
    if (app.Input().SimultaneousSupported() &&
        CheckRow(ui, c, "fingers", Tr("vr.fingerTracking"), s.fingersWithControllers, exclusive ? Tr("vr.fingerExclusive") : "")) {
        changed = true;
        if (s.fingersWithControllers && exclusive) s.bodyTracking = false;
    }
    changed |= CheckRow(ui, c, "handsgames", Tr("vr.handsInGames"), s.handsInGames, Tr("vr.handsHint"));
    if (device.FaceSupported(app.Xr()))
        changed |= CheckRow(ui, c, "face", Tr("vr.faceTracking"), s.faceTracking, device.FaceHint());
    return changed;
}

bool PageDesktop(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    bool changed = false;
    const float gap = 44, w = (body.w - gap) / 2;
    const float distanceBefore = s.screenDistance;
    Column l{body.x, body.y, w};
    Section(ui, l, Tr("desktop.screensSection"));
    float curve = s.curvature;
    if (BarRow(ui, l, "curve", Tr("desktop.curve"), curve, 0, 1, 0.05f, s.curvature < 0.03f ? Tr("desktop.flat") : Percent(s.curvature))) {
        s.curvature = curve;
        changed = true;
    }
    l.y += 8;
    float dist = s.screenDistance;
    char hint[32];
    snprintf(hint, sizeof(hint), "%.2f m", s.screenDistance);
    if (BarRow(ui, l, "distance", Tr("desktop.distance"), dist, 0.8f, 3.f, 0.05f, hint)) {
        s.screenDistance = dist;
        changed = true;
    }
    l.y += 8;
    ButtonRow b{l.x, l.y};
    if (b.Add(ui, "arrange", Tr("btn.recenter"))) app.Recenter();

    Column c{body.x + w + gap, body.y, w};
    Section(ui, c, Tr("desktop.qualitySection"));
    Label(ui, c, Tr("desktop.fpsLabel"));
    int fpsI = s.desktopFps == 0 ? 0 : s.desktopFps <= 30 ? 1 : s.desktopFps <= 60 ? 2 : 3;
    static const std::vector<std::string> fps = {Tr("fps.auto"), "30", "60", "90"};
    if (RadioGrid(ui, c, "dfps", Tr("vr.fpsA11y"), fps, fpsI, 2)) {
        static const int v[] = {0, 30, 60, 90};
        s.desktopFps = v[fpsI];
        changed = true;
    }
    c.y += 8;
    changed |= CheckRow(ui, c, "sharp", Tr("desktop.sharpText"), s.sharpLayers, Tr("desktop.sharpHint"));
    if (changed) app.UpdateScreenShape(distanceBefore);
    return changed;
}

bool PageEnvironment(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    UiRenderer& R = ui.R();
    bool changed = false;
    const auto& envs = Environments();
    const float gap = 16, cardW = (body.w - 2 * gap) / 3, cardH = 186;
    int shown = 0;
    float y = body.y;
    for (const EnvironmentInfo& e : envs) {
        if (e.id == Environment::Passthrough && !app.Xr().PassthroughSupported()) continue;
        const Rect card{body.x + (shown % 3) * (cardW + gap), body.y + (shown / 3) * (cardH + gap), cardW, cardH};
        y = card.Bottom();
        shown++;
        const std::string id = std::string("env-") + e.key;
        const bool sel = s.environment == e.key;
        ui.NextLabel(Tr((std::string("env.") + e.key).c_str()));
        if (ui.Hit(id.c_str(), card)) {
            s.environment = e.key;
            changed = true;
        }
        const Rect pic{card.x, card.y, card.w, 146};
        const Texture& thumb = app.EnvironmentThumbnail(e.key);
        if (thumb.id) {
            R.Image(thumb.id, pic, kRadius);
            R.RectBorder(pic, Color(0, 0, 0, 0), kRadius, kLine, 2.f);
        } else {
            // Your own room: no picture to show. The tile stays plain.
            R.RectFill(pic, kControl, kRadius);
        }
        if (sel) {
            R.RectBorder(pic, Color(0, 0, 0, 0), kRadius, kAccent, 4.f);
            R.RectFill({pic.Right() - 44, pic.y + 12, 32, 32}, kAccent, 3);
            R.Icon("check", pic.Right() - 28, pic.y + 28, 28, kOnAccent);
        }
        ui.HoverLighten(pic, ui.IsHot(id.c_str()) ? 1.f : 0.f);
        R.TextIn(e.name, {card.x, pic.Bottom() + 2, card.w, 38}, 20, kText, sel ? Face::SemiBold : Face::Medium);
    }
    Column c{body.x, y + 14, cardW * 2 + gap};
    float b = s.skyBrightness;
    if (BarRow(ui, c, "brightness", Tr("env.brightness"), b, 0.3f, 1.6f, 0.05f, Percent(s.skyBrightness), true, 1.f)) {
        s.skyBrightness = b;
        changed = true;
    }
    return changed;
}

bool PageGeneral(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    bool changed = false;
    const float gap = 44, w = (body.w - gap) / 2;
    Column l{body.x, body.y, w};
    Section(ui, l, Tr("general.audioSection"));
    changed |= CheckRow(ui, l, "audio", Tr("general.playSound"), s.streamAudio);
    l.y += 8;
    float v = s.volume;
    if (BarRow(ui, l, "volume", Tr("general.volume"), v, 0, 1, 0.05f, Percent(s.volume), s.streamAudio)) {
        s.volume = v;
        changed = true;
    }
    const bool micAllowed = Platform::HasMicPermission();
    // Without the permission the box shows off and can't be changed.
    bool mic = s.micPassthrough && micAllowed;
    // The connected PC reports which virtual microphone it routes to.
    std::string micName = "RinCast Microphone";
    const json& info = app.ServerInfo();
    if (info.contains("audioDriver") && info["audioDriver"].value("devices", "") == "steam")
        micName = "Steam Streaming Microphone";
    if (CheckRow(ui, l, "mic", Tr("general.useMic"), mic,
                 micAllowed ? Trf("general.micOnPc", {{"mic", micName}})
                            : Trf("general.micPermission", {{"where", std::string(app.Device().SettingsName())}}),
                 micAllowed)) {
        s.micPassthrough = mic;
        changed = true;
    }
    Section(ui, l, Tr("general.controllersSection"));
    changed |= CheckRow(ui, l, "haptics", Tr("general.haptics"), s.haptics);
    l.y += 8;
    float sp = s.scrollSpeed;
    char spHint[16];
    snprintf(spHint, sizeof(spHint), "%.1f×", s.scrollSpeed);
    if (BarRow(ui, l, "scroll", Tr("general.scrollSpeed"), sp, 0.3f, 3, 0.1f, spHint, true, 1.f)) {
        s.scrollSpeed = sp;
        changed = true;
    }

    Column c{body.x + w + gap, body.y, w};
    Section(ui, c, Tr("general.connectingSection"));
    changed |= CheckRow(ui, c, "autoconnect", Tr("general.autoConnect"), s.autoConnect, Tr("general.autoConnectHint"));
    {
        // The headset asks once whether RinCast may use the cable.
        using U = Platform::UsbNetwork;
        const U& u = app.UsbNetwork();
        const std::string hint = !s.usbCable ? Tr("general.usbHint.wifi")
                                 : u.state == U::Connected ? (u.kbps >= 1000000 ? Tr("general.usbHint.usb3") : Tr("general.usbHint.usb2"))
                                 : u.state == U::NotAllowed ? Tr("general.usbHint.notAllowed")
                                                            : Tr("general.usbHint.waiting");
        bool usb = s.usbCable;
        if (CheckRow(ui, c, "usbcable", Tr("general.usbCable"), usb, hint)) app.SetUsbCable(usb);
    }
    Section(ui, c, Tr("general.interfaceSection"));
    Label(ui, c, Tr("general.sizeLabel"));
    // Larger would no longer fit in view.
    static const std::vector<std::string> sizes = {"100%", "115%", "130%"};
    int sizeI = s.uiScale > 1.2f ? 2 : s.uiScale > 1.05f ? 1 : 0;
    if (RadioLine(ui, c, "uisize", Tr("general.sizeLabel"), sizes, sizeI)) {
        static const float sv[] = {1.f, 1.15f, 1.3f};
        s.uiScale = sv[sizeI];
        app.ApplyAccessibility();
        changed = true;
    }
    c.y += 4;
    if (CheckRow(ui, c, "contrast", Tr("general.highContrast"), s.highContrast)) {
        app.ApplyAccessibility();
        changed = true;
    }
    changed |= CheckRow(ui, c, "sounds", Tr("general.uiSounds"), s.uiSounds);
    changed |= CheckRow(ui, c, "music", Tr("general.music"), s.music, Tr("general.musicHint"));
    changed |= CheckRow(ui, c, "readaloud", Tr("general.readAloud"), s.readAloud, Tr("general.readAloudHint"));
    if (CheckRow(ui, c, "stats", Tr("general.perfOverlay"), s.showStats)) {
        app.ToggleWindow("stats", s.showStats && app.CurrentMode() != Mode::Home);
        changed = true;
    }
    c.y += 4;
    // The language, named in its own words so it's findable in any UI.
    Label(ui, c, Tr("general.language"));
    size_t langCount = 0;
    const auto* langs = LanguageChoices(langCount);
    std::vector<std::string> langNames;
    int langI = 0;
    for (size_t i = 0; i < langCount; i++) {
        langNames.push_back(langs[i].name);
        if (Language() == langs[i].code) langI = int(i) + 1;
    }
    // Index 0 is "auto", which follows the headset's system language.
    langNames.insert(langNames.begin(), Tr("language.auto"));
    if (RadioLine(ui, c, "language", Tr("general.language"), langNames, langI)) {
        if (langI == 0) {
            s.language = "auto";
        } else {
            s.language = langs[size_t(langI) - 1].code;
        }
        SetLanguage(s.language, Platform::SystemLocale());
        app.SaveSettings();
        changed = true;
    }
    return changed;
}

void PageAccount(Ui& ui, const Rect& body) {
    App& app = *g_app;
    HeadsetSettings& s = app.Settings();
    UiRenderer& R = ui.R();
    const auto st = app.Account().State();
    switch (st) {
        case Cloud::SignInState::SignedOut:
        case Cloud::SignInState::Error: {
            float y = Message(ui, body, Tr("account.reachTitle"), Tr("account.reachHint"));
            if (st == Cloud::SignInState::Error) y += R.WrapText(app.Account().Error(), {body.x, y, std::min(body.w, 680.f), 90}, 21, kDanger, Face::Medium, 30) + 14;
            ButtonRow b{body.x, y};
            if (b.Add(ui, "getcode", Tr("btn.signIn"), ButtonStyle::Primary)) app.Account().BeginSignIn();
            break;
        }
        case Cloud::SignInState::RequestingCode:
            Message(ui, body, Tr("account.gettingCode"), {}, kText, true);
            break;
        case Cloud::SignInState::WaitingForApproval: {
            const std::string code = app.Account().UserCode();
            static std::string announced;
            if (code != announced) {
                announced = code;
                std::string spaced;
                for (char c : code) if (c != '-') { spaced += c; spaced += ' '; }
                ui.Announce(Trf("account.codeAnnounce", {{"code", spaced}}));
            }
            R.Text(Tr("account.enterCode"), body.x, body.y, 28, kText, Face::SemiBold);
            R.Text(code, body.x, body.y + 54, 70, kText, Face::SemiBold);
            std::string url = app.Account().VerificationUrl();
            const size_t scheme = url.find("://");
            if (scheme != std::string::npos) url = url.substr(scheme + 3);
            const float top = body.y + 158;
            const float h = R.WrapText(Trf("account.goTo", {{"url", url}}),
                                       {body.x, top, std::min(body.w, 680.f), 90}, 21, kMuted, Face::Medium, 30);
            const float y = top + h + 20;
            R.Text(Tr("account.waitingConfirm"), body.x, y, 21, kText);
            ui.Spinner(body.x + R.Measure(Tr("account.waitingConfirm"), 21, Face::Medium) + 24, y + 13, 12, kText);
            ButtonRow b{body.x, y + 46};
            if (b.Add(ui, "cancel-signin", Tr("btn.cancel"))) app.Account().CancelSignIn();
            break;
        }
        case Cloud::SignInState::SignedIn: {
            const float y = Message(ui, body, Trf("account.signedInAs", {{"name", app.Account().UserName()}}));
            static float signOutArmed = -100;
            const bool armed = ui.Time() - signOutArmed < 4.f;
            ButtonRow b{body.x, y};
            if (b.Add(ui, "signout", armed ? Tr("btn.signOutAgain") : Tr("btn.signOut"), armed ? ButtonStyle::Danger : ButtonStyle::Secondary)) {
                if (armed) app.Account().SignOut();
                else signOutArmed = ui.Time();
            }
            break;
        }
    }
    if (s.cloudServer != HeadsetSettings{}.cloudServer) R.TextIn(Trf("account.serverLine", {{"server", s.cloudServer}}), {body.x, body.Bottom() - 30, body.w, 30}, 18, kMuted);
}

void PageHelp(Ui& ui, const Rect& body) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Column l{body.x, body.y, body.w};
    Section(ui, l, Tr("help.section"));
    struct Line {
        const char* input;
        const char* action;
    };
    static const Line lines[] = {{"help.input1", "help.action1"},
                                 {"help.input2", "help.action2"},
                                 {"help.input3", "help.action3"},
                                 {"help.input4", "help.action4"},
                                 {"help.input5", "help.action5"},
                                 {"help.input6", "help.action6"},
                                 {"help.input7", "help.action7"},
                                 {"help.input8", "help.action8"}};
    for (const Line& line : lines) {
        ui.Divider(l.x, l.y, l.w);
        R.Text(Tr(line.action), l.x, l.y + 10, 21, kText);
        const float h = R.WrapText(Tr(line.input), {l.x + 270, l.y + 10, l.w - 270, 60}, 21, kMuted, Face::Medium, 28);
        l.y += h + 19;
    }
    ui.Divider(l.x, l.y, l.w);
    l.y += 6;
    Section(ui, l, std::string("RinCast ") + kVersion);
    const json& info = app.ServerInfo();
    if (!info.empty()) {
        R.Text(info.value("pcName", "") + ": Streamer " + info.value("version", "") + ", " + info.value("os", "") + ", " + info.value("gpu", ""), l.x, l.y, 20, kMuted);
        l.y += 34;
    }
    l.y += R.WrapText(Tr("help.license"),
                      {l.x, l.y, l.w, 90}, 20, kMuted, Face::Medium, 28) + 12;
    // CC-BY: title, author, license and where to find them, for each track.
    for (const MusicTrack& t : kMusicTracks) {
        R.Text(std::string("'") + t.title + "' by Scott Buckley - released under CC-BY 4.0. www.scottbuckley.com.au", l.x, l.y, 18, kMuted);
        l.y += 26;
    }
}

void BuildMain(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    const bool connected = Connected(app);
    const float navW = 248, statusH = connected ? 82 : 2;
    const float statusY = r.Bottom() - statusH;
    R.RectFill({r.x + navW, r.y + 2, 2, statusY - r.y - 2}, kLine);

    Logo(ui, r.x + 44, r.y + 44, 44);
    R.Text("RinCast", r.x + 76, r.y + 28, 27, kText, Face::SemiBold);

    static const char* names[kPageCount] = {"nav.pcs", "nav.games", "nav.vrQuality", "nav.desktop", "nav.environment", "nav.general", "nav.account", "nav.help"};
    float ny = r.y + 84;
    for (int i = 0; i < kPageCount; i++) {
        const Rect row{r.x + 12, ny, navW - 24, 52};
        const std::string id = std::string("nav-") + names[i];
        ui.NextLabel(Tr(names[i]));
        if (ui.Row(id.c_str(), row, g_page == i)) ShowPage(app, i);
        const Color fg = g_page == i || ui.IsHot(id.c_str()) ? kText : kMuted;
        R.TextIn(Tr(names[i]), {row.x + 16, row.y, row.w - 24, row.h}, 22, fg, g_page == i ? Face::SemiBold : Face::Medium);
        ny += 54;
    }
    if (connected) {
        R.RectFill({r.x + 12, statusY - 14 - 54 - 12, navW - 24, 2}, kLine);
        if (ui.Button("nav-close", {r.x + 12, statusY - 14 - 54, navW - 24, 54}, Tr("btn.hideMenu"), nullptr, ButtonStyle::Quiet)) app.ToggleWindow("home", false);
        // Live numbers along the bottom, on every page.
        R.RectFill({r.x + 2, statusY, r.w - 4, 2}, kLine);
        const auto st = app.CurrentStats();
        const float sx = r.Right() - 36;
        if (g_page != kComputers) Stat(ui, r.x + 28, statusY + 14, Tr("stat.connectedTo"), app.Client().ServerName());
        Stat(ui, sx, statusY + 14, Tr("stat.bitrate"), st.mbps > 0 ? FormatMs(st.mbps) + " Mbps" : "–", Align::Right);
        Stat(ui, sx - 180, statusY + 14, Tr("stat.fps"), st.fps > 0 ? FormatMs(st.fps) + " fps" : "–", Align::Right, app.CurrentMode() == Mode::Vr && PoorFrameRate(st, app.Settings().refreshRate));
        Stat(ui, sx - 360, statusY + 14, Tr("stat.latency"), st.latencyMs > 0 ? FormatMs(st.latencyMs) + " ms" : "–", Align::Right, PoorLatency(st));
    }

    // A lost connection, a retry or a failure is news on any page.
    static bool wasTrouble = false;
    const bool trouble = app.Reconnecting() || !app.ConnectionProblem().empty();
    if (trouble && !wasTrouble) g_page = kComputers;
    wasTrouble = trouble;

    const Rect body{r.x + navW + 36, r.y + 30, r.w - navW - 72, statusY - r.y - 54};
    bool changed = false;
    switch (g_page) {
        case kComputers: PageComputers(ui, body); break;
        case kGames: PageGames(ui, body); break;
        case kVrGames: changed = PageVrQuality(ui, body); break;
        case kDesktop: changed = PageDesktop(ui, body); break;
        case kEnvironment: changed = PageEnvironment(ui, body); break;
        case kGeneral: changed = PageGeneral(ui, body); break;
        case kAccount: PageAccount(ui, body); break;
        case kHelp: PageHelp(ui, body); break;
    }
    if (changed) app.ApplyStreamSettings();
}

// ---- Dock ---------------------------------------------------------------------

void BuildDock(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    // Connection summary.
    const auto st = app.CurrentStats();
    const bool slow = st.latencyMs >= 80;  // the same line the PC draws
    R.TextIn(app.Client().ServerName(), {r.x + 24, r.y + 18, 176, 36}, 21, kText, Face::SemiBold);
    const std::string sub = st.latencyMs > 0 ? Trf("dock.latency", {{"ms", FormatMs(st.latencyMs)}}) + (slow ? Tr("net.slowSuffix") : "") : app.Client().IsVpn() ? Tr("pcDetail.vpn") : app.Client().IsRemote() ? Tr("net.overInternet") : Tr("net.connected");
    R.TextIn(sub, {r.x + 24, r.y + 54, 176, 30}, 19, slow ? kWarn : kMuted);
    R.RectFill({r.x + 212, r.y + 14, 2, r.h - 28}, kLine);

    struct Btn {
        const char* id;
        const char* icon;
        const char* label;
    };
    // The labels are i18n keys; "SteamVR" isn't one and Tr passes it through.
    const Btn btns[] = {{"monitors", "desktop_windows", "nav.desktop"}, {"games", "sports_esports", "nav.games"},
                        {"keyboard", "keyboard", "dock.keyboard"}, {"menu", "apps", "dock.menu"},
                        {"steamvr", "rocket_launch", app.VrRunning() ? "btn.backToVr" : "dock.steamvr"}, {"recenter", "center_focus_strong", "btn.recenter"},
                        {"disconnect", "logout", "btn.disconnect"}};
    const float bw = 100, gap = 4;
    float x = r.x + 224;
    int index = 0;
    for (const auto& b : btns) {
        // Toggles, then one-shot actions, then the way out.
        if (index == 4 || index == 6) {
            R.RectFill({x + 4, r.y + 14, 2, r.h - 28}, kLine);
            x += 14;
        }
        index++;
        const Rect br{x, r.y + 8, bw, r.h - 16};
        const std::string id = b.id;
        const bool panel = app.IsVisible("home");
        bool active = false;
        if (id == "monitors") active = app.MonitorsVisible();
        if (id == "games") active = panel && g_page == kGames;
        if (id == "keyboard") active = app.IsVisible("keyboard");
        if (id == "menu") active = panel;
        // Disconnecting takes a second press within a few seconds: it ends what you're doing.
        const bool armed = id == "disconnect" && ui.Time() - g_disconnectArmed < 4.f;
        if (armed) R.RectFill(br, kDangerFill, kRadius);
        if (ui.IconButton(b.id, br, armed ? "warning" : b.icon, active, armed ? Tr("btn.pressAgain") : Tr(b.label))) {
            if (id == "monitors") app.ShowMonitors(!app.MonitorsVisible());
            if (id == "games") {
                if (panel && g_page == kGames) {
                    app.ToggleWindow("home", false);
                } else {
                    ShowPage(app, kGames);
                    app.ToggleWindow("home", true);
                }
            }
            if (id == "steamvr") app.LaunchSteamVr();
            if (id == "keyboard") app.ToggleWindow("keyboard");
            if (id == "recenter") app.Recenter();
            // Back to wherever you were.
            if (id == "menu") {
                if (!panel && g_page == kGames) app.RequestGames();
                app.ToggleWindow("home", !panel);
            }
            if (id == "disconnect") {
                if (armed) app.Disconnect();
                else g_disconnectArmed = ui.Time();
            }
        }
        x += bw + gap;
    }
}

// ---- Keyboard --------------------------------------------------------------------

bool g_shift = false, g_shiftLock = false, g_ctrl = false, g_alt = false, g_win = false;
bool g_symbols = false;

void BuildKeyboard(Ui& ui, const Rect& window) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, window);
    static const char* letters[3] = {"qwertyuiop'", "asdfghjkl", "zxcvbnm,."};
    static const char* symbols[3] = {"!@#$%^&*()?", "_=+[]{}\\|", ";:\"`~<>,."};
    const float pad = 22, gap = 8;
    // What was typed, because the PC's screen may be out of view.
    static std::string typed;
    const Rect echo{window.x + pad, window.y + pad - 6, window.w - pad * 2, 50};
    R.RectBorder(echo, Color(0, 0, 0, 1), kRadius, kEdge, 2.f);
    // It could be a password: like on a phone, only the last character shows, unless asked.
    static bool showTyped = false;
    static float lastTyped = 0;
    if (!typed.empty() && ui.Time() - lastTyped > 30.f) typed.clear();
    if (typed.empty()) {
        R.TextIn(Tr("kb.hint"), {echo.x + 14, echo.y, echo.w - 120, echo.h}, 20, kMuted);
    } else {
        const std::string shown = showTyped ? typed : std::string(typed.size() - 1, '*') + typed.back();
        R.TextIn(shown, {echo.x + 14, echo.y, echo.w - 120, echo.h}, 21, kText, Face::Medium);
    }
    if (!typed.empty()) {
        ui.NextLabel(showTyped ? Tr("a11y.hideTyped") : Tr("a11y.showTyped"));
        if (ui.Button("echo-show", {echo.Right() - 136, echo.y + 5, 130, echo.h - 10}, showTyped ? Tr("kb.hideText") : Tr("kb.showText"), nullptr, ButtonStyle::Secondary, true, 20)) showTyped = !showTyped;
    }
    const std::string typedBefore = typed;

    // Every row is 11 key widths wide, so all of them line up at both ends.
    const float top = echo.Bottom() + 12;
    const float keyW = (window.w - pad * 2 - gap * 10) / 11.f, keyH = (window.Bottom() - pad - top - gap * 5) / 6.f;
    auto span = [&](float units) { return units * keyW + (units - 1) * gap; };
    auto rowY = [&](int row) { return top + row * (keyH + gap); };
    auto key = [&](const std::string& id, float x, int row, float units, const std::string& label, const char* icon, bool active, const char* spoken = nullptr, bool quiet = false) {
        if (spoken) ui.NextLabel(spoken);
        // Letters as large as a key can take; words like Shift smaller. Keys that aren't for typing are darker.
        return ui.Button(id.c_str(), {x, rowY(row), span(units), keyH}, label, icon, active ? ButtonStyle::Primary : quiet ? ButtonStyle::Quiet : ButtonStyle::Secondary, true, label.size() == 1 ? 30.f : 21.f);
    };
    auto withModifiers = [&](auto fn) {
        if (g_ctrl) app.SendKey(0x11, true);
        if (g_alt) app.SendKey(0x12, true);
        if (g_win) app.SendKey(0x5B, true);
        if (g_shift) app.SendKey(0x10, true);
        fn();
        if (g_shift) app.SendKey(0x10, false);
        if (g_win) app.SendKey(0x5B, false);
        if (g_alt) app.SendKey(0x12, false);
        if (g_ctrl) app.SendKey(0x11, false);
        g_ctrl = g_alt = g_win = false;
        if (!g_shiftLock) g_shift = false;
    };
    auto tap = [&](uint16_t vk) {
        withModifiers([&] {
            app.SendKey(vk, true);
            app.SendKey(vk, false);
        });
    };
    // Backspace, space and the arrows repeat while held, like on any keyboard: after a
    // moment, then quickly. True when the key should fire now.
    auto repeating = [&](const char* id, bool clicked) {
        static std::string heldId;
        static float heldSince = 0, lastFired = 0;
        static bool repeated = false;
        if (ui.IsHeld(id)) {
            if (heldId != id) {
                heldId = id;
                heldSince = ui.Time();
                repeated = false;
            }
            if (ui.Time() - heldSince > 0.4f && ui.Time() - lastFired > 0.04f) {
                lastFired = ui.Time();
                repeated = true;
                return true;
            }
            return false;
        }
        if (heldId == id) {
            heldId.clear();
            if (repeated) return false;  // it already fired while held
        }
        return clicked;
    };
    auto typeOrCombo = [&](char c, const std::string& text) {
        if (g_ctrl || g_alt || g_win) {
            // Letters and digits are their own key codes; punctuation isn't.
            uint16_t vk = uint16_t(isalpha(c) ? toupper(c) : c);
            switch (c) {
                case '-': vk = 0xBD; break;
                case '/': vk = 0xBF; break;
                case ';': vk = 0xBA; break;
                case '\'': vk = 0xDE; break;
                case '.': vk = 0xBE; break;
                case ',': vk = 0xBC; break;
                case '=': vk = 0xBB; break;
                case '[': vk = 0xDB; break;
                case ']': vk = 0xDD; break;
                case '\\': vk = 0xDC; break;
                case '`': vk = 0xC0; break;
                default: break;
            }
            tap(vk);
        } else {
            app.SendText(text);
            typed += text;
            if (typed.size() > 60) typed.erase(0, typed.size() - 60);
            if (!g_shiftLock) g_shift = false;
        }
    };

    // Top row: what a desktop needs next to the letters.
    float x = window.x + pad;
    if (key("esc", x, 0, 1.4f, "Esc", nullptr, false, nullptr, true)) tap(0x1B);
    x += span(1.4f) + gap;
    if (key("tab", x, 0, 1.4f, "Tab", nullptr, false, nullptr, true)) tap(0x09);
    x += span(1.4f) + gap;
    if (key("ctrl", x, 0, 1.4f, "Ctrl", nullptr, g_ctrl, nullptr, true)) g_ctrl = !g_ctrl;
    x += span(1.4f) + gap;
    if (key("alt", x, 0, 1.4f, "Alt", nullptr, g_alt, nullptr, true)) g_alt = !g_alt;
    x += span(1.4f) + gap;
    if (key("win", x, 0, 1.4f, "Win", nullptr, g_win, nullptr, true)) g_win = !g_win;
    x += span(1.4f) + gap;
    static const struct {
        const char* id;
        const char* icon;
        const char* spoken;
        uint16_t vk;
    } arrows[] = {{"left", "arrow_back", "a11y.left", 0x25}, {"up", "arrow_upward", "a11y.up", 0x26}, {"down", "arrow_downward", "a11y.down", 0x28}, {"right", "arrow_forward", "a11y.right", 0x27}};
    for (const auto& a : arrows) {
        if (repeating(a.id, key(a.id, x, 0, 1.f, "", a.icon, false, Tr(a.spoken), true))) tap(a.vk);
        x += span(1.f) + gap;
    }

    // Digits, always there.
    x = window.x + pad;
    for (int i = 0; i < 10; i++) {
        const char c = "1234567890"[i];
        if (key("d" + std::to_string(i), x, 1, 1.f, std::string(1, c), nullptr, false)) typeOrCombo(c, std::string(1, c));
        x += span(1.f) + gap;
    }
    if (repeating("back", key("back", x, 1, 1.f, "", "backspace", false, Tr("a11y.backspace")))) {
        const bool combo = g_ctrl || g_alt || g_win;
        tap(0x08);
        if (!combo && !typed.empty()) typed.pop_back();
    }

    // The letters, or the symbols.
    const char* const* rows = g_symbols ? symbols : letters;
    for (int row = 0; row < 3; row++) {
        const char* chars = rows[row];
        const int n = int(strlen(chars));
        x = window.x + pad + (row == 1 ? 0.5f * (keyW + gap) : 0.f);
        if (row == 2) {
            if (key("shift", x, 4, 2.f, g_shiftLock ? "Caps" : "Shift", nullptr, g_shift, nullptr, true)) {
                static float lastShift = -100;
                if (g_shiftLock) {
                    g_shiftLock = g_shift = false;
                } else if (g_shift && ui.Time() - lastShift < 0.5f) {
                    g_shiftLock = true;
                } else {
                    g_shift = !g_shift;
                }
                lastShift = ui.Time();
            }
            x += span(2.f) + gap;
        }
        for (int i = 0; i < n; i++) {
            std::string ch(1, chars[i]);
            if (g_shift && !g_symbols) ch[0] = char(toupper(ch[0]));
            if (key("k" + std::to_string(row) + "_" + std::to_string(i), x, row + 2, 1.f, ch, nullptr, false)) typeOrCombo(chars[i], ch);
            x += span(1.f) + gap;
        }
        if (row == 1 && key("enter", x, 3, 1.5f, "", "keyboard_return", false, Tr("a11y.enter"))) {
            tap(0x0D);
            typed.clear();
        }
    }

    // Bottom row.
    x = window.x + pad;
    if (key("sym", x, 5, 2.f, g_symbols ? "ABC" : "#+=", nullptr, false, nullptr, true)) g_symbols = !g_symbols;
    x += span(2.f) + gap;
    if (key("slash", x, 5, 1.f, "/", nullptr, false)) typeOrCombo('/', "/");
    x += span(1.f) + gap;
    if (repeating("space", key("space", x, 5, 7.f, "", "space_bar", false, Tr("a11y.space")))) {
        const bool combo = g_ctrl || g_alt || g_win;
        tap(0x20);
        if (!combo) typed += ' ';
    }
    x += span(7.f) + gap;
    if (key("dash", x, 5, 1.f, "-", nullptr, false)) typeOrCombo('-', "-");
    if (typed != typedBefore) lastTyped = ui.Time();
}

// ---- Stats ---------------------------------------------------------------------------

void BuildStats(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    const auto s = app.CurrentStats();
    struct Item {
        const char* label;
        std::string value;
    };
    const Item items[] = {{Tr("stat.latency"), s.latencyMs > 0 ? FormatMs(s.latencyMs) + " ms" : "–"},
                          {Tr("stat.fps"), s.fps > 0 ? FormatMs(s.fps) + " fps" : "–"},
                          {Tr("stat.bitrate"), s.mbps > 0 ? FormatMs(s.mbps) + " Mbps" : "–"},
                          {Tr("stat.decoding"), s.decodeMs > 0 ? FormatMs(s.decodeMs) + " ms" : "–"},
                          {Tr("stat.network"), s.rttMs > 0 ? FormatMs(s.rttMs) + " ms" : "–"},
                          {Tr("stat.video"), s.codec.empty() ? "–" : s.codec + " " + PixelSize(s.width, s.height)}};
    for (int i = 0; i < 6; i++) {
        const float y = r.y + 16 + i * 38;
        R.Text(items[i].label, r.x + 22, y, 20, kMuted);
        const bool poor = (i == 0 && PoorLatency(s)) || (i == 1 && app.CurrentMode() == Mode::Vr && PoorFrameRate(s, app.Settings().refreshRate));
        R.Text(items[i].value, r.Right() - 22, y, 20, poor ? kWarn : kText, Face::SemiBold, Align::Right);
    }
    // The latency, stage by stage. Bars are shares of the whole; the biggest stands out.
    double total = 0, biggest = 0;
    for (double ms : s.stageMs) {
        total += std::max(ms, 0.0);
        biggest = std::max(biggest, ms);
    }
    float y = r.y + 16 + 6 * 38 + 6;
    ui.Divider(r.x + 22, y, r.w - 44);
    y += 16;
    if (total <= 0) {
        R.Text(Tr("stats.stagesWaiting"), r.x + 22, y, 18, kMuted);
        return;
    }
    R.Text(Tr("stats.stages"), r.x + 22, y, 20, kText, Face::SemiBold);
    y += 36;
    for (int i = 0; i < kStageCount; i++) {
        const double ms = s.stageMs[i];
        if (ms < 0) continue;
        R.Text(Tr((std::string("stage.") + kLatencyStages[i].key).c_str()), r.x + 22, y, 18, kMuted);
        R.Text(FormatMs(ms) + " ms", r.Right() - 22, y, 18, kText, Face::SemiBold, Align::Right);
        const float w = float((r.w - 44) * std::max(ms, 0.0) / total);
        R.RectFill({r.x + 22, y + 25, std::max(w, 2.f), 4}, ms == biggest ? kAccent : kEdge, 2);
        y += 40;
    }
}

// ---- Notices ---------------------------------------------------------------------------

void BuildNotices(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    if (app.Notices().empty()) return;
    const Notice& n = app.Notices().back();
    const Color c = n.level == "error" ? kDanger : n.level == "warning" ? kWarn : kText;
    const float fade = std::min(1.f, (n.life - n.age) * 2.f);
    R.RectBorder(r.Inset(1), kPanel.WithAlpha(fade), kPanelRadius, (n.level == "info" ? kPanelBorder : c).WithAlpha(fade), 2.f);
    R.Icon(n.level == "error" ? "error" : n.level == "warning" ? "warning" : "info", r.x + 42, r.y + r.h / 2, 32, c.WithAlpha(fade));
    // Something to do: the action, and a way to say no to it.
    const bool actions = !n.action.empty();
    const bool stays = actions || n.level == "error";
    const Rect text{r.x + 76, r.y, r.w - (actions ? 390 : stays ? 240 : 100), r.h};
    const float h = WrapHeight(ui, n.text, text.w, 20, 27);
    R.WrapText(n.text, {text.x, r.y + (r.h - h) / 2, text.w, h}, 20, kText.WithAlpha(fade), Face::Medium, 27);
    if (stays && !actions && ui.Button("dismiss", {r.Right() - 134, r.y + 22, 114, r.h - 44}, Tr("btn.dismiss"))) app.DismissNotice();
    if (actions) {
        const std::string label = std::string(n.action == "vr.retry" ? Tr("btn.tryAgain") : n.action.find("estart") != std::string::npos ? Tr("btn.restart") : Tr("btn.open"));
        const std::string action = n.action;
        if (ui.Button("dismiss", {r.Right() - 170 - 8 - 114, r.y + 22, 114, r.h - 44}, Tr("btn.notNow"))) app.DismissNotice();
        else if (ui.Button("action", {r.Right() - 170, r.y + 22, 150, r.h - 44}, label, nullptr, label == Tr("btn.restart") ? ButtonStyle::Danger : ButtonStyle::Primary)) {
            app.DismissNotice();
            app.RunAction(action);
        }
    }
}

// ---- VR launch ---------------------------------------------------------------------------

void BuildLaunch(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    ui.Spinner(r.x + 44, r.y + r.h / 2, 20, kText);
    R.TextIn(app.VrStateText(), {r.x + 80, r.y + 18, r.w - 320, 40}, 26, kText, Face::SemiBold);
    R.TextIn(Tr("launch.takeTime"), {r.x + 80, r.y + 58, r.w - 320, 30}, 20, kMuted);
    // SteamVR keeps starting on the PC; this only stops waiting for it here.
    if (ui.Button("launch-cancel", {r.Right() - 24 - 200, r.y + (r.h - 54) / 2, 200, 54}, Tr("btn.backToDesktop"))) app.ExitVr();
}

// ---- VR overlay ---------------------------------------------------------------------------

void BuildVrOverlay(Ui& ui, const Rect& r) {
    App& app = *g_app;
    UiRenderer& R = ui.R();
    Background(ui, r);
    const float pad = 24;
    R.TextIn(app.VrStateText(), {r.x + pad, r.y + 14, r.w - 400, 54}, 30, kText, Face::SemiBold);
    const auto s = app.CurrentStats();
    if (s.latencyMs > 0) R.TextIn(Trf("dock.latency", {{"ms", FormatMs(s.latencyMs)}}), {r.Right() - 390, r.y + 14, 230, 54}, 21, kMuted, Face::Medium, Align::Right);
    if (ui.Button("close", {r.Right() - pad - 120, r.y + 14, 120, 54}, Tr("btn.resume"), nullptr, ButtonStyle::Primary)) app.ToggleWindow("vr-overlay", false);
    // Left-aligned with the title, in the order you're likely to need them.
    ButtonRow b{r.x + pad, r.y + 88};
    if (b.Add(ui, "desktop", Tr("btn.backToDesktop"))) app.ExitVr();
    if (b.Add(ui, "recenter", Tr("btn.recenter"))) app.Recenter();
    if (b.Add(ui, "stats", app.Settings().showStats ? Tr("vrOverlay.hidePerf") : Tr("vrOverlay.showPerf"))) {
        app.Settings().showStats = !app.Settings().showStats;
        app.SaveSettings();
        app.ToggleWindow("stats", app.Settings().showStats);
    }
    const bool armed = ui.Time() - g_disconnectArmed < 4.f;
    const Rect dis{r.Right() - pad - 180, r.y + 88, 180, 54};
    if (ui.Button("disconnect", dis, armed ? Tr("btn.pressAgain") : Tr("btn.disconnect"), nullptr, armed ? ButtonStyle::Danger : ButtonStyle::Secondary)) {
        if (armed) app.Disconnect();
        else g_disconnectArmed = ui.Time();
    }
}

}  // namespace

void SetPage(int page) { g_page = std::clamp(page, 0, kPageCount - 1); }

void ToggleMenu(App& app) {
    // Before connecting the panel always stays up, so there's nothing to show or hide.
    if (app.CurrentMode() != Mode::Home && app.CurrentMode() != Mode::Connecting) app.ToggleWindow("home");
}

void Build(App& app, WindowManager& wm) {
    g_app = &app;
    auto make = [&](const char* id, int w, int h, float widthM, bool resizable,
                    std::function<void(Ui&, const Rect&)> build) {
        Window* win = wm.Create(id, WindowKind::Ui);
        win->canvasW = w;
        win->canvasH = h;
        win->widthM = widthM;
        win->aspect = float(h) / float(w);
        win->resizable = resizable;
        win->minWidthM = widthM * 0.6f;
        win->maxWidthM = widthM * 2.5f;
        win->build = std::move(build);
        return win;
    };
    make("home", 1240, 860, 1.34f, true, BuildMain)->visible = true;
    Window* dock = make("dock", 1000, 112, 0.84f, false, BuildDock);
    dock->showChrome = true;
    make("keyboard", 1100, 560, 0.66f, true, BuildKeyboard);
    Window* stats = make("stats", 380, 600, 0.30f, false, BuildStats);
    stats->showChrome = true;
    Window* notices = make("notices", 800, 104, 0.68f, false, BuildNotices);
    notices->showChrome = false;
    notices->movable = false;
    Window* launch = make("launch", 760, 108, 0.65f, false, BuildLaunch);
    launch->showChrome = false;
    launch->movable = false;
    make("vr-overlay", 960, 166, 0.94f, false, BuildVrOverlay);
}

}  // namespace rv::screens
