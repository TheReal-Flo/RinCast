#include "video_decoder.h"

#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <media/NdkMediaFormat.h>

#include <cstring>
#include <string>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "rc/platform.h"

namespace rv {

namespace {

PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pGetNativeClientBuffer = nullptr;
PFNEGLCREATEIMAGEKHRPROC pCreateImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture = nullptr;

void LoadEglProcs() {
    if (pCreateImage) return;
    pGetNativeClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
}

const char* Mime(rc::VideoCodec c) {
    switch (c) {
        case rc::VideoCodec::H264: return "video/avc";
        case rc::VideoCodec::HEVC: return "video/hevc";
        case rc::VideoCodec::AV1: return "video/av01";
    }
    return "video/hevc";
}

// Splits an Annex-B stream into NAL units (start code included).
std::vector<std::pair<size_t, size_t>> SplitNals(const std::vector<uint8_t>& d) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < d.size(); i++) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
            starts.push_back(i);
            i += 2;
        }
    }
    std::vector<std::pair<size_t, size_t>> nals;
    for (size_t k = 0; k < starts.size(); k++)
        nals.push_back({starts[k], (k + 1 < starts.size() ? starts[k + 1] : d.size()) - starts[k]});
    return nals;
}

int NalType(const std::vector<uint8_t>& d, size_t start, rc::VideoCodec codec) {
    size_t p = start;
    while (p < d.size() && d[p] == 0) p++;
    p++;  // the 0x01
    if (p >= d.size()) return -1;
    return codec == rc::VideoCodec::H264 ? (d[p] & 0x1F) : ((d[p] >> 1) & 0x3F);
}

}  // namespace

VideoDecoder::~VideoDecoder() { Shutdown(); }

DecoderCaps VideoDecoder::Capabilities() {
    // Quest, PICO and Android XR headsets decode H.264 and HEVC (10-bit too) up to 8K wide in
    // hardware; AV1 only where a decoder for it exists.
    DecoderCaps caps;
    caps.hevc10 = true;
    // Only a hardware AV1 decoder counts. Android's own software one (c2.android.*, OMX.google.*)
    // exists on headsets without AV1 hardware (Quest 1 and 2) and is far too slow for a stream.
    if (AMediaCodec* av1 = AMediaCodec_createDecoderByType("video/av01")) {
        char* name = nullptr;
        AMediaCodec_getName(av1, &name);
        const bool software = name && (!strncmp(name, "c2.android.", 11) || !strncmp(name, "OMX.google.", 11));
        caps.av1 = caps.av110 = !software;
        RC_LOGI("AV1 decoder: %s%s", name ? name : "?", software ? " (software, not used)" : "");
        if (name) AMediaCodec_releaseName(av1, name);
        AMediaCodec_delete(av1);
    }
    // The Quest 1's Snapdragon 835 decodes up to 4K; later chips up to 8K.
    char device[PROP_VALUE_MAX] = {};
    __system_property_get("ro.product.device", device);
    const bool quest1 = !strcmp(device, "monterey");
    caps.maxWidth = quest1 ? 4096 : 8192;
    caps.maxHeight = quest1 ? 2160 : 4096;
    return caps;
}

bool VideoDecoder::Init(rc::VideoCodec codec, int width, int height, float fps) {
    LoadEglProcs();
    m_codec = codec;
    m_width = width;
    m_height = height;
    m_fps = fps > 1 ? fps : 90;
    const media_status_t st = AImageReader_newWithUsage(width, height, AIMAGE_FORMAT_PRIVATE,
                                                        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 5, &m_reader);
    if (st != AMEDIA_OK) {
        RC_LOGE("Decoder %s %dx%d: AImageReader_newWithUsage failed (%d)", Mime(codec), width, height, st);
        m_failed = true;
        return false;
    }
    AImageReader_getWindow(m_reader, &m_window);
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    m_running = true;
    m_inputThread = std::thread([this] { InputLoop(); });
    return true;
}

void VideoDecoder::Shutdown() {
    if (m_running.exchange(false)) {
        m_cv.notify_all();
        if (m_inputThread.joinable()) m_inputThread.join();
        if (m_outputThread.joinable()) m_outputThread.join();
    }
    if (m_codecHandle) {
        AMediaCodec_stop(m_codecHandle);
        AMediaCodec_delete(m_codecHandle);
        m_codecHandle = nullptr;
    }
    ReleaseImage(m_image, m_eglImage);
    m_image = nullptr;
    m_eglImage = EGL_NO_IMAGE_KHR;
    for (auto& [img, egl] : m_retired) ReleaseImage(img, egl);
    m_retired.clear();
    if (m_reader) {
        AImageReader_delete(m_reader);
        m_reader = nullptr;
    }
    if (m_texture) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
}

