// Immediate-mode UI toolkit for VR panels.
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../gfx/ui_renderer.h"

namespace rv {

namespace theme {
// Black panels and white text. Grey only sets a control or a selected row apart from the black.
inline Color kPanel = Color::Hex(0x000000);
inline Color kPanelBorder = Color::Hex(0x4A4A4A);
inline Color kText = Color::Hex(0xFFFFFF);
inline Color kMuted = Color::Hex(0xC8C8C8);
inline Color kFaint = Color::Hex(0xA6A6A6);
inline Color kEdge = Color::Hex(0x8C8C8C);        // outlines of controls: 6:1 on black
inline constexpr Color kAccent = Color::Hex(0x2EC4FF);        // RinCast's color. Black on it is 10:1
inline constexpr Color kAccentHover = Color::Hex(0x8ADFFF);
inline constexpr Color kOnAccent = Color::Hex(0x000000);
inline constexpr Color kGood = Color::Hex(0x4ADE80);
inline constexpr Color kWarn = Color::Hex(0xFFB84D);
inline constexpr Color kDanger = Color::Hex(0xFF6B63);
inline constexpr Color kDangerFill = Color::Hex(0xA8201A);
inline constexpr Color kDangerHover = Color::Hex(0xC62B24);
inline Color kControl = Color::Hex(0x3A3A3A);
inline Color kControlHover = Color::Hex(0x4D4D4D);
inline constexpr Color kControlPress = Color::Hex(0x5A5A5A);
inline Color kSelected = Color::Hex(0x4D4D4D);    // a chosen row, set apart from buttons
inline constexpr Color kQuiet = Color::Hex(0x262626);
inline Color kHover = Color::Hex(0x333333);          // under the pointer: rows, checkboxes, icon buttons
inline Color kSelectedHover = Color::Hex(0x5C5C5C);  // a chosen row under the pointer
inline Color kSelectedPress = Color::Hex(0x6B6B6B);
inline Color kLine = Color::Hex(0x4A4A4A);
inline void SetHighContrast(bool on) {
    kPanelBorder = Color::Hex(on ? 0xFFFFFF : 0x4A4A4A);
    kMuted = Color::Hex(on ? 0xFFFFFF : 0xC8C8C8);
    kFaint = Color::Hex(on ? 0xE6E6E6 : 0xA6A6A6);
    kEdge = Color::Hex(on ? 0xFFFFFF : 0x8C8C8C);
    kControl = Color::Hex(on ? 0x4A4A4A : 0x3A3A3A);
    kControlHover = Color::Hex(on ? 0x666666 : 0x4D4D4D);
    kSelected = Color::Hex(on ? 0x666666 : 0x4D4D4D);
    kHover = Color::Hex(on ? 0x4D4D4D : 0x333333);
    kSelectedHover = Color::Hex(on ? 0x7A7A7A : 0x5C5C5C);
    kSelectedPress = Color::Hex(on ? 0x8C8C8C : 0x6B6B6B);
    kLine = Color::Hex(on ? 0xBDBDBD : 0x4A4A4A);
}
inline constexpr float kRadius = 4.f;
inline constexpr float kPanelRadius = 10.f;
}  // namespace theme

struct PointerState {
    bool hovering = false;   // ray is over this panel
    Vec2 pos;                // panel px
    bool down = false;       // trigger held
    bool pressed = false;    // went down this frame
    bool released = false;   // went up this frame
    float scroll = 0;        // thumbstick / wheel, px per frame
};

enum class ButtonStyle { Primary, Secondary, Quiet, Danger };

class Ui {
   public:
    explicit Ui(UiRenderer& r) : m_r(r) {}

