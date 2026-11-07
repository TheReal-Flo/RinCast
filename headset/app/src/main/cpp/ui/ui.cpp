#include "ui.h"

#include <cmath>
#include <algorithm>
#include <cstdio>

#include "app/strings.h"

namespace rv {

uint64_t Ui::Id(const char* id) const {
    // FNV-1a over scope + id.
    uint64_t h = 1469598103934665603ULL;
    for (char c : m_scope) h = (h ^ uint8_t(c)) * 1099511628211ULL;
    h = (h ^ '/') * 1099511628211ULL;
    for (const char* p = id; *p; p++) h = (h ^ uint8_t(*p)) * 1099511628211ULL;
    return h;
}

void Ui::Begin(const std::string& scope, const PointerState& p, float dt, float time) {
    // One Ui serves every window in turn, so the pressed and hovered widgets are
    // kept per window; otherwise drawing one window would cancel a press in another.
    m_scope = scope;
    m_p = p;
    m_dt = dt;
    m_time = time;
    m_animating = false;
    WindowState& s = m_windowState[scope];
    m_active = s.active;
    m_lastHot = s.hot;
    m_hot = 0;
    if (!p.down && !p.released) m_active = 0;
    m_nextLabel.clear();
}

void Ui::End() {
    if (m_hot != m_lastHot && m_hot != 0) {
        m_hoverChanged = true;
        m_speech = m_hotLabel;
    }
    if (m_p.released) m_active = 0;
    WindowState& s = m_windowState[m_scope];
    s.active = m_active;
    s.hot = m_hot;
}

bool Ui::IsHot(const char* id) const { return m_hot == Id(id); }
bool Ui::IsHeld(const char* id) const { return m_active == Id(id) && m_p.down; }

float Ui::Anim(const char* id, float target, float speed) {
    const uint64_t key = Id(id);
    auto it = m_anim.find(key);
    if (it == m_anim.end()) it = m_anim.emplace(key, target).first;
    it->second = Damp(it->second, target, speed, m_dt);
    if (std::fabs(it->second - target) > 0.002f) m_animating = true;
    else it->second = target;
    return it->second;
}

bool Ui::Interact(uint64_t id, const Rect& r, bool enabled, bool& hovered, bool& held) {
    Rect hit = r;
    if (!m_clipStack.empty()) {
        const Rect& c = m_clipStack.back();
        if (!c.Contains(m_p.pos)) {
            hovered = held = false;
            return false;
        }
    }
    hovered = enabled && m_p.hovering && hit.Contains(m_p.pos) && (m_active == 0 || m_active == id);
    if (hovered) m_hot = id;
    if (hovered && m_p.pressed) m_active = id;
    held = m_active == id && m_p.down;
    // Pulling the trigger or pinching moves the ray a little: the press stays on the
    // widget it started on, with some tolerance when it lets go.
    const bool clicked = enabled && m_active == id && m_p.released && hit.Inset(-30.f).Contains(m_p.pos);
    if (clicked) m_clicked = true;
    return clicked;
}

bool Ui::Button(const char* id, const Rect& r, const std::string& label, const char* icon, ButtonStyle style,
                bool enabled, float textSize) {
    bool hovered, held;
    const bool clicked = Interact(Id(id), r, enabled, hovered, held);
    if (hovered) m_hotLabel = label.empty() ? m_nextLabel : label;
    m_nextLabel.clear();
    const std::string key = std::string(id) + "#h";
    const float h = Anim(key.c_str(), hovered ? 1.f : 0.f, 30.f);
    const float p = Anim((key + "p").c_str(), held ? 1.f : 0.f, 40.f);
    Color bg, fg = theme::kText;
    switch (style) {
        case ButtonStyle::Primary:
            bg = Mix(theme::kAccent, theme::kAccentHover, h);
            fg = theme::kOnAccent;
            break;
        case ButtonStyle::Secondary: bg = Mix(theme::kControl, theme::kControlHover, h); break;
        case ButtonStyle::Quiet: bg = Mix(theme::kQuiet, theme::kHover, h); break;
        case ButtonStyle::Danger: bg = Mix(theme::kDangerFill, theme::kDangerHover, h); break;
    }
    if (held) bg = style == ButtonStyle::Primary ? theme::kAccent : style == ButtonStyle::Danger ? theme::kDangerFill : theme::kControlPress;
    if (!enabled) {
        bg = theme::kControl.WithAlpha(0.55f);
        fg = theme::kFaint;
    }
    const Rect br = r.Inset(p * 1.5f);
    m_r.RectFill(br, bg, theme::kRadius);
    const float size = textSize;
    const float iconSize = 26.f;
    const float textW = label.empty() ? 0 : m_r.Measure(label, size, Face::SemiBold);
    const float gap = icon && !label.empty() ? 10.f : 0.f;
    const float total = (icon ? iconSize : 0) + gap + textW;
    float x = br.x + (br.w - total) / 2;
    if (icon) {
        m_r.Icon(icon, x + iconSize / 2, br.y + br.h / 2, iconSize, fg);
        x += iconSize + gap;
    }
    if (!label.empty()) m_r.TextIn(label, {x, br.y, textW + 2, br.h}, size, fg, Face::SemiBold);
    return clicked;
}

bool Ui::IconButton(const char* id, const Rect& r, const char* icon, bool active, const char* label, bool enabled) {
    bool hovered, held;
    const bool clicked = Interact(Id(id), r, enabled, hovered, held);
    if (hovered) m_hotLabel = label ? label : !m_nextLabel.empty() ? m_nextLabel : icon;
    m_nextLabel.clear();
    const std::string key = std::string(id) + "#h";
    const float h = Anim(key.c_str(), hovered ? 1.f : 0.f, 30.f);
    if (held) m_r.RectFill(r, theme::kControlPress, theme::kRadius);
    else HoverFill(r, h);
    Color fg = !enabled ? theme::kFaint : theme::kText;
    if (label) {
        // Icon and label stay together, centered, whatever the button's height.
        const float iconH = 36, textH = 26, top = r.y + (r.h - iconH - textH - 2) / 2;
        m_r.Icon(icon, r.x + r.w / 2, top + iconH / 2, iconH, fg);
        m_r.TextIn(label, {r.x + 2, top + iconH + 2, r.w - 4, textH}, 18, fg, active ? Face::SemiBold : Face::Medium, Align::Center);
    } else {
        m_r.Icon(icon, r.x + r.w / 2, r.y + r.h / 2, r.h * 0.56f, fg);
    }
    if (active) m_r.RectFill({r.x + 16, r.Bottom() - 5, r.w - 32, 5}, theme::kAccent);
    return clicked;
}

bool Ui::Check(const char* id, const Rect& r, const std::string& label, bool& value, bool enabled) {
    bool hovered, held;
    const bool clicked = Interact(Id(id), r, enabled, hovered, held);
    if (clicked) value = !value;
    if (hovered) m_hotLabel = (m_nextLabel.empty() ? label : m_nextLabel) + (value ? ", on" : ", off");
    if (clicked) m_speech = value ? Tr("speech.on") : Tr("speech.off");
    m_nextLabel.clear();
    const std::string key = std::string(id) + "#c";
    const float h = Anim(key.c_str(), hovered ? 1.f : 0.f, 30.f);
    if (held) m_r.RectFill(r, theme::kControlPress, theme::kRadius);
    else HoverFill(r, h);
    const Rect box{r.x + 8, r.y + 9, 30, 30};
    if (value) {
        m_r.RectFill(box, enabled ? theme::kAccent : theme::kControlHover, 3);
        m_r.Icon("check", box.x + 15, box.y + 15, 28, enabled ? theme::kOnAccent : theme::kText);
    } else {
        m_r.RectBorder(box, Color(0, 0, 0, 1), 3, enabled ? theme::kEdge : theme::kControlHover, 2.f);
    }
    m_r.TextIn(label, {box.Right() + 14, r.y, r.w - 60, 48}, 21, enabled ? theme::kText : theme::kFaint);
    return clicked;
}

bool Ui::Radio(const char* id, const Rect& r, const std::string& label, bool selected, const std::string& note, bool enabled) {
    bool hovered, held;
    const bool clicked = Interact(Id(id), r, enabled, hovered, held);
    if (hovered) m_hotLabel = (m_nextLabel.empty() ? "" : m_nextLabel + ": ") + label + (note.empty() ? "" : ", " + note) + (selected ? ", selected" : "");
    if (clicked && !selected) m_speech = label + ", selected";
    m_nextLabel.clear();
    const std::string key = std::string(id) + "#o";
    const float h = Anim(key.c_str(), hovered ? 1.f : 0.f, 30.f);
    if (held) m_r.RectFill(r, theme::kControlPress, theme::kRadius);
    else HoverFill(r, h);
    const float cx = r.x + 23, cy = r.y + r.h / 2;
    if (!selected) m_r.RectBorder({cx - 15, cy - 15, 30, 30}, Color(0, 0, 0, 1), 15, enabled ? theme::kEdge : theme::kControlHover, 2.f);
    if (selected) {
        m_r.Circle(cx, cy, 15, enabled ? theme::kAccent : theme::kControlHover);
        m_r.Circle(cx, cy, 6, enabled ? theme::kOnAccent : theme::kText);
    }
    const float textX = r.x + 52;
    m_r.TextIn(label, {textX, r.y, r.w - (textX - r.x) - 4, r.h}, 21, enabled ? theme::kText : theme::kFaint);
    if (!note.empty()) m_r.TextIn(note, {r.x, r.y, r.w - 2, r.h}, 20, theme::kMuted, Face::Medium, Align::Right);
    return clicked && !selected;
}

bool Ui::Bar(const char* id, const Rect& r, float& value, float min, float max, float step, const std::string& text, bool enabled, float mark) {
    bool hovered, held;
    Interact(Id(id), r, enabled, hovered, held);
    if (hovered) m_hotLabel = (m_nextLabel.empty() ? "" : m_nextLabel + ", ") + text;
    m_nextLabel.clear();
    bool changed = false;
    if (held && m_p.pressed) {
        m_barPressX = m_p.pos.x;
        m_barMoving = false;
    }
    if (held && !m_p.pressed && std::fabs(m_p.pos.x - m_barPressX) > 12.f) m_barMoving = true;
    if (held && (m_p.pressed || m_barMoving)) {
        float v = min + Clamp01((m_p.pos.x - r.x - 3) / (r.w - 6)) * (max - min);
        if (step > 0) v = std::round(v / step) * step;
        v = std::clamp(v, min, max);
        if (v != value) {
            value = v;
            changed = true;
        }
    }
    const float t = Clamp01((value - min) / (max - min));
    // A line and a solid handle, in a taller area that's easy to hit. The value is written by whoever labels it.
    // The line runs the full width, lining up with the label above; the handle stays on it.
    const float cy = r.y + r.h / 2, x0 = r.x, x1 = r.Right();
    const float hx = x0 + 8 + (x1 - x0 - 16) * t;
    HoverFill(r.Inset(-6), hovered || held ? 1.f : 0.f);
    m_r.RectFill({x0, cy - 3, x1 - x0, 6}, !enabled ? theme::kControl : theme::kEdge, 3);
    if (enabled) m_r.RectFill({x0, cy - 3, hx - x0, 6}, theme::kAccent, 3);
    // The usual value, unless the handle is on it anyway.
    if (mark >= min && mark <= max && std::fabs(value - mark) > (max - min) * 0.01f) {
        const float mx = x0 + 8 + (x1 - x0 - 16) * (mark - min) / (max - min);
        m_r.RectFill({mx - 1, cy + 8, 2, 10}, theme::kMuted);
    }
    if (enabled) m_r.RectFill({hx - 8, cy - 18, 16, 36}, theme::kText, 3);
    return changed;
}

bool Ui::Row(const char* id, const Rect& r, bool selected, bool enabled) {
    bool hovered, held;
    const bool clicked = Interact(Id(id), r, enabled, hovered, held);
    if (hovered) m_hotLabel = m_nextLabel + (selected ? ", selected" : "");
    m_nextLabel.clear();
    const std::string key = std::string(id) + "#r";
    const float h = Anim(key.c_str(), hovered ? 1.f : 0.f, 30.f);
    if (selected) m_r.RectFill(r, held ? theme::kSelectedPress : Mix(theme::kSelected, theme::kSelectedHover, h), theme::kRadius);
    else if (held) m_r.RectFill(r, theme::kControlPress, theme::kRadius);
    else HoverFill(r, h);
    return clicked;
}

void Ui::HoverFill(const Rect& r, float amount) {
    if (amount > 0.01f) m_r.RectFill(r, theme::kHover.WithAlpha(amount), theme::kRadius);
}

void Ui::HoverLighten(const Rect& r, float amount) {
    if (amount > 0.01f) m_r.RectFill(r, Color(1, 1, 1, 0.25f * amount), theme::kRadius);
}

bool Ui::Hit(const char* id, const Rect& r, bool enabled) {
    bool hovered, held;
    const bool clicked = Interact(Id(id), r, enabled, hovered, held);
    if (hovered) m_hotLabel = m_nextLabel;
    m_nextLabel.clear();
    return clicked;
}

void Ui::Spinner(float cx, float cy, float radius, const Color& c) {
    m_r.Arc(cx, cy, radius, radius * 0.22f, std::fmod(m_time * 5.5f, 6.2831853f), 4.4f, c);
    m_animating = true;
}

void Ui::Divider(float x, float y, float w) { m_r.RectFill({x, y, w, 2.f}, theme::kLine); }

float Ui::BeginScroll(const char* id, const Rect& r, float contentHeight) {
    const uint64_t key = Id(id);
    float& offset = m_scroll[key];
    if (m_p.hovering && r.Contains(m_p.pos)) offset += m_p.scroll;
    // Drag to scroll. A press that turns into a drag no longer clicks what it started on.
    if (m_p.pressed && m_p.hovering && r.Contains(m_p.pos)) {
        m_scrollDrag = key;
        m_scrollDragY = m_p.pos.y;
        m_scrollDragDistance = 0;
        m_scrolling = false;
    }
    if (m_scrollDrag == key) {
        if (m_p.down && m_p.hovering) {
            const float delta = m_scrollDragY - m_p.pos.y;
            m_scrollDragY = m_p.pos.y;
            // How far the ray got from the press, not the path it shook along.
            m_scrollDragDistance += delta;
            if (contentHeight > r.h && (m_scrolling || std::fabs(m_scrollDragDistance) > 40.f)) {
                m_scrolling = true;
                offset += delta;
                m_active = 1;  // no widget has this id
            }
        } else {
            m_scrollDrag = 0;
        }
    }
    const float maxOffset = std::max(0.f, contentHeight - r.h);
    offset = std::clamp(offset, 0.f, maxOffset);
    m_r.PushClip(r);
    m_clipStack.push_back(r);
    m_scrollOffset.push_back(offset);
    // Scrollbar
    if (maxOffset > 0) {
        const float barH = std::max(40.f, r.h * r.h / contentHeight);
        const float barY = r.y + (r.h - barH) * (offset / maxOffset);
        m_r.RectFill({r.Right() - 8, barY, 6, barH}, theme::kEdge, 2);
    }
    return offset;
}

void Ui::EndScroll() {
    m_r.PopClip();
    m_clipStack.pop_back();
    m_scrollOffset.pop_back();
}

}  // namespace rv