bool VideoDecoder::Configure(const std::vector<uint8_t>& keyframe) {
    m_codecHandle = AMediaCodec_createDecoderByType(Mime(m_codec));
    if (!m_codecHandle) {
        RC_LOGE("No decoder for %s", Mime(m_codec));
        m_failed = true;
        return false;
    }
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, Mime(m_codec));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, m_width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, m_height);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_WIDTH, m_width);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_HEIGHT, m_height);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, m_width * m_height);
    // Latency over everything else.
    AMediaFormat_setInt32(fmt, "low-latency", 1);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_PRIORITY, 0);
    // Twice the stream's frame rate, so each frame decodes in half a frame's time. Not the
    // maximum (32767): the Quest 2's video driver books a stream asking for that at the most it
    // could ever decode (960 fps for 1080p), its whole budget, and then refuses every other
    // stream ("H/W is overloaded ... MBPS limit exceeded"): a second monitor, or the game.
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, int32_t(m_fps + 0.5f));
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_OPERATING_RATE, int32_t(m_fps * 2 + 0.5f));
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(fmt, "vendor.qti-ext-dec-picture-order.enable", 1);

    // Parameter sets as codec-specific data (they are also repeated inline).
    if (m_codec != rc::VideoCodec::AV1) {
        std::vector<uint8_t> csd0, csd1;
        for (auto [off, len] : SplitNals(keyframe)) {
            const int t = NalType(keyframe, off, m_codec);
            if (m_codec == rc::VideoCodec::H264) {
                if (t == 7) csd0.insert(csd0.end(), keyframe.begin() + off, keyframe.begin() + off + len);
                if (t == 8) csd1.insert(csd1.end(), keyframe.begin() + off, keyframe.begin() + off + len);
            } else if (t == 32 || t == 33 || t == 34) {
                csd0.insert(csd0.end(), keyframe.begin() + off, keyframe.begin() + off + len);
            }
        }
        if (!csd0.empty()) AMediaFormat_setBuffer(fmt, "csd-0", csd0.data(), csd0.size());
        if (!csd1.empty()) AMediaFormat_setBuffer(fmt, "csd-1", csd1.data(), csd1.size());
    }
    media_status_t st = AMediaCodec_configure(m_codecHandle, fmt, m_window, nullptr, 0);
    AMediaFormat_delete(fmt);
    const media_status_t started = st == AMEDIA_OK ? AMediaCodec_start(m_codecHandle) : st;
    if (st != AMEDIA_OK || started != AMEDIA_OK) {
        // A headset has only so much decoding hardware; another stream still being decoded
        // can be what's in the way (an "insufficient resource" error, -10000 and below).
        RC_LOGE("Decoder %s %dx%d: %s failed (%d)", Mime(m_codec), m_width, m_height,
                st != AMEDIA_OK ? "configure" : "start", st != AMEDIA_OK ? st : started);
        AMediaCodec_delete(m_codecHandle);
        m_codecHandle = nullptr;
        m_failed = true;
        return false;
    }
    m_configured = true;
    m_failed = false;  // a later keyframe got it going after all
    m_outputThread = std::thread([this] { OutputLoop(); });
    RC_LOGI("Decoder %s %dx%d started", Mime(m_codec), m_width, m_height);
    return true;
}

void VideoDecoder::InputLoop() {
    setpriority(PRIO_PROCESS, gettid(), -10);  // best effort
    while (m_running) {
        Pending p;
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(20), [&] { return !m_running || !m_queue.empty(); });
            if (!m_running) break;
            if (m_queue.empty()) continue;
            p = std::move(m_queue.front());
            m_queue.pop_front();
        }
        if (!m_configured) {
            if (!p.keyframe || !Configure(p.data)) continue;
        }
        ssize_t index = -1;
        for (int attempt = 0; attempt < 50 && m_running && index < 0; attempt++)
            index = AMediaCodec_dequeueInputBuffer(m_codecHandle, 2000);
        if (index < 0) {
            Warn("no free input buffer for 100 ms, the decoder is falling behind");
            // This frame never reaches the decoder, so the chain breaks here.
            std::lock_guard lock(m_mutex);
            m_queue.clear();
            m_seenKeyframe = false;
            m_lastFrameIndex = m_lastQueued;
            m_needKeyframe = true;
            continue;
        }
        size_t capacity = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(m_codecHandle, size_t(index), &capacity);
        const size_t n = std::min(capacity, p.data.size());
        std::memcpy(buf, p.data.data(), n);
        {
            std::lock_guard lock(m_statsMutex);
            m_inputTimes[int64_t(p.frameIndex)] = rc::NowNs();
            while (m_inputTimes.size() > 64) m_inputTimes.erase(m_inputTimes.begin());
        }
        if (AMediaCodec_queueInputBuffer(m_codecHandle, size_t(index), 0, n, uint64_t(p.frameIndex), 0) != AMEDIA_OK)
            Warn("queueInputBuffer failed");
        m_lastQueued = p.frameIndex;
    }
}

