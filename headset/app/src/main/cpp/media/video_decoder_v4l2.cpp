// Hardware video decoding on Linux through a V4L2 stateful decoder: on the Steam Frame, Qualcomm's
// iris driver at /dev/video-dec0 (H.264 and 8-bit HEVC). Bitstream goes into mmap'd OUTPUT
// buffers; decoded NV12 pictures come back in CAPTURE buffers, which are exported as dmabufs and
// imported into EGL as external textures, without copies.
//
// The Frame's driver has quirks this follows: the CAPTURE side may only be set up after the
// driver's source-change event (before it, its size is a placeholder and the buffers come out
// too small), and each dmabuf is imported once and kept (importing per frame crashes its kernel).
#include "video_decoder.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>

#include "rc/platform.h"

// DRM_FORMAT_NV12 (drm_fourcc.h): the same fourcc as V4L2's NV12.
constexpr int kDrmFormatNv12 = int(v4l2_fourcc('N', 'V', '1', '2'));

namespace rv {

namespace {

PFNEGLCREATEIMAGEKHRPROC pCreateImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture = nullptr;

void LoadEglProcs() {
    if (pCreateImage) return;
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
}

int Ioctl(int fd, unsigned long request, void* arg) {
    int r;
    do r = ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

uint32_t PixelFormat(rc::VideoCodec c) {
    switch (c) {
        case rc::VideoCodec::H264: return V4L2_PIX_FMT_H264;
        case rc::VideoCodec::HEVC: return V4L2_PIX_FMT_HEVC;
        case rc::VideoCodec::AV1: return v4l2_fourcc('A', 'V', '0', '1');
    }
    return V4L2_PIX_FMT_HEVC;
}

bool HasOutputFormat(int fd, uint32_t fourcc) {
    for (uint32_t i = 0;; i++) {
        v4l2_fmtdesc d{};
        d.index = i;
        d.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (Ioctl(fd, VIDIOC_ENUM_FMT, &d) < 0) return false;
        if (d.pixelformat == fourcc) return true;
    }
}

// The Frame names its decoder /dev/video-dec0. Elsewhere, the first memory-to-memory device that
// takes H.264 or HEVC in.
int OpenDecoder() {
    int fd = open("/dev/video-dec0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) return fd;
    for (int i = 0; i < 64; i++) {
        const std::string path = "/dev/video" + std::to_string(i);
        fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability cap{};
        const uint32_t caps = Ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0
                                  ? (cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps : cap.capabilities)
                                  : 0;
        if ((caps & V4L2_CAP_VIDEO_M2M_MPLANE) &&
            (HasOutputFormat(fd, V4L2_PIX_FMT_HEVC) || HasOutputFormat(fd, V4L2_PIX_FMT_H264)))
            return fd;
        close(fd);
    }
    return -1;
}

// v4l2_buffer::timestamp: timeval in older kernel headers (the SDK's), v4l2_timeval in newer ones.
using BufferTime = decltype(v4l2_buffer::timestamp);

BufferTime Timestamp(uint32_t frameIndex) {
    BufferTime t{};
    t.tv_sec = frameIndex / 1'000'000;
    t.tv_usec = frameIndex % 1'000'000;
    return t;
}

uint32_t FrameIndex(const BufferTime& t) { return uint32_t(t.tv_sec * 1'000'000 + t.tv_usec); }

}  // namespace

VideoDecoder::~VideoDecoder() { Shutdown(); }

DecoderCaps VideoDecoder::Capabilities() {
    DecoderCaps caps;
    caps.h264 = caps.hevc = false;
    const int fd = OpenDecoder();
    if (fd < 0) {
        RC_LOGE("No V4L2 video decoder found");
        return caps;
    }
    caps.h264 = HasOutputFormat(fd, V4L2_PIX_FMT_H264);
    caps.hevc = HasOutputFormat(fd, V4L2_PIX_FMT_HEVC);
    // The Frame's firmware could do AV1 and 10-bit, but its driver offers neither yet.
    for (uint32_t i = 0;; i++) {
        v4l2_frmsizeenum s{};
        s.index = i;
        s.pixel_format = caps.hevc ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
        if (Ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &s) < 0) break;
        if (s.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
            caps.maxWidth = std::max(caps.maxWidth, int(s.discrete.width));
            caps.maxHeight = std::max(caps.maxHeight, int(s.discrete.height));
        } else {
            caps.maxWidth = int(s.stepwise.max_width);
            caps.maxHeight = int(s.stepwise.max_height);
            break;
        }
    }
    close(fd);
    RC_LOGI("V4L2 decoder: H.264 %d, HEVC %d, up to %dx%d", caps.h264, caps.hevc, caps.maxWidth, caps.maxHeight);
    return caps;
}

bool VideoDecoder::Init(rc::VideoCodec codec, int width, int height, float fps) {
    LoadEglProcs();
    m_codec = codec;
    m_width = width;
    m_height = height;
    m_fps = fps > 1 ? fps : 90;
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (!pCreateImage || !pImageTargetTexture) {
        RC_LOGE("Decoder %s: EGL can't import dmabufs (eglCreateImageKHR / glEGLImageTargetTexture2DOES missing)",
                CodecName(codec));
        m_failed = true;
        return false;
    }

    m_device = OpenDecoder();
    if (m_device < 0) {
        RC_LOGE("Decoder %s %dx%d: no V4L2 decoder", CodecName(codec), width, height);
        m_failed = true;
        return false;
    }
    // Bitstream in: one plane per buffer, big enough for a keyframe.
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.pixelformat = PixelFormat(codec);
    fmt.fmt.pix_mp.width = uint32_t(width);
    fmt.fmt.pix_mp.height = uint32_t(height);
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = uint32_t(std::max(width * height, 2 << 20));
    if (Ioctl(m_device, VIDIOC_S_FMT, &fmt) < 0) {
        RC_LOGE("Decoder %s %dx%d: the driver refused the format (%s)", CodecName(codec), width, height, strerror(errno));
        m_failed = true;
        return false;
    }
    v4l2_event_subscription sub{};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    Ioctl(m_device, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = V4L2_EVENT_EOS;
    Ioctl(m_device, VIDIOC_SUBSCRIBE_EVENT, &sub);

    v4l2_requestbuffers req{};
    req.count = 6;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (Ioctl(m_device, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        RC_LOGE("Decoder %s: no bitstream buffers (%s)", CodecName(codec), strerror(errno));
        m_failed = true;
        return false;
    }
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_plane plane{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = &plane;
        if (Ioctl(m_device, VIDIOC_QUERYBUF, &buf) < 0) break;
        void* p = mmap(nullptr, plane.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_device, plane.m.mem_offset);
        if (p == MAP_FAILED) break;
        m_bitstream.push_back({p, plane.length});
    }
    m_bitstreamFree.assign(m_bitstream.size(), true);
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (m_bitstream.empty() || Ioctl(m_device, VIDIOC_STREAMON, &type) < 0) {
        RC_LOGE("Decoder %s: couldn't start the bitstream queue (%s)", CodecName(codec), strerror(errno));
        m_failed = true;
        return false;
    }
    RC_LOGI("Decoder %s %dx%d opened (%zu bitstream buffers)", CodecName(codec), width, height, m_bitstream.size());
    m_running = true;
    m_inputThread = std::thread([this] { InputLoop(); });
    m_outputThread = std::thread([this] { OutputLoop(); });
    return true;
}

void VideoDecoder::Shutdown() {
    if (m_running.exchange(false)) {
        m_cv.notify_all();
        if (m_inputThread.joinable()) m_inputThread.join();
        if (m_outputThread.joinable()) m_outputThread.join();
    }
    if (m_device >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        Ioctl(m_device, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        Ioctl(m_device, VIDIOC_STREAMOFF, &type);
    }
    for (auto& p : m_pictures) {
        if (p.image != EGL_NO_IMAGE_KHR && pDestroyImage) pDestroyImage(eglGetCurrentDisplay(), p.image);
        if (p.fd >= 0) close(p.fd);
    }
    m_pictures.clear();
    for (auto& b : m_bitstream) munmap(b.data, b.length);
    m_bitstream.clear();
    if (m_device >= 0) close(m_device);
    m_device = -1;
    if (m_texture) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
}

// Called by the output thread once the driver knows the stream's size.
bool VideoDecoder::SetUpCapture() {
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (Ioctl(m_device, VIDIOC_G_FMT, &fmt) < 0) return false;
    // Linear NV12: what EGL can import as one dmabuf with two planes.
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    if (Ioctl(m_device, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12) {
        RC_LOGE("Decoder: the driver won't give linear NV12 (%s)", strerror(errno));
        return false;
    }
    m_captureWidth = fmt.fmt.pix_mp.width;
    m_captureHeight = fmt.fmt.pix_mp.height;
    m_captureStride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    // NV12 in one buffer: the chroma rows follow the luma rows, padded to the buffer's height.
    m_captureUvOffset = m_captureStride * m_captureHeight;

    // The visible part (the rest is the decoder's padding).
    v4l2_selection sel{};
    sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    sel.target = V4L2_SEL_TGT_COMPOSE;
    uint32_t vx = 0, vy = 0, vw = uint32_t(m_width), vh = uint32_t(m_height);
    if (Ioctl(m_device, VIDIOC_G_SELECTION, &sel) == 0 && sel.r.width && sel.r.height) {
        vx = uint32_t(sel.r.left);
        vy = uint32_t(sel.r.top);
        vw = sel.r.width;
        vh = sel.r.height;
    }
    const float W = float(m_captureWidth), H = float(m_captureHeight);
    // Edges next to padding are pulled in by half a texel, as on Android.
    const float x0 = (vx + (vx > 0 ? 0.5f : 0.f)) / W, y0 = (vy + (vy > 0 ? 0.5f : 0.f)) / H;
    const float x1 = (vx + vw - (vx + vw < m_captureWidth ? 0.5f : 0.f)) / W;
    const float y1 = (vy + vh - (vy + vh < m_captureHeight ? 0.5f : 0.f)) / H;
    m_crop[0] = x0;
    m_crop[1] = y0;
    m_crop[2] = x1 - x0;
    m_crop[3] = y1 - y0;

    v4l2_control minBuffers{};
    minBuffers.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
    const int needed = Ioctl(m_device, VIDIOC_G_CTRL, &minBuffers) == 0 ? minBuffers.value : 4;
    v4l2_requestbuffers req{};
    req.count = uint32_t(needed + 3);  // + shown, the one before it, and one ready
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (Ioctl(m_device, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        RC_LOGE("Decoder: no picture buffers (%s)", strerror(errno));
        return false;
    }
    {
        std::lock_guard lock(m_pictureMutex);
        m_pictures.assign(req.count, {});
        for (uint32_t i = 0; i < req.count; i++) {
            v4l2_exportbuffer ex{};
            ex.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            ex.index = i;
            ex.plane = 0;
            ex.flags = O_RDONLY | O_CLOEXEC;
            if (Ioctl(m_device, VIDIOC_EXPBUF, &ex) < 0) {
                RC_LOGE("Decoder: couldn't export picture buffer %u as a dmabuf (%s)", i, strerror(errno));
                return false;
            }
            m_pictures[i].fd = ex.fd;
        }
    }
    for (uint32_t i = 0; i < req.count; i++) QueuePicture(int(i));
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (Ioctl(m_device, VIDIOC_STREAMON, &type) < 0) {
        RC_LOGE("Decoder: couldn't start the picture queue (%s)", strerror(errno));
        return false;
    }
    RC_LOGI("Decoder %s: %ux%u NV12 pictures (stride %u, visible %ux%u at %u,%u), %u buffers", CodecName(m_codec),
            m_captureWidth, m_captureHeight, m_captureStride, vw, vh, vx, vy, req.count);
    m_configured = true;
    return true;
}

void VideoDecoder::QueuePicture(int index) {
    v4l2_plane plane{};
    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = uint32_t(index);
    buf.length = 1;
    buf.m.planes = &plane;
    if (Ioctl(m_device, VIDIOC_QBUF, &buf) < 0) Warn("couldn't give a picture buffer back to the driver");
}

bool VideoDecoder::Configure(const std::vector<uint8_t>&) { return true; }  // V4L2 starts in Init

void VideoDecoder::InputLoop() {
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), -10);  // best effort
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
        // A free bitstream buffer: never used yet, or one the driver is done with.
        int index = -1;
        for (int attempt = 0; attempt < 50 && m_running && index < 0; attempt++) {
            for (size_t i = 0; i < m_bitstreamFree.size(); i++)
                if (m_bitstreamFree[i]) {
                    index = int(i);
                    break;
                }
            if (index >= 0) break;
            v4l2_plane plane{};
            v4l2_buffer buf{};
            buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.length = 1;
            buf.m.planes = &plane;
            if (Ioctl(m_device, VIDIOC_DQBUF, &buf) == 0) {
                m_bitstreamFree[buf.index] = true;
                continue;
            }
            pollfd pfd{m_device, POLLOUT, 0};
            poll(&pfd, 1, 2);
        }
        if (index < 0) {
            Warn("no free bitstream buffer for 100 ms, the decoder is falling behind");
            std::lock_guard lock(m_mutex);
            m_queue.clear();
            m_seenKeyframe = false;
            m_lastFrameIndex = m_lastQueued;
            m_needKeyframe = true;
            continue;
        }
        const Plane& dst = m_bitstream[size_t(index)];
        if (p.data.size() > dst.length) {
            Warn("a frame is bigger than a bitstream buffer; it's cut off");
            p.data.resize(dst.length);
        }
        std::memcpy(dst.data, p.data.data(), p.data.size());
        v4l2_plane plane{};
        plane.bytesused = uint32_t(p.data.size());
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = uint32_t(index);
        buf.length = 1;
        buf.m.planes = &plane;
        buf.timestamp = Timestamp(p.frameIndex);
        {
            std::lock_guard lock(m_statsMutex);
            m_inputTimes[int64_t(p.frameIndex)] = rc::NowNs();
            while (m_inputTimes.size() > 64) m_inputTimes.erase(m_inputTimes.begin());
        }
        if (Ioctl(m_device, VIDIOC_QBUF, &buf) < 0) {
            Warn("the driver refused a frame");
            continue;
        }
        m_bitstreamFree[size_t(index)] = false;
        m_lastQueued = p.frameIndex;
    }
}

void VideoDecoder::OutputLoop() {
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), -10);  // best effort
    while (m_running) {
        pollfd pfd{m_device, short(POLLIN | POLLPRI), 0};
        if (poll(&pfd, 1, 10) <= 0) continue;
        if (pfd.revents & POLLPRI) {
            v4l2_event ev{};
            while (Ioctl(m_device, VIDIOC_DQEVENT, &ev) == 0) {
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE && !m_configured) {
                    if (!SetUpCapture()) m_failed = true;
                } else if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                    // The PC starts a new stream (and a new decoder) for a new size, so this
                    // shouldn't happen mid-stream.
                    Warn("the stream changed size mid-stream; pictures may be wrong");
                }
            }
        }
        if (!(pfd.revents & POLLIN) || !m_configured) continue;
        v4l2_plane plane{};
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = &plane;
        if (Ioctl(m_device, VIDIOC_DQBUF, &buf) < 0) continue;
        if (plane.bytesused == 0 || (buf.flags & V4L2_BUF_FLAG_ERROR)) {
            if (buf.flags & V4L2_BUF_FLAG_ERROR) Warn("the driver returned a corrupt picture");
            QueuePicture(int(buf.index));
            continue;
        }
        const uint32_t frameIndex = FrameIndex(buf.timestamp);
        const int64_t now = rc::NowNs();
        {
            std::lock_guard lock(m_statsMutex);
            auto it = m_inputTimes.find(int64_t(frameIndex));
            if (it != m_inputTimes.end()) {
                m_decodeMsSum += rc::NsToMs(now - it->second);
                m_decodeCount++;
                m_inputTimes.erase(it);
            }
        }
        {
            std::lock_guard lock(m_mutex);
            auto it = m_infos.find(int64_t(frameIndex));
            if (it != m_infos.end()) it->second.decodedNs = now;
        }
        {
            std::lock_guard lock(m_pictureMutex);
            m_pictures[buf.index].frameIndex = frameIndex;
            m_ready.push_back(int(buf.index));
        }
        if (!m_decoded++) RC_LOGI("Decoder %s %dx%d: first frame decoded", CodecName(m_codec), m_width, m_height);
    }
}

