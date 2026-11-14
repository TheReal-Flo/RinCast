// RinCast's keyboard: a port of BetterFrame Keyboard (MIT, github.com/hardwaresteam/BetterFrameKeyboard),
// the Steam Frame keyboard, so it looks and behaves the same: its Minimal, Standard and Full layouts,
// its top bar, themes, key preview, swipe typing, word suggestions, emoji page and 47 languages.
//
// It draws into its window's canvas like BetterFrame draws its overlay picture, in the same pixels
// (1600 wide, 2048 for Full), and takes both hands at once, like BetterFrame takes both lasers.
// What it types goes to the PC as key presses and text.
//
// Not ported: voice typing, Chinese (its input methods need their dictionaries), Japanese kanji
// conversion (kana are typed as they are), Steam's emoticons and the Steam Controller.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "gfx/gl.h"
#include "gfx/math.h"
#include "gfx/ui_renderer.h"
#include "kb_config.h"
#include "kb_layout.h"
#include "kb_sound.h"
#include "kb_swipe.h"

namespace rv {
class Assets;
struct Window;
struct PointerState;
enum class HitZone;
}  // namespace rv

namespace rv::kb {

class Keyboard {
   public:
    struct Output {
        std::function<void(uint16_t vk, bool down)> key;
        std::function<void(const std::string&)> text;
        std::function<void()> hide;
        std::function<void()> recenter;
        std::function<void()> saved;  // Config changed
    };

    bool Init(const Assets& assets, Output out);
    Config& Conf() { return m_conf; }
    // After the settings changed it from outside (the settings page).
    void ConfChanged();
    // Every frame its window is drawn: takes the hands, sizes the window, draws.
    void Build(UiRenderer& R, Window& w, float time);
    // Lays it out and sizes its window, before the window's canvas is made for the frame.
    void Size(Window& w);
    // What a canvas point is: a key (Content), a handle, or nothing (the ray goes through).
    HitZone ZoneAt(Vec2 px) const;
    void SetVisible(bool visible);
    // Where it opens: BetterFrame's spot, in front, below the eyes, leaning back like a laptop.
    Pose PlaceInFront(const Pose& head) const;
    static const std::vector<std::string>& ThemeNames();
    State& PreviewState() { return m_st; }  // (tools/ui-preview)
    // Opens its language ("lang") or key layout ("fmt") picker, as BetterFrame's settings do.
    void OpenPicker(const std::string& page);
    // Its first size, back in front of you and level (the settings' reset).
    void ResetPlace();

   private:
    struct Item {
        Key k;
        float x0, y0, x1, y1;
        float W() const { return x1 - x0; }
        float H() const { return y1 - y0; }
    };
    struct Swipe {
        int start;
        std::vector<Pt> pts, trail;
        std::vector<float> trailT;
        std::vector<int> keys;
        float t;
        bool shift, caps, after, held = false, real = false;
    };
    struct Repeat {
        int slot, item;
        std::string text, fn;
        float next, since = -1;
        bool first = false;
    };
    struct Slot {
        int hover = -1, held = -1;
        float pressedUntil = 0;
        std::unique_ptr<Swipe> swipe;
        int ehold = -1;
        float eholdT = 0;
        bool eholdDone = false;
        int flick = -1, flickDir = 0;
        Pt flickAt;
    };

    // ---- layout and drawing (Renderer.layout / draw_keys)
    void Layout();
    void Draw(UiRenderer& R, float time);
    void DrawKeys(UiRenderer& R);
    void DrawText(UiRenderer& R, const std::string& text, float x, float y, float px, const Color& c, bool left = false);
    float FitFont(UiRenderer& R, const std::string& text, float px, float maxW);
    void DrawIcon(UiRenderer& R, const std::string& kind, float cx, float cy, float s, const Color& c);
    void DrawEmoji(UiRenderer& R, const std::string& e, float cx, float cy, float size);
    void DrawOverlays(UiRenderer& R, float now);
    Color Pal(const char* name, float alpha = -1) const;
    void LoadImages(const Assets& assets);
    void LoadEmoji();

    // ---- input (the main loop's laser handling)
    void Input(const PointerState* hands, float now);
    int ItemAt(float x, float y) const;
    void Pressed(int slot, int h, float now);
    void Release(int slot, float now);
    void Timers(float now);