void VideoDecoder::OutputLoop() {
    setpriority(PRIO_PROCESS, gettid(), -10);  // best effort
    while (m_running) {
        AMediaCodecBufferInfo info;
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(m_codecHandle, &info, 10000);
        if (index >= 0) {
            const int64_t now = rc::NowNs();
            {
                std::lock_guard lock(m_statsMutex);
                auto it = m_inputTimes.find(info.presentationTimeUs);
                if (it != m_inputTimes.end()) {
                    m_decodeMsSum += rc::NsToMs(now - it->second);
                    m_decodeCount++;
                    m_inputTimes.erase(it);
                }
            }
            {
                std::lock_guard lock(m_mutex);
                auto it = m_infos.find(info.presentationTimeUs);
                if (it != m_infos.end()) it->second.decodedNs = now;
            }
            AMediaCodec_releaseOutputBuffer(m_codecHandle, size_t(index), info.size != 0);
            if (!m_decoded++) RC_LOGI("Decoder %s %dx%d: first frame decoded", Mime(m_codec), m_width, m_height);
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* f = AMediaCodec_getOutputFormat(m_codecHandle);
            RC_LOGI("Decoder output format: %s", AMediaFormat_toString(f));
            AMediaFormat_delete(f);
        } else if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER && index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            Warn("dequeueOutputBuffer returned an error");
        }
    }
}

void VideoDecoder::ReleaseImage(AImage* image, EGLImageKHR egl) {
    if (egl != EGL_NO_IMAGE_KHR && pDestroyImage) pDestroyImage(eglGetCurrentDisplay(), egl);
    if (image) AImage_delete(image);
}

bool VideoDecoder::Latch(DecodedFrameInfo& out) {
    if (!m_reader) return false;
    AImage* image = nullptr;
    if (AImageReader_acquireLatestImage(m_reader, &image) != AMEDIA_OK || !image) return false;
    AHardwareBuffer* hb = nullptr;
    AImage_getHardwareBuffer(image, &hb);
    if (!hb || !pCreateImage) {
        Warn("a decoded picture has no hardware buffer to show");
        AImage_delete(image);
        return false;
    }
    const EGLint attribs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR egl = pCreateImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                   pGetNativeClientBuffer(hb), attribs);
    if (egl == EGL_NO_IMAGE_KHR) {
        Warn("couldn't turn a decoded picture into a texture");
        AImage_delete(image);
        return false;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, egl);

    // Hardware decoders pad their buffers (e.g. 1080 -> 1088 rows, or the width to the
    // GPU's alignment). The texture covers the whole buffer, so sample only the
    // visible crop rectangle. Edges that border padding are pulled in by half a
    // texel so bilinear filtering never blends in the padding.
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(hb, &desc);
    AImageCropRect crop{0, 0, int32_t(desc.width), int32_t(desc.height)};
    if (AImage_getCropRect(image, &crop) != AMEDIA_OK || crop.right <= crop.left || crop.bottom <= crop.top)
        crop = {0, 0, int32_t(desc.width), int32_t(desc.height)};
    if (desc.width && desc.height) {
        const float W = float(desc.width), H = float(desc.height);
        const float x0 = (crop.left + (crop.left > 0 ? 0.5f : 0.f)) / W;
        const float y0 = (crop.top + (crop.top > 0 ? 0.5f : 0.f)) / H;
        const float x1 = (crop.right - (uint32_t(crop.right) < desc.width ? 0.5f : 0.f)) / W;
        const float y1 = (crop.bottom - (uint32_t(crop.bottom) < desc.height ? 0.5f : 0.f)) / H;
        const float next[4] = {x0, y0, x1 - x0, y1 - y0};
        if (std::memcmp(next, m_crop, sizeof(next)) != 0) {
            RC_LOGI("Decoder output %ux%u, visible %d,%d-%d,%d", desc.width, desc.height, crop.left, crop.top,
                    crop.right, crop.bottom);
            std::memcpy(m_crop, next, sizeof(next));
        }
    }

    // Keep the previous images alive for a couple of frames (GPU may still read them).
    if (m_image) m_retired.push_back({m_image, m_eglImage});
    while (m_retired.size() > 2) {
        ReleaseImage(m_retired.front().first, m_retired.front().second);
        m_retired.pop_front();
    }
    m_image = image;
    m_eglImage = egl;

    int64_t ts = 0;
    AImage_getTimestamp(image, &ts);
    const int64_t pts = ts / 1000;
    std::lock_guard lock(m_mutex);
    auto it = m_infos.find(pts);
    if (it != m_infos.end()) {
        out = it->second;
        m_infos.erase(m_infos.begin(), std::next(it));
    } else {
        out = {};
        out.frameIndex = uint32_t(pts);
    }
    return true;
}

}  // namespace rv
