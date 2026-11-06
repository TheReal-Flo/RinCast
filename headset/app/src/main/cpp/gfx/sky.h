// Environments: procedural skies baked once into a cube map at runtime (no
// texture assets), and the hyperspace streaks drawn over them while SteamVR
// launches.
#pragma once

#include <string>
#include <vector>

#include "gl.h"
#include "math.h"

namespace rv {

class Assets;

enum class Environment {
    SpaceViolet,
    SpaceBlue,
    SpaceCyan,
    SpaceGreen,
    SpacePink,
    SpaceRed,
    DayClouds,
    Void,
    Passthrough,
};

struct EnvironmentInfo {
    Environment id;
    const char* key;
    const char* name;
};
const std::vector<EnvironmentInfo>& Environments();
Environment EnvironmentFromKey(const std::string& key);

class SkyRenderer {
   public:
    bool Init(Assets& assets);

    // Once per frame, before any eye is bound: advances the crossfade and runs a
    // few milliseconds of baking if `env` isn't ready yet. Changes the framebuffer
    // binding and viewport.
    void Prepare(Environment env, float time);

    // view: head rotation only (translation is irrelevant for an infinite sky).
    // warp: 0 = no streaks, 1 = full hyperspace over the sky; warpPhase is the
    // integrated travel distance, and its rate of change sets the streak length.
    void Draw(const Mat4& projection, const Quat& headRotation, Environment env, float time, float brightness,
              float warp, float warpPhase, Quat tunnelOrientation);

   private:
    // A cube-map pool slot. Two at most: the sky on show and the one being baked.
    struct Slot {
        GLuint texture = 0;
        Environment env = Environment::Passthrough;
        bool ready = false;
    };
    // What's on show. slot < 0 means the analytic gradient for env (the void, or
    // the stand-in while env bakes), and Passthrough means nothing is drawn.
    struct Layer {
        Environment env = Environment::Passthrough;
        int slot = -1;
    };

    void Select(Environment env);
    void Show(Layer next);
    int FindReadySlot(Environment env) const;
    void StartBake(Environment env, int slot);
    void BakeSlice();
    void BakeTiles(int count);
    void BakeStars();
    void FinishBake();
    void CollectTimerResult();
    void DrawLayer(const Layer& layer, const float ray[4], const float toWorld[9], float brightness, float alpha);
    void DrawWarp(const Mat4& projection, const Quat& headRotation, const Quat& tunnelOrientation, float warp,
                  float warpPhase, float time);

    Program m_sky;
    Program m_gradient;
    Program m_bakeSpace;
    Program m_bakeClouds;
    Program m_bakeStars;
    Program m_streaks;
    Program m_ring;

    Slot m_slots[2];
    GLuint m_bakeFbo = 0;
    Environment m_target = Environment::Passthrough;
    bool m_hasTarget = false;
    Layer m_from, m_to;
    float m_fade = 1;
    float m_lastPrepareTime = 0;
    float m_drift = 0;

    struct Bake {
        bool active = false;
        int slot = 0;
        int face = 0;
        int tile = 0;        // next tile of the face
        bool stars = false;  // this face's tiles are done, its stars come next
        double startSeconds = 0;
        double gpuMs = 0;
        int frames = 0;
    } m_bake;

    // GPU timing, when the driver has it: keeps a bake slice within budget.
    GLuint m_timer = 0;
    bool m_timerPending = false;
    int m_timerTiles = 0;
    int m_tilesPerFrame = 4;

    float m_warpLastPhase = 0;
    float m_warpLastTime = 0;
    float m_warpRate = 0;
};

}  // namespace rv
