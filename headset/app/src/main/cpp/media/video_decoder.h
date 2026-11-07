// Hardware video decoding into a GL_TEXTURE_EXTERNAL_OES texture, without copies:
//   Android: MediaCodec -> AImageReader -> AHardwareBuffer -> EGLImage (video_decoder.cpp)
//   Linux (Steam Frame): V4L2 stateful decoder -> dmabuf -> EGLImage (video_decoder_v4l2.cpp)
// What both share is in video_decoder_common.cpp.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef __ANDROID__
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#endif

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

// What the headset can decode, told to the PC so it picks a codec and size that work.
struct DecoderCaps {
    bool h264 = true, hevc = true, hevc10 = false, av1 = false, av110 = false;
    int maxWidth = 4096, maxHeight = 4096;
};

struct DecodedFrameInfo {
    uint32_t frameIndex = 0;
    std::vector<uint8_t> metadata;   // VrFrameMetadata / DesktopFrameMetadata
    int64_t receivedNs = 0;          // all fragments arrived
    int64_t decodedNs = 0;           // decoder output
};

class VideoDecoder {
   public:
    ~VideoDecoder();
    static DecoderCaps Capabilities();

    // fps: the stream's frame rate, which the decoder books its share of the hardware for.
    bool Init(rc::VideoCodec codec, int width, int height, float fps);
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
#ifdef __ANDROID__
    bool HasFrame() const { return m_image != nullptr; }
#else
    bool HasFrame() const { return m_shown >= 0; }
#endif
    int Width() const { return m_width; }
    int Height() const { return m_height; }
    rc::VideoCodec Codec() const { return m_codec; }

    // Statistics (thread-safe).
    double AverageDecodeMs();
    bool NeedsKeyframe() { return m_needKeyframe.exchange(false); }
    bool Failed() const { return m_failed; }
    // For explaining a missing picture: did the decoder start, and has it produced anything.
    bool Started() const { return m_configured; }
    uint32_t DecodedFrames() const { return m_decoded; }

   private:
    bool Configure(const std::vector<uint8_t>& keyframe);
    void InputLoop();
    void OutputLoop();
    static const char* CodecName(rc::VideoCodec c);

    rc::VideoCodec m_codec = rc::VideoCodec::HEVC;
    int m_width = 0, m_height = 0;
    float m_fps = 90;
#ifdef __ANDROID__
    void ReleaseImage(AImage* image, EGLImageKHR egl);
    AMediaCodec* m_codecHandle = nullptr;
    AImageReader* m_reader = nullptr;
    ANativeWindow* m_window = nullptr;
#else
    // V4L2: OUTPUT buffers carry the bitstream in, CAPTURE buffers the pictures out. Each picture
    // buffer is exported as a dmabuf and imported into EGL once (per-frame imports crash the
    // Frame's kernel), then handed between driver, ready and shown.
    struct Plane {
        void* data = nullptr;
        size_t length = 0;
    };
    struct Picture {
        int fd = -1;  // dmabuf
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        uint32_t frameIndex = 0;
    };
    bool SetUpCapture();
    void QueuePicture(int index);
    int m_device = -1;
    std::vector<Plane> m_bitstream;  // OUTPUT buffers, mmap'd
    std::vector<bool> m_bitstreamFree;
    std::vector<Picture> m_pictures;  // CAPTURE buffers
    uint32_t m_captureFormat = 0, m_captureWidth = 0, m_captureHeight = 0, m_captureStride = 0;
    uint32_t m_captureUvOffset = 0;
    std::atomic<bool> m_captureReady{false};
    std::mutex m_pictureMutex;
    std::deque<int> m_ready;  // decoded, not latched yet (newest last)
    int m_shown = -1;         // bound to the texture now
    int m_retiredPicture = -1;  // shown the frame before: the GPU may still read it
#endif
    std::atomic<bool> m_configured{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_needKeyframe{false};
    std::atomic<uint32_t> m_decoded{0};
    void Warn(const char* what);  // at most every 5 s per reason

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
#ifdef __ANDROID__
    AImage* m_image = nullptr;
    EGLImageKHR m_eglImage = EGL_NO_IMAGE_KHR;
    std::deque<std::pair<AImage*, EGLImageKHR>> m_retired;
#endif

    std::mutex m_statsMutex;
    double m_decodeMsSum = 0;
    int m_decodeCount = 0;
    std::map<int64_t, int64_t> m_inputTimes;  // pts -> queued time
};

}  // namespace rv