bool VideoDecoder::Latch(DecodedFrameInfo& out) {
    int index = -1;
    std::vector<int> skipped;
    {
        std::lock_guard lock(m_pictureMutex);
        if (m_ready.empty()) return false;
        index = m_ready.back();  // the newest; older ones go straight back to the driver
        m_ready.pop_back();
        skipped.assign(m_ready.begin(), m_ready.end());
        m_ready.clear();
    }
    for (int i : skipped) QueuePicture(i);

    Picture& pic = m_pictures[size_t(index)];
    if (pic.image == EGL_NO_IMAGE_KHR) {
        const EGLint attribs[] = {
            EGL_WIDTH, EGLint(m_captureWidth),
            EGL_HEIGHT, EGLint(m_captureHeight),
            EGL_LINUX_DRM_FOURCC_EXT, kDrmFormatNv12,
            EGL_DMA_BUF_PLANE0_FD_EXT, pic.fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(m_captureStride),
            EGL_DMA_BUF_PLANE1_FD_EXT, pic.fd,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGLint(m_captureUvOffset),
            EGL_DMA_BUF_PLANE1_PITCH_EXT, EGLint(m_captureStride),
            // What the PC's encoders produce.
            EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC709_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
            EGL_NONE};
        pic.image = pCreateImage(eglGetCurrentDisplay(), EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
        if (pic.image == EGL_NO_IMAGE_KHR) {
            Warn("EGL couldn't import a decoded picture (eglCreateImageKHR on a dmabuf)");
            QueuePicture(index);
            return false;
        }
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_texture);
    pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, pic.image);

    // The picture shown before this one is still being read by the frame in flight; the one
    // before that is free again.
    if (m_retiredPicture >= 0) QueuePicture(m_retiredPicture);
    m_retiredPicture = m_shown;
    m_shown = index;

    const int64_t pts = int64_t(pic.frameIndex);
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
