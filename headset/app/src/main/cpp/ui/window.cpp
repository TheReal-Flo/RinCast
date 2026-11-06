#include "window.h"

#include <algorithm>
#include <cstring>
#include <cmath>

namespace rv {

Pose Window::ChromePose() const {
    // The handle bar sits under the content; for curved windows it follows the
    // chord at the bottom centre.
    const float y = -(HeightM() / 2 + ChromeGapM() + ChromeHeightM() / 2);
    return {pose.orientation, pose.Transform({0, y, 0})};
}

bool WindowManager::Init(UiRenderer* ui, SceneRenderer* scene) {
    m_r = ui;
    m_scene = scene;
    m_ui = std::make_unique<Ui>(*ui);
    return true;
}

Window* WindowManager::Create(const std::string& id, WindowKind kind) {
    if (Window* w = Find(id)) return w;
    auto w = std::make_unique<Window>();
    w->id = id;
    w->kind = kind;
    m_windows.push_back(std::move(w));
    return m_windows.back().get();
}

Window* WindowManager::Find(const std::string& id) {
    for (auto& w : m_windows)
        if (w->id == id && !w->removed) return w.get();
    return nullptr;
}

// Removal is deferred: a window can be removed from inside another window's content
// (for example the Disconnect button), while the window list is being iterated. The
// window is hidden right away and freed at the start of the next Update().
void WindowManager::Remove(const std::string& id) {
    for (auto& w : m_windows) {
        if (w->id != id || w->removed) continue;
        if (m_drag.window == w.get()) m_drag = {};
        w->removed = true;
        w->visible = false;
        w->appear = 0;
        return;
    }
}

void WindowManager::PurgeRemoved() {
    for (auto it = m_windows.begin(); it != m_windows.end();) {
        if ((*it)->removed) {
            (*it)->rt.Destroy();
            (*it)->chromeRt.Destroy();
            m_pointers.erase((*it)->id);
            m_chromeHover.erase((*it)->id);
            m_order.erase(std::remove(m_order.begin(), m_order.end(), it->get()), m_order.end());
            it = m_windows.erase(it);
        } else {
            ++it;
        }
    }
}

Quat WindowManager::FaceTowards(const Vec3& position, const Vec3& target) {
    // Window +Z faces the target; keep it upright, allow gentle pitch.
    Vec3 f = position - target;
    const float horizontal = std::sqrt(f.x * f.x + f.z * f.z);
    const float pitch = std::atan2(f.y, std::max(horizontal, 0.01f));
    const float clampedPitch = std::clamp(pitch, -0.6f, 0.6f);
    f = Normalize(Vec3{f.x, std::tan(clampedPitch) * horizontal, f.z});
    return LookRotation(f, {0, 1, 0});
}

Pose WindowManager::PlaceInFront(const Pose& head, float distance, float yawOffsetDeg, float heightOffset,
                                 float pitchDeg) {
    Vec3 fwd = head.Forward();
    fwd.y = 0;
    fwd = Normalize(fwd);
    const Quat yaw = Quat::Yaw(-yawOffsetDeg * kPi / 180.f);
    const Vec3 dir = yaw.Rotate(fwd);
    const float pitch = pitchDeg * kPi / 180.f;
    Vec3 pos = head.position + dir * (distance * std::cos(pitch));
    pos.y = head.position.y + heightOffset + distance * std::sin(pitch);
    return {FaceTowards(pos, head.position), pos};
}

// Ray against a window's content (flat or cylindrical) and chrome.
static bool HitContent(const Window& w, const Vec3& origin, const Vec3& dir, WindowHit& out) {
    const float hw = w.widthM / 2, hh = w.HeightM() / 2;
    const Quat inv = w.pose.orientation.Conjugate();
    const Vec3 o = inv.Rotate(origin - w.pose.position);
    const Vec3 d = inv.Rotate(dir);
    if (w.curveRadius <= 0) {
        if (std::fabs(d.z) < 1e-6f) return false;
        const float t = -o.z / d.z;
        if (t < 0) return false;
        const Vec3 p = o + d * t;
        if (std::fabs(p.x) > hw || std::fabs(p.y) > hh) return false;
        out.distance = t;
        out.uv = {p.x / w.widthM + 0.5f, 0.5f - p.y / w.HeightM()};
        out.point = origin + dir * t;
        out.normal = w.pose.orientation.Rotate({0, 0, 1});
        return true;
    }
    // Cylinder centred at local (0, *, R), surface passes through the origin.
    const float R = w.curveRadius;
    const float a = d.x * d.x + d.z * d.z;
    const float b = 2 * (o.x * d.x + (o.z - R) * d.z);
    const float c = o.x * o.x + (o.z - R) * (o.z - R) - R * R;
    const float disc = b * b - 4 * a * c;
    if (a < 1e-8f || disc < 0) return false;
    const float sq = std::sqrt(disc);
    for (float t : {(-b + sq) / (2 * a), (-b - sq) / (2 * a)}) {
        if (t < 0) continue;
        const Vec3 p = o + d * t;
        if (p.z > R) continue;  // only the half of the cylinder behind the axis
        const float angle = std::atan2(p.x, R - p.z);
        const float arc = angle * R;
        if (std::fabs(arc) > hw || std::fabs(p.y) > hh) continue;
        out.distance = t;
        out.uv = {arc / w.widthM + 0.5f, 0.5f - p.y / w.HeightM()};
        out.point = origin + dir * t;
        out.normal = w.pose.orientation.Rotate(Normalize(Vec3{-p.x, 0, R - p.z}));
        return true;
    }
    return false;
}

WindowHit WindowManager::Raycast(const Vec3& origin, const Vec3& dir) {
    WindowHit best;
    for (auto& wp : m_windows) {
        Window& w = *wp;
        if (!w.visible || w.appear < 0.6f) continue;
        WindowHit h;
        if (HitContent(w, origin, dir, h) && h.distance < best.distance) {
            h.window = &w;
            h.zone = HitZone::Content;
            best = h;
        }
        if (w.showChrome && (w.movable || w.resizable)) {
            const Pose cp = w.ChromePose();
            Vec2 local;
            const float t = RayRect(origin, dir, cp, w.widthM / 2, w.ChromeHeightM() / 2, &local);
            if (t > 0 && t < best.distance) {
                const float u = local.x / w.widthM + 0.5f;
                HitZone zone = HitZone::None;
                const float handleHalf = std::min(0.16f, 0.12f / std::max(w.widthM, 0.3f)) + 0.06f;
                if (w.movable && std::fabs(u - 0.5f) < handleHalf) zone = HitZone::Handle;
                else if (w.resizable && u < 0.1f) zone = HitZone::ResizeLeft;
                else if (w.resizable && u > 0.9f) zone = HitZone::ResizeRight;
                if (zone != HitZone::None) {
                    best.window = &w;
                    best.zone = zone;
                    best.distance = t;
                    best.uv = {u, 0.5f};
                    best.point = origin + dir * t;
                    best.normal = cp.orientation.Rotate({0, 0, 1});
                }
            }
        }
    }
    return best;
}

void WindowManager::Update(const HandRay hands[2], const Pose& head, float dt, float time, WindowHit hits[2]) {
    PurgeRemoved();
    for (int h = 0; h < 2; h++) hits[h] = hands[h].valid ? Raycast(hands[h].origin, hands[h].dir) : WindowHit{};

    // Appear / disappear animations.
    for (auto& w : m_windows) w->appear = Damp(w->appear, w->visible ? 1.f : 0.f, 14.f, dt);

    // ---- dragging (move / resize) ----
    if (m_drag.window) {
        const HandRay& r = hands[m_drag.hand];
        Window& w = *m_drag.window;
        if (!r.valid || !r.triggerDown || !w.visible) {
            m_drag = {};
        } else if (m_drag.zone == HitZone::Handle) {
            m_drag.distance = std::clamp(m_drag.distance + r.push * dt * 1.6f, 0.35f, 8.f);
            const Vec3 grab = r.origin + r.dir * m_drag.distance;
            const Quat facing = FaceTowards(grab, head.position);
            const Vec3 target = grab + facing.Rotate(m_drag.offsetLocal);
            w.pose.position = Damp(w.pose.position, target, 22.f, dt);
            w.pose.orientation = Slerp(w.pose.orientation, FaceTowards(w.pose.position, head.position), Clamp01(dt * 18.f));
        } else {
            // Resize: measure the pointer's horizontal distance from the window centre on its plane.
            const Vec3 n = w.pose.orientation.Rotate({0, 0, 1});
            const float denom = Dot(r.dir, n);
            if (std::fabs(denom) > 1e-5f) {
                const float t = Dot(w.pose.position - r.origin, n) / denom;
                const Vec3 p = r.origin + r.dir * t;
                const Vec3 local = w.pose.orientation.Conjugate().Rotate(p - w.pose.position);
                const float span = std::max(0.05f, std::fabs(local.x));
                const float width = std::clamp(m_drag.startWidth * span / m_drag.startSpan, w.minWidthM, w.maxWidthM);
                // Keep the top edge in place while growing (feels natural, like the Quest shell).
                const float oldH = w.HeightM();
                w.widthM = Damp(w.widthM, width, 25.f, dt);
                w.pose.position = w.pose.position + w.pose.orientation.Rotate({0, (oldH - w.HeightM()) / 2, 0});
            }
        }
    } else {
        for (int h = 0; h < 2; h++) {
            const WindowHit& hit = hits[h];
            if (!hit.window || !hands[h].triggerPressed) continue;
            if (hit.zone == HitZone::Handle || hit.zone == HitZone::ResizeLeft || hit.zone == HitZone::ResizeRight) {
                Window& w = *hit.window;
                m_drag.window = &w;
                m_drag.hand = h;
                m_drag.zone = hit.zone;
                m_drag.distance = hit.distance;
                m_drag.offsetLocal = w.pose.orientation.Conjugate().Rotate(w.pose.position - hit.point);
                m_drag.startWidth = w.widthM;
                const Vec3 local = w.pose.orientation.Conjugate().Rotate(hit.point - w.pose.position);
                m_drag.startSpan = std::max(0.05f, std::fabs(local.x));
                m_hapticPending = true;
                m_hapticHand = h;
                break;
            }
        }
    }

    // ---- pointer routing to UI windows ----
    m_pointers.clear();
    int primary = -1;
    for (int h = 1; h >= 0; h--) {  // right hand wins ties
        if (hits[h].window && hits[h].zone == HitZone::Content && hits[h].window->kind == WindowKind::Ui) {
            if (primary < 0 || hands[h].triggerDown || hands[h].triggerPressed) primary = h;
        }
    }
    if (primary >= 0 && !m_drag.window) {
        const WindowHit& hit = hits[primary];
        Window& w = *hit.window;
        PointerState p;
        p.hovering = true;
        p.pos = {hit.uv.x * w.canvasW, hit.uv.y * w.canvasH};
        p.down = hands[primary].triggerDown;
        p.pressed = hands[primary].triggerPressed;
        p.released = hands[primary].triggerReleased;
        p.scroll = -hands[primary].scroll * 900.f * dt;
        m_pointers[w.id] = p;
        m_hapticHand = primary;
    }
    // Releases must reach the window that captured the press even if the ray left it.
    for (int h = 0; h < 2; h++) {
        if (hands[h].triggerReleased && primary != h) {
            for (auto& w : m_windows)
                if (w->kind == WindowKind::Ui && !m_pointers.count(w->id)) {
                    PointerState p;
                    p.released = true;
                    m_pointers[w->id] = p;
                }
        }
    }
    // Chrome hover (for highlighting).
    for (auto& w : m_windows) {
        HitZone hover = HitZone::None;
        for (int h = 0; h < 2; h++)
            if (hits[h].window == w.get() && hits[h].zone != HitZone::Content) hover = hits[h].zone;
        if (m_drag.window == w.get()) hover = m_drag.zone;
        if (m_chromeHover[w->id] != hover) {
            m_chromeHover[w->id] = hover;
            if (hover != HitZone::None) {
                m_hapticPending = true;
            }
        }
    }
    (void)time;
}

void WindowManager::RenderChrome(Window& w, HitZone hover) {
    const int cw = 600, ch = 60;
    if (!w.chromeRt.fbo) w.chromeRt.Create(cw, ch, GL_SRGB8_ALPHA8, false, true);
    glBindFramebuffer(GL_FRAMEBUFFER, w.chromeRt.fbo);
    glViewport(0, 0, cw, ch);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    m_r->Begin(cw, ch);
    // Fixed physical size for the handle regardless of window width.
    const float pxPerM = cw / w.widthM;
    const float handleW = std::min(cw * 0.5f, 0.16f * pxPerM);
    const bool handleHot = hover == HitZone::Handle;
    m_r->RectFill({cw / 2.f - handleW / 2, ch / 2.f - 5, handleW, 10}, handleHot ? Color(1, 1, 1, 1) : Color(1, 1, 1, 0.7f), 2);
    if (w.resizable) {  // always there, so it can be found: faint until pointed at
        const Color c = hover == HitZone::None ? Color(1, 1, 1, 0.3f) : Color(1, 1, 1, 0.6f);
        const float s = std::min(48.f, 0.05f * pxPerM);
        m_r->RectFill({6, 4, s, 8}, hover == HitZone::ResizeLeft ? Color(1, 1, 1, 0.95f) : c, 1);
        m_r->RectFill({6, 4, 8, s * 0.6f}, hover == HitZone::ResizeLeft ? Color(1, 1, 1, 0.95f) : c, 1);
        m_r->RectFill({cw - 6 - s, 4, s, 8}, hover == HitZone::ResizeRight ? Color(1, 1, 1, 0.95f) : c, 1);
        m_r->RectFill({cw - 14.f, 4, 8, s * 0.6f}, hover == HitZone::ResizeRight ? Color(1, 1, 1, 0.95f) : c, 1);
    }
    m_r->End();
    glBindTexture(GL_TEXTURE_2D, w.chromeRt.color.id);
    glGenerateMipmap(GL_TEXTURE_2D);
}

void WindowManager::RenderCanvases(float dt, float time) {
    for (auto& wp : m_windows) {
        Window& w = *wp;
        if (w.appear < 0.01f && !w.visible) continue;
        if (w.canvasLayer) continue;
        if (w.showChrome) RenderChrome(w, m_chromeHover[w.id]);
        if (w.kind != WindowKind::Ui || !w.build) continue;
        const int rw = int(w.canvasW * w.rtScale), rh = int(w.canvasH * w.rtScale);
        if (w.rt.color.width != rw || w.rt.color.height != rh) w.rt.Create(rw, rh, GL_SRGB8_ALPHA8, false, true);
        glBindFramebuffer(GL_FRAMEBUFFER, w.rt.fbo);
        glClearColor(0, 0, 0, 0);
        glViewport(0, 0, rw, rh);
        glClear(GL_COLOR_BUFFER_BIT);
        // Widgets work in logical px; the renderer supersamples by rtScale.
        m_r->Begin(rw, rh, w.rtScale);
        const PointerState p = m_pointers.count(w.id) ? m_pointers[w.id] : PointerState{};
        m_ui->Begin(w.id, p, dt, time);
        w.build(*m_ui, Rect{0, 0, float(w.canvasW), float(w.canvasH)});
        m_ui->End();
        m_r->End();
        glBindTexture(GL_TEXTURE_2D, w.rt.color.id);
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WindowManager::RenderCanvasTo(Window& w, GLuint fbo, int width, int height, float dt, float time) {
    if (w.kind != WindowKind::Ui || !w.build) return;
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glClearColor(0, 0, 0, 0);
    glViewport(0, 0, width, height);
    glClear(GL_COLOR_BUFFER_BIT);
    m_r->Begin(width, height, float(width) / float(w.canvasW));
    m_ui->Begin(w.id, PointerState{}, dt, time);
    w.build(*m_ui, Rect{0, 0, float(w.canvasW), float(w.canvasH)});
    m_ui->End();
    m_r->End();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void WindowManager::SortForDrawing(const Vec3& head) {
    // Last frame's order first, then any new windows.
    std::vector<Window*> order;
    for (const std::string& id : m_orderIds)
        if (Window* w = Find(id); w && w->appear > 0.01f) order.push_back(w);
    for (auto& w : m_windows)
        if (w->appear > 0.01f && std::find(order.begin(), order.end(), w.get()) == order.end()) order.push_back(w.get());
    std::vector<float> d(order.size());
    for (size_t i = 0; i < order.size(); i++) d[i] = Length(order[i]->pose.position - head);
    // Swaps neighbours only when the later one is farther by more than a margin; a handful of
    // windows, so a bubble sort.
    constexpr float kMargin = 0.02f;
    for (bool swapped = true; swapped;) {
        swapped = false;
        for (size_t i = 0; i + 1 < order.size(); i++) {
            if (d[i + 1] > d[i] + kMargin) {
                std::swap(order[i], order[i + 1]);
                std::swap(d[i], d[i + 1]);
                swapped = true;
            }
        }
    }
    m_order = order;
    m_orderIds.clear();
    for (Window* w : order) m_orderIds.push_back(w->id);
}

void WindowManager::Draw(SceneRenderer& scene) {
    for (Window* w : m_order) {
        const float s = 0.94f + 0.06f * w->appear;
        PanelDraw p;
        p.pose = w->pose;
        p.width = w->widthM * s;
        p.height = w->HeightM() * s;
        p.radiusM = w->curveRadius;
        p.opacity = w->appear;
        if (w->kind == WindowKind::Ui) {
            p.texture = w->rt.color.id;
            p.cornerRadius = 0;  // the canvas draws its own rounded background
            scene.Panel(p);
        } else {
            p.cornerRadius = 0.012f * w->widthM / 1.2f + 0.004f;
            if (w->layerBacked && w->appear > 0.98f) {
                p.holePunch = true;
                scene.Panel(p);
            } else if (w->videoReady) {
                p.source = PanelSource::External;
                p.texture = w->videoTexture;
                std::memcpy(p.crop, w->videoCrop, sizeof(p.crop));
                scene.Panel(p);
            } else {
                // Placeholder while the first frame arrives.
                p.texture = 0;
                p.brightness = 0;
                p.source = PanelSource::Texture2D;
                scene.Panel(p);
            }
        }
        if (w->showChrome && w->chromeRt.fbo && (w->movable || w->resizable)) {
            PanelDraw c;
            c.pose = w->ChromePose();
            c.width = w->widthM * s;
            c.height = w->ChromeHeightM() * s;
            c.texture = w->chromeRt.color.id;
            c.opacity = w->appear;
            scene.Panel(c);
        }
    }
    // What's drawn next (lasers, cursors) lies on the windows and mustn't fight them for depth.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
}

}  // namespace rv