    void Begin(const std::string& scope, const PointerState& p, float dt, float time);
    void End();
    UiRenderer& R() { return m_r; }
    float Time() const { return m_time; }
    float Dt() const { return m_dt; }
    bool WantsRedraw() const { return m_animating; }
    bool PointerCaptured() const { return m_active != 0; }
    // Fires once when a widget was clicked (for haptic feedback).
    bool ConsumeClick() {
        const bool c = m_clicked;
        m_clicked = false;
        return c;
    }
    // Text for the widget the pointer just moved to, for reading aloud.
    std::string ConsumeSpeech() {
        std::string s;
        s.swap(m_speech);
        return s;
    }
    // Says this next, e.g. a code or a state that changed by itself.
    void Announce(const std::string& text) { m_speech = text; }
    // Names the next widget, for widgets whose labels are drawn separately or are only an icon.
    void NextLabel(const std::string& label) { m_nextLabel = label; }
    bool ConsumeHoverChange() {
        const bool c = m_hoverChanged;
        m_hoverChanged = false;
        return c;
    }

    // Widgets
    bool Button(const char* id, const Rect& r, const std::string& label, const char* icon = nullptr,
                ButtonStyle style = ButtonStyle::Secondary, bool enabled = true, float textSize = 21.f);
    bool IconButton(const char* id, const Rect& r, const char* icon, bool active = false, const char* label = nullptr,
                    bool enabled = true);
    // A checkbox and its label on one line, 48 px high. r can be taller, to take in a hint below.
    bool Check(const char* id, const Rect& r, const std::string& label, bool& value, bool enabled = true);
    // One option of a group, with an optional note at the right. True when it was newly chosen.
    bool Radio(const char* id, const Rect& r, const std::string& label, bool selected, const std::string& note = {}, bool enabled = true);
    // A slider that's a bar with its value written in it.
    // mark: a value to show a tick at, such as a slider's normal setting.
    bool Bar(const char* id, const Rect& r, float& value, float min, float max, float step, const std::string& text, bool enabled = true, float mark = -1);
    // Clickable full-width row; draws hover background, returns clicked.
    bool Row(const char* id, const Rect& r, bool selected = false, bool enabled = true);
    // The same, for something that draws its own hover state.
    bool Hit(const char* id, const Rect& r, bool enabled = true);
    void Spinner(float cx, float cy, float radius, const Color& c);
    void Divider(float x, float y, float w);

    // Scroll region: content drawn between Begin/End is offset by the scroll amount.
    // With dragToScroll, pressing inside and moving turns into a scroll and cancels the
    // press — right for big row lists, wrong for dense forms where a trigger pull alone
    // moves the ray that far. Stick scrolling works either way.
    float BeginScroll(const char* id, const Rect& r, float contentHeight, bool dragToScroll = true);
    void EndScroll();

    // Damped animation value keyed by id.
    float Anim(const char* id, float target, float speed = 18.f);
    bool IsHot(const char* id) const;
    bool IsHeld(const char* id) const;

    // What's under the laser gets this outline, whatever it is.
    // Under the pointer: a lighter background, drawn before the content on top of it.
    void HoverFill(const Rect& r, float amount);
    // Over a picture, which has no background to change: it lightens.
    void HoverLighten(const Rect& r, float amount);

   private:
    uint64_t Id(const char* id) const;
    bool Interact(uint64_t id, const Rect& r, bool enabled, bool& hovered, bool& held);

    UiRenderer& m_r;
    std::string m_scope;
    PointerState m_p;
    float m_dt = 0, m_time = 0;
    uint64_t m_hot = 0, m_lastHot = 0;
    uint64_t m_active = 0;
    struct WindowState {
        uint64_t active = 0;  // widget holding the press
        uint64_t hot = 0;     // widget hovered last frame
    };
    std::unordered_map<std::string, WindowState> m_windowState;
    std::unordered_map<uint64_t, float> m_anim;
    std::unordered_map<uint64_t, float> m_scroll;
    std::vector<Rect> m_clipStack;
    std::vector<float> m_scrollOffset;
    bool m_animating = false;
    bool m_clicked = false;
    bool m_hoverChanged = false;
    // Dragging a scroll region (works with pinches as well as triggers).
    std::string m_speech, m_nextLabel, m_hotLabel;
    uint64_t m_scrollDrag = 0;
    float m_scrollDragY = 0, m_scrollDragDistance = 0;
    bool m_scrolling = false;
    // Pulling the trigger moves the ray: a bar stays put until it's clearly being dragged.
    float m_barPressX = 0;
    bool m_barMoving = false;
};

}  // namespace rv
