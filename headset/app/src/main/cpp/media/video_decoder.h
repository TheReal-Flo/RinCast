// Hardware video decoding: MediaCodec -> AImageReader -> AHardwareBuffer ->
// EGLImage -> GL_TEXTURE_EXTERNAL_OES (zero copy).
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "../gfx/gl.h"
#include "rc/protocol.h"

namespace rv {

struct DecodedFrameInfo {
    uint32_t frameIndex = 0;
    std::vector<uint8_t> metadata;   // VrFrameMetadata / DesktopFrameMetadata
    int64_t receivedNs = 0;          // all fragments arrived
    int64_t decodedNs = 0;           // decoder output
};

class VideoDecoder {
   public:
    ~VideoDecoder();

    bool Init(rc::VideoCodec codec, int width, int height);
    void Shutdown();

    // Network thread. Frames before the first keyframe are dropped. `recovery`: the PC
    // built this frame only from frames we decoded (kVideoRecovery), so it can end a
    // wait after a loss just like a keyframe.
    void Submit(uint32_t frameIndex, bool keyframe, bool recovery, std::vector<uint8_t>&& bitstream,
                std::vector<uint8_t>&& metadata, int64_t receivedNs);
    // Newest frame of the unbroken chain handed to the decoder (reported to the PC).
    uint32_t LastGoodFrame();

    // GL thread: latches the newest decoded image into the external texture.
    // Returns true when a new frame was latched; `info` describes it.
    bool Latch(DecodedFrameInfo& info);
    GLuint Texture() const { return m_texture; }
    // Visible part of the texture: {offsetU, offsetV, scaleU, scaleV} (texture space, row 0 at v = 0).
    const float* Crop() const { return m_crop; }
    bool HasFrame() const { return m_image != nullptr; }
    int Width() const { return m_width; }
    int Height() const { return m_height; }
    rc::VideoCodec Codec() const { return m_codec; }

    // Statistics (thread-safe).
    double AverageDecodeMs();
    bool NeedsKeyframe() { return m_needKeyframe.exchange(false); }
    bool Failed() const { return m_failed; }

   private:
    bool Configure(const std::vector<uint8_t>& keyframe);
    void InputLoop();
    void OutputLoop();
    void ReleaseImage(AImage* image, EGLImageKHR egl);

    rc::VideoCodec m_codec = rc::VideoCodec::HEVC;
    int m_width = 0, m_height = 0;
    AMediaCodec* m_codecHandle = nullptr;
    AImageReader* m_reader = nullptr;
    ANativeWindow* m_window = nullptr;
    std::atomic<bool> m_configured{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_needKeyframe{false};

    struct Pending {
        uint32_t frameIndex;
        bool keyframe;
        std::vector<uint8_t> data;
        int64_t receivedNs;
    };
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<Pending> m_queue;
    std::map<int64_t, DecodedFrameInfo> m_infos;  // by pts (us) == frameIndex
    std::thread m_inputThread;
    std::thread m_outputThread;
    bool m_hadKeyframe = false;     // the decoder has been started from a keyframe
    bool m_seenKeyframe = false;    // the chain since then is unbroken
    uint32_t m_lastFrameIndex = 0;  // last frame accepted into the chain
    std::atomic<uint32_t> m_lastQueued{0};  // last frame actually given to MediaCodec

    // GL side
    GLuint m_texture = 0;
    float m_crop[4] = {0, 0, 1, 1};
    AImage* m_image = nullptr;
    EGLImageKHR m_eglImage = EGL_NO_IMAGE_KHR;
    std::deque<std::pair<AImage*, EGLImageKHR>> m_retired;

    std::mutex m_statsMutex;
    double m_decodeMsSum = 0;
    int m_decodeCount = 0;
    std::map<int64_t, int64_t> m_inputTimes;  // pts -> queued time
};

}  // namespace rv
