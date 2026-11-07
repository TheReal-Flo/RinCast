// World-space drawing: panels (flat or curved), laser beams, cursors, and
// texture blits used to feed compositor layers.
#pragma once

#include "gl.h"
#include "math.h"
#include "ui_renderer.h"

namespace rv {

enum class PanelSource { Texture2D, External };

// A foveated VR frame's layout (rc::VrFoveationMetadata), to stretch it back when it's drawn.
struct VideoFoveation {
    float start[2][2] = {};  // per eye, where the full-density square starts: x, y from the top
    float size = 0;          // its side, as a part of the eye's picture
    float density[2] = {};   // of the rest: x, y
};

struct PanelDraw {
    Pose pose;              // centre of the panel; faces +Z
    float width = 1, height = 1;   // meters (arc length when curved)
    float curvature = 0;    // 0 flat, 1 = curved on a cylinder of radius 1 / curvature... (see scene.cpp)
    float radiusM = 0;      // cylinder radius in meters (0 = flat)
    GLuint texture = 0;
    PanelSource source = PanelSource::Texture2D;
    float opacity = 1;
    float cornerRadius = 0; // in meters
    bool holePunch = false; // write (0,0,0,0) to reveal a compositor layer underneath
    bool holeFromAlpha = false;  // ... only as far as the texture covers (a UI canvas's rounded card)
    float brightness = 1;
    float crop[4] = {0, 0, 1, 1};  // external video: visible part of the decoder buffer
};

class SceneRenderer {
   public:
    bool Init();
    void SetViewProjection(const Mat4& viewProj, const Vec3& eyePos) {
        m_viewProj = viewProj;
        m_eye = eyePos;
    }
    void Panel(const PanelDraw& p);
    void Beam(const Vec3& from, const Vec3& to, float width, const Color& start, const Color& end);
    void Dot(const Vec3& position, const Vec3& normal, float radius, const Color& c);
    void ControllerOrb(const Pose& aim, const Color& c);

    // Copies an external (video) texture into the currently bound framebuffer.
    // crop: VideoDecoder::Crop() (null = whole texture).
    void BlitExternal(GLuint texture, int width, int height, const float* crop = nullptr, float u0 = 0, float v0 = 0,
                      float u1 = 1, float v1 = 1, const VideoFoveation* foveation = nullptr);
    bool UpscalerReady() const { return m_upscalerReady; }
    // Keeps a width x height video frame as a normal texture, gamma-encoded, then draws it to dstFbo:
    void KeepVideo(GLuint texture, const float* crop, int width, int height, const VideoFoveation* foveation = nullptr);
    void DrawKept(GLuint dstFbo, int dstW, int dstH);
    void DrawKeptUpscaled(GLuint dstFbo, int dstW, int dstH);  // through Snapdragon Game Super Resolution
    // With its contents moved along the motion field, for frame generation. scaleX, scaleY: a
    // full field value, as a part of the picture. fieldU, fieldV: picture to field coordinates.
    void DrawKeptMoved(GLuint dstFbo, int dstW, int dstH, float scaleX, float scaleY, float fieldU, float fieldV);
    void UploadMotion(const int8_t* grid, int width, int height);
    void Blit2D(GLuint texture, int width, int height, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1);

   private:
    Program m_panel2D, m_panelExt, m_beam, m_dot, m_blitExt, m_blit2D, m_blitExtGamma, m_gsr, m_kept, m_keptMoved;
    GLuint m_motionTexture = 0;
    int m_motionWidth = 0, m_motionHeight = 0;
    bool m_upscalerReady = false;
    GLuint m_gsrTexture = 0, m_gsrFbo = 0;
    int m_gsrWidth = 0, m_gsrHeight = 0;
    GLuint m_gridVao = 0, m_gridVbo = 0;
    int m_gridSegments = 48;
    Mat4 m_viewProj;
    Vec3 m_eye;
};

}  // namespace rv
