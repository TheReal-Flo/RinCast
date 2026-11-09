// Floating, movable, resizable windows in 3D.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../gfx/gl.h"
#include "../gfx/scene.h"
#include "ui.h"

namespace rv {

enum class WindowKind { Ui, Video };
enum class HitZone { None, Content, Handle, ResizeLeft, ResizeRight, Close, Tilt };

struct Window {
    std::string id;
    std::string title;
    WindowKind kind = WindowKind::Ui;
    bool visible = false;
    float baseWidthM = 0;  // before the interface size setting
    float appear = 0;           // 0..1 animated visibility
    Pose pose;                  // centre of the content area
    float widthM = 1.0f;        // content width in meters
    float aspect = 0.75f;       // content height / width
    float minWidthM = 0.35f, maxWidthM = 5.f;
    float curveRadius = 0;      // meters, 0 = flat
    bool movable = true;
    bool resizable = true;
    bool showChrome = true;     // handle bar under the window
    bool layerBacked = false;   // content shown by a compositor layer (hole punched)
    bool canvasLayer = false;   // UI drawn by the app into a compositor layer of its own, not the eye images
    bool removed = false;       // Remove() was called; freed at the next Update()
    // A window that does its own pointing (the keyboard): it gets both hands every frame, so one
    // can hold Shift while the other types, and says what each part of its canvas is. None lets
    // the ray through; Handle, ResizeRight and Tilt move, size and turn it like its chrome would.
    bool twoHands = false;
    PointerState hands[2];
    std::function<HitZone(Vec2 canvasPx)> zoneAt;
    float roll = 0;             // radians, turned around the way it faces (the keyboard's tilt handle)

    // UI content (kind == Ui)
    int canvasW = 900, canvasH = 680;
    float rtScale = 1.6f;
    std::function<void(Ui&, const Rect&)> build;
    RenderTarget rt;
    RenderTarget chromeRt;
    bool needsRedraw = true;
    uint64_t lastHash = 0;

    // Video content (kind == Video)
    GLuint videoTexture = 0;
    float videoCrop[4] = {0, 0, 1, 1};  // visible part of the decoder buffer
    bool videoReady = false;

    float HeightM() const { return widthM * aspect; }
    float ChromeGapM() const { return std::max(0.022f, widthM * 0.025f); }
    float ChromeHeightM() const { return std::max(0.045f, widthM * 0.05f); }
    Pose ChromePose() const;
};

struct WindowHit {
    Window* window = nullptr;
    HitZone zone = HitZone::None;
    float distance = 1e9f;
    Vec2 uv;          // content uv (0..1, top-left origin)
    Vec3 point;       // world hit
    Vec3 normal;
};

class WindowManager {
   public:
    bool Init(UiRenderer* ui, SceneRenderer* scene);
    Window* Create(const std::string& id, WindowKind kind);
    Window* Find(const std::string& id);
    void Remove(const std::string& id);
    std::vector<std::unique_ptr<Window>>& All() { return m_windows; }

    // Places a window in front of the head at `distance`, offset in head-yaw space.
    static Pose PlaceInFront(const Pose& head, float distance, float yawOffsetDeg = 0, float heightOffset = 0,
                             float pitchDeg = 0);
    static Quat FaceTowards(const Vec3& position, const Vec3& target);

    WindowHit Raycast(const Vec3& origin, const Vec3& dir);

    struct HandRay {
        bool valid = false;
        Vec3 origin, dir;
        bool triggerDown = false;
        bool triggerPressed = false;
        bool triggerReleased = false;
        float scroll = 0;        // thumbstick y (for UI scrolling)
        float push = 0;          // thumbstick y while dragging (distance)
    };
    // Handles move/resize and routes pointers to UI windows.
    // Returns per hand the hit (for laser drawing / video pointer).
    void Update(const HandRay hands[2], const Pose& head, float dt, float time, WindowHit hits[2]);
    // Redraws dirty UI canvases.
    void RenderCanvases(float dt, float time);
    // Draws a UI window's content into another target (a compositor layer's image).
    void RenderCanvasTo(Window& w, GLuint fbo, int width, int height, float dt, float time);
    // Back to front from the head, once per frame, for both eyes and the compositor layers alike.
    // A window only moves past another when it's clearly nearer, so two at about the same
    // distance don't swap from frame to frame.
    void SortForDrawing(const Vec3& head);
    const std::vector<Window*>& DrawOrder() const { return m_order; }
    // Overlaps are settled per pixel by depth, so windows that cross look right too.
    void Draw(SceneRenderer& scene);
    bool Dragging() const { return m_drag.window != nullptr; }
    Ui& UiContext() { return *m_ui; }
    bool ConsumeHaptic() {
        const bool h = m_hapticPending;
        m_hapticPending = false;
        return h;
    }
    int HapticHand() const { return m_hapticHand; }

   private:
    void RenderChrome(Window& w, HitZone hover);
    void PurgeRemoved();

    UiRenderer* m_r = nullptr;
    SceneRenderer* m_scene = nullptr;
    std::unique_ptr<Ui> m_ui;
    std::vector<std::unique_ptr<Window>> m_windows;
    std::vector<std::string> m_orderIds;  // last frame's order
    std::vector<Window*> m_order;
    std::unordered_map<std::string, PointerState> m_pointers;
    std::unordered_map<std::string, HitZone> m_chromeHover;

    struct Drag {
        Window* window = nullptr;
        int hand = -1;
        HitZone zone = HitZone::None;
        float distance = 1;
        Vec3 offsetLocal;     // window centre relative to the grab point, in window space
        float startWidth = 1;
        float startSpan = 1;
        float angle0 = 0, roll0 = 0;  // tilting
    } m_drag;
    bool m_hapticPending = false;
    int m_hapticHand = 1;
    int m_pressHand = -1;  // the hand whose press on a UI window is still held (it owns the pointer)
};

}  // namespace rv