    // ---- what keys do (_press and friends)
    void Press(const Key& k, bool quiet = false);
    std::string CharFor(const Key& k) const;
    const std::map<std::string, std::string>& LayerMap() const;
    bool Repeatable(const Key& k) const;
    std::string HeldVariant(const Key& k, const std::string& typed) const;
    std::string RepeatText(const Key& k) const;
    bool LockTap(const std::string& name, bool on, float now);
    void ModsUsed();
    bool ShiftHeld() const;
    void Flash(const std::string& msg, float secs = 2.5f);
    void Sound(bool big);

    // ---- sending (type_text, the virtual keyboard's taps)
    void Tap(uint16_t vk, bool withMods, bool withShift);
    void Combo(std::initializer_list<uint16_t> keys);
    void TypeText(const std::string& s);
    void ReplaceTyped(size_t n, const std::string& text);

    // ---- Japanese romaji, Korean and Vietnamese
    std::pair<size_t, std::string> RomajiFeed(const std::string& c);
    std::pair<size_t, std::string> RomajiFlush();
    bool ComposePress(const Key& k);
    std::string Compose() const;

    // ---- swipe and the word bar
    bool SwipeReady(const Key& k) const;
    std::string SwipeLang() const;
    bool RealSwipe(Swipe& sw) const;
    bool HeldStill(const Swipe& sw, int item) const;
    bool DrawSwipe(Swipe& sw) const;
    float KeyW() const;
    std::map<std::string, Pt> SwipeKeys(float* kw) const;
    void FinishSwipe(int slot, float now);
    void PickSuggestion(int i);
    void NoteWord(const Key& k);
    void UpdateWords(float now);
    void FinishFlick(int slot);

    void Save();
    bool m_scaleDirty = false;  // resized by hand: saved once it settles
    void SizeWindow(Window& w);

    Output m_out;
    Config m_conf;
    State m_st;
    const Assets* m_assets = nullptr;
    std::unique_ptr<Decoder> m_decoder;
    ClickPlayer m_clicks;
    std::map<std::string, Data::Rgba> m_pal;
    std::string m_palFor;

    // layout
    std::vector<Item> m_items;
    std::string m_layoutKey;
    float m_W = 1600, m_H = 700, m_top = 0;  // image size; canvas margin above it
    float m_upx = 100, m_uph = 80, m_gap = 10, m_margin = 20, m_widthM = 0.6f;
    float m_barPx = 20, m_pickCap = 20;  // the top bar's text size; the language picker's
    Item m_grab{}, m_resize{}, m_rotate{};
    float m_stripY0 = 0, m_stripY1 = 0, m_gx0 = 0, m_gx1 = 0;
    struct Box {
        float x0, y0, x1, y1;
    } m_panel{};

    // images
    Texture m_icons, m_emoji;
    bool m_emojiLoaded = false;
    std::map<std::string, std::array<float, 4>> m_iconCells, m_emojiCells;
    float m_iconsW = 1, m_iconsH = 1, m_emojiW = 1, m_emojiH = 1, m_iconSize = 192, m_handleBox = 120, m_handleMargin = 38;

    // input
    Slot m_slots[2];
    std::unique_ptr<Repeat> m_repeat;
    struct ShiftLock {
        int slot, item;
        float at;
    };
    std::unique_ptr<ShiftLock> m_shiftLock;
    std::map<std::string, float> m_lockT;
    float m_lastShift = -10, m_lastPress = 0;
    std::string m_prevAct, m_curAct;
    bool m_shiftTyped = false, m_autoSpace = false, m_afterLetter = false;
    std::string m_lastChar, m_msg;
    float m_msgUntil = 0;
    bool m_visible = false;
    float m_now = 0;

    // romaji, Korean, Vietnamese
    std::string m_romajiBuf;
    std::vector<std::string> m_units;  // Korean jamo / Vietnamese letters of the word being composed
    std::string m_composed;            // what's on screen for them

    // word bar
    std::string m_curWord, m_wordMode = "type", m_lastSwipe, m_previewWord;
    std::vector<std::string> m_wordList;
    int m_wordSel = -1;
    bool m_wordsDue = false;
    float m_previewT = 0;
    size_t m_previewN = 0;
};

}  // namespace rv::kb
