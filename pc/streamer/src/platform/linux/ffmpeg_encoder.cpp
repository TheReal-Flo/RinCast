// Video encoding on Linux with FFmpeg, set up the way the Windows encoders are (nvenc_encoder.cpp)
// and VideoToolbox on macOS: no B-frames and no reordering, one IDR at the start and then only
// when the headset asks for one, constant bitrate with about a frame of VBV, BT.709 limited
// range written into the VUI, Annex B with the parameter sets on every keyframe.
//
//   VAAPI (AMD, Intel): BGRA pictures are uploaded and converted to NV12 / P010 by the GPU's video
//     processor (hwupload + scale_vaapi); DMA-BUF images are imported as VA surfaces and
//     converted the same way, without a copy.
//   NVENC (NVIDIA): BGRA from memory, which NVENC converts itself; DMA-BUF images are mapped and
//     read when they are linear.
//   Software: libx264 / libx265 behind swscale, the last resort.
#include "platform/linux/ffmpeg_encoder.h"

#include <dirent.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include "gpu.h"
#include "rc/platform.h"

namespace rcs {

namespace {

enum class Api { None, Vaapi, Nvenc, Software };

const char* BackendKey(Api b) {
    switch (b) {
        case Api::Vaapi: return "vaapi";
        case Api::Nvenc: return "nvenc";
        case Api::Software: return "software";
        case Api::None: break;
    }
    return "none";
}

const char* EncoderName(Api b, rc::VideoCodec c) {
    switch (b) {
        case Api::Vaapi:
            return c == rc::VideoCodec::H264 ? "h264_vaapi" : c == rc::VideoCodec::HEVC ? "hevc_vaapi" : "av1_vaapi";
        case Api::Nvenc:
            return c == rc::VideoCodec::H264 ? "h264_nvenc" : c == rc::VideoCodec::HEVC ? "hevc_nvenc" : "av1_nvenc";
        case Api::Software:
            return c == rc::VideoCodec::H264 ? "libx264" : c == rc::VideoCodec::HEVC ? "libx265" : nullptr;
        case Api::None: break;
    }
    return nullptr;
}

// While probing, encoders that can't open are expected: FFmpeg's complaints about them would
// only read like faults in the log.
thread_local bool t_quietFfmpeg = false;

void FfmpegLog(void* avcl, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING || t_quietFfmpeg) return;
    char line[1024];
    int prefix = 1;
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &prefix);
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
    if (n) RC_LOGW("FFmpeg: %s", line);
}

void RouteFfmpegLog() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_callback(FfmpegLog); });
}

// Frames handed to FFmpeg point at the caller's pixels: every encoder here copies or converts
// them before avcodec_send_frame / the filter graph returns, so nothing has to be freed.
void NoFree(void*, uint8_t*) {}

// The byte order of 32-bit pixels in memory, as FFmpeg names it.
AVPixelFormat MemoryFormat(uint32_t drmFourcc) {
    switch (drmFourcc) {
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888: return AV_PIX_FMT_BGR0;  // B, G, R, A/X in memory
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888: return AV_PIX_FMT_RGB0;  // R, G, B, A/X in memory
    }
    return AV_PIX_FMT_NONE;
}

// A keyframe without its parameter sets (an encoder that keeps them in extradata) can't start
// a decoder: they go in front then.
bool HasParameterSets(const std::vector<uint8_t>& d, rc::VideoCodec codec) {
    if (codec == rc::VideoCodec::AV1) return true;  // the sequence header OBU comes with keyframes
    const size_t end = std::min<size_t>(d.size(), 4096);
    for (size_t i = 0; i + 3 < end; ++i) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) continue;
        const uint8_t nal = d[i + 3];
        if (codec == rc::VideoCodec::H264 ? (nal & 0x1f) == 7 : ((nal >> 1) & 0x3f) == 32 || ((nal >> 1) & 0x3f) == 33)
            return true;
    }
    return false;
}

// What probing learned about VAAPI's low-power (fixed-function) mode per codec and depth, so an
// encoder doesn't try the variant that failed again.
std::mutex g_lowPowerMutex;
std::map<int, bool> g_lowPower;
int LowPowerKey(rc::VideoCodec c, bool tenBit) { return int(c) * 2 + (tenBit ? 1 : 0); }

}  // namespace

std::string RenderNode() {
    static const std::string node = [] {
        const UINT vendor = DescribeGpu().vendorId;
        std::vector<std::string> nodes;
        if (DIR* d = opendir("/sys/class/drm")) {
            while (dirent* e = readdir(d))
                if (std::strncmp(e->d_name, "renderD", 7) == 0) nodes.push_back(e->d_name);
            closedir(d);
        }
        std::sort(nodes.begin(), nodes.end());
        for (const auto& n : nodes) {
            std::ifstream f("/sys/class/drm/" + n + "/device/vendor");
            std::string text;
            std::getline(f, text);
            if (!text.empty() && UINT(std::strtoul(text.c_str(), nullptr, 16)) == vendor) return "/dev/dri/" + n;
        }
        return nodes.empty() ? std::string("/dev/dri/renderD128") : "/dev/dri/" + nodes.front();
    }();
    return node;
}

namespace {

struct EncoderCore {
    Api backend = Api::None;
    EncoderConfig config;
    bool tenBit = false;
    bool lowPower = false;

    AVCodecContext* ctx = nullptr;
    AVPacket* packet = nullptr;
    int64_t pts = 0;
    uint64_t frames = 0;
    bool loggedNoParameterSets = false;

    // VAAPI
    AVBufferRef* device = nullptr;     // the render node
    AVBufferRef* encFrames = nullptr;  // NV12 / P010 surfaces the encoder is opened with
    AVBufferRef* mapFrames = nullptr;  // DMA-BUF images imported as VA surfaces
    AVPixelFormat mapFormat = AV_PIX_FMT_NONE;
    uint32_t mapWidth = 0, mapHeight = 0;
    AVFilterGraph* graph = nullptr;  // to NV12 / P010 on the GPU
    AVFilterContext* graphIn = nullptr;
    AVFilterContext* graphOut = nullptr;
    AVPixelFormat graphFormat = AV_PIX_FMT_NONE;  // what the graph takes: BGR0 / RGB0 in memory, or VAAPI
    uint32_t graphWidth = 0, graphHeight = 0;

    // Software
    SwsContext* sws = nullptr;
    AVPixelFormat swsFrom = AV_PIX_FMT_NONE;
    AVFrame* yuv = nullptr;

    // Bitrate: set from any thread, applied before the next frame.
    std::atomic<uint32_t> wantKbps{0};
    uint32_t appliedKbps = 0;
    int64_t lastReopenNs = 0;

    std::vector<uint8_t> swizzled;  // NVENC with R, G, B, A pixels
    bool loggedTiled = false;

    ~EncoderCore() { Close(); }

    void CloseEncoder() {
        avcodec_free_context(&ctx);
        av_packet_free(&packet);
    }

    void FreeGraph() {
        avfilter_graph_free(&graph);
        graphIn = graphOut = nullptr;
        graphFormat = AV_PIX_FMT_NONE;
    }

    void Close() {
        FreeGraph();
        CloseEncoder();
        av_buffer_unref(&mapFrames);
        av_buffer_unref(&encFrames);
        av_buffer_unref(&device);
        sws_freeContext(sws);
        sws = nullptr;
        av_frame_free(&yuv);
        mapFormat = AV_PIX_FMT_NONE;
    }

    // Average and peak at the target, and one frame of VBV (two for desktops), as on Windows:
    // even frame sizes keep the latency flat, at the cost of quality spikes.
    void ApplyRate(uint32_t kbps) {
        const int64_t bps = int64_t(kbps) * 1000;
        const double fps = config.fps > 1 ? config.fps : 60;
        const int perFrame = int(std::min<double>(INT_MAX / 2, bps / fps));
        ctx->bit_rate = bps;
        ctx->rc_max_rate = bps;
        ctx->rc_buffer_size = config.desktop ? perFrame * 2 : perFrame;
        ctx->rc_initial_buffer_occupancy = ctx->rc_buffer_size;
    }

    bool CreateDevice() {
        if (device) return true;
        const std::string node = RenderNode();
        const int err = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, node.c_str(), nullptr, 0);
        if (err < 0) {
            if (!t_quietFfmpeg) RC_LOGW("VAAPI: can't open %s (%d)", node.c_str(), err);
            return false;
        }
        return true;
    }

    // One attempt at an encoder with the current settings.
    bool OpenEncoder(uint32_t kbps) {
        CloseEncoder();
        const char* name = EncoderName(backend, config.codec);
        const AVCodec* codec = name ? avcodec_find_encoder_by_name(name) : nullptr;
        if (!codec) return false;
        ctx = avcodec_alloc_context3(codec);
        packet = av_packet_alloc();
        if (!ctx || !packet) return false;

        ctx->width = int(config.width);
        ctx->height = int(config.height);
        ctx->framerate = av_d2q(config.fps > 1 ? config.fps : 60, 100000);
        ctx->time_base = av_inv_q(ctx->framerate);
        // Keyframes only when asked for (forceKeyframe): a periodic one would be a burst several
        // times a frame's size. INT_MAX frames is days at any rate.
        ctx->gop_size = INT_MAX;
        ctx->max_b_frames = 0;
        ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
        // BT.709, limited range, as the Windows encoders write it.
        ctx->color_primaries = AVCOL_PRI_BT709;
        ctx->color_trc = AVCOL_TRC_BT709;
        ctx->colorspace = AVCOL_SPC_BT709;
        ctx->color_range = AVCOL_RANGE_MPEG;
        ApplyRate(kbps);
        void* priv = ctx->priv_data;

        switch (backend) {
            case Api::Vaapi: {
                if (!CreateDevice()) return false;
                if (!encFrames) {
                    encFrames = av_hwframe_ctx_alloc(device);
                    if (!encFrames) return false;
                    auto* fc = reinterpret_cast<AVHWFramesContext*>(encFrames->data);
                    fc->format = AV_PIX_FMT_VAAPI;
                    fc->sw_format = tenBit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
                    fc->width = int(config.width);
                    fc->height = int(config.height);
                    if (av_hwframe_ctx_init(encFrames) < 0) {
                        av_buffer_unref(&encFrames);
                        return false;
                    }
                }
                ctx->pix_fmt = AV_PIX_FMT_VAAPI;
                ctx->hw_frames_ctx = av_buffer_ref(encFrames);
                av_opt_set(priv, "rc_mode", "CBR", 0);
                av_opt_set_int(priv, "low_power", lowPower ? 1 : 0, 0);
                // One frame in flight: the bitstream comes back before Encode returns.
                av_opt_set_int(priv, "async_depth", 1, 0);
                break;
            }
            case Api::Nvenc:
                ctx->pix_fmt = AV_PIX_FMT_BGR0;  // NVENC converts from RGB itself
                av_opt_set(priv, "preset", config.desktop ? "p4" : "p1", 0);
                av_opt_set(priv, "tune", config.desktop ? "ll" : "ull", 0);
                av_opt_set(priv, "rc", "cbr", 0);
                av_opt_set_int(priv, "zerolatency", 1, 0);
                av_opt_set_int(priv, "delay", 0, 0);
                av_opt_set_int(priv, "forced-idr", 1, 0);
                av_opt_set_int(priv, "no-scenecut", 1, 0);
                av_opt_set_int(priv, "rc-lookahead", 0, 0);
                av_opt_set_int(priv, "spatial-aq", config.adaptiveQuant ? 1 : 0, 0);
                av_opt_set(priv, "multipass", config.twoPass ? "qres" : "disabled", 0);
                // 10-bit output from 8-bit input, where this FFmpeg has the option.
                if (tenBit && av_opt_set_int(priv, "highbitdepth", 1, 0) < 0) return false;
                break;
            case Api::Software:
                if (config.codec == rc::VideoCodec::H264) {
                    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
                    av_opt_set(priv, "preset", "ultrafast", 0);
                    av_opt_set(priv, "tune", "zerolatency", 0);
                    av_opt_set_int(priv, "forced-idr", 1, 0);
                    av_opt_set(priv, "x264-params", "keyint=infinite:scenecut=0:repeat-headers=1", 0);
                } else {
                    ctx->pix_fmt = tenBit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
                    av_opt_set(priv, "preset", "ultrafast", 0);
                    av_opt_set(priv, "tune", "zerolatency", 0);
                    av_opt_set_int(priv, "forced-idr", 1, 0);
                    av_opt_set(priv, "x265-params", "keyint=-1:scenecut=0:repeat-headers=1:log-level=error", 0);
                }
                break;
            case Api::None: return false;
        }
        if (avcodec_open2(ctx, codec, nullptr) < 0) {
            CloseEncoder();
            return false;
        }
        appliedKbps = kbps;
        return true;
    }

    bool Open(Api b, const EncoderConfig& c) {
        Close();
        backend = b;
        config = c;
        tenBit = c.tenBit && c.codec != rc::VideoCodec::H264;
        const uint32_t kbps = wantKbps.load() ? wantKbps.load() : c.bitrateKbps;
        wantKbps = kbps;
        if (b != Api::Vaapi) return OpenEncoder(kbps);
        // Low power (Intel's fixed-function encoder) where the driver has it; AMD doesn't.
        int known = -1;
        {
            std::lock_guard lock(g_lowPowerMutex);
            if (auto it = g_lowPower.find(LowPowerKey(c.codec, tenBit)); it != g_lowPower.end()) known = it->second;
        }
        for (const bool lp : {true, false}) {
            if (known >= 0 && lp != bool(known)) continue;
            lowPower = lp;
            if (OpenEncoder(kbps)) {
                std::lock_guard lock(g_lowPowerMutex);
                g_lowPower[LowPowerKey(c.codec, tenBit)] = lp;
                return true;
            }
        }
        return false;
    }

    // VAAPI and the software encoders take the rate once, when they open: a new rate means
    // reopening, which costs an IDR. So only for a change of more than 15 %, at most every 2 s.
    // NVENC changes it on the fly (FFmpeg reconfigures it when it sees the new values).
    void ApplyPendingBitrate() {
        const uint32_t want = wantKbps.load();
        if (!ctx || !want || want == appliedKbps) return;
        if (backend == Api::Nvenc) {
            ApplyRate(want);
            appliedKbps = want;
            return;
        }
        const double change = std::fabs(double(want) - double(appliedKbps)) / std::max(1u, appliedKbps);
        const int64_t now = rc::NowNs();
        if (change <= 0.15 || now - lastReopenNs < 2'000'000'000LL) return;
        lastReopenNs = now;
        const uint32_t was = appliedKbps;
        if (OpenEncoder(want)) {
            RC_LOGI("%s: %u -> %u kbps (reopened, the next frame is an IDR)", ctx->codec->name, was, want);
        } else if (!OpenEncoder(was)) {
            RC_LOGE("%s: reopening at a new bitrate failed", EncoderName(backend, config.codec));
        }
    }

    // The VAAPI conversion: [hwupload ->] scale_vaapi to NV12 / P010, BT.709 limited range.
    bool BuildGraph(AVPixelFormat inFormat, AVBufferRef* inFrames, uint32_t w, uint32_t h) {
        if (graph && graphFormat == inFormat && graphWidth == w && graphHeight == h)
            return true;
        FreeGraph();
        graph = avfilter_graph_alloc();
        if (!graph) return false;
        graph->nb_threads = 1;
        graphIn = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffer"), "in");
        AVBufferSrcParameters* par = av_buffersrc_parameters_alloc();
        if (!graphIn || !par) {
            av_free(par);
            FreeGraph();
            return false;
        }
        par->format = inFormat;
        par->width = int(w);
        par->height = int(h);
        par->time_base = ctx->time_base;
        par->hw_frames_ctx = inFrames;  // referenced by the filter
        const int set = av_buffersrc_parameters_set(graphIn, par);
        av_free(par);
        if (set < 0 || avfilter_init_dict(graphIn, nullptr) < 0) {
            FreeGraph();
            return false;
        }
        AVFilterContext* last = graphIn;
        if (inFormat != AV_PIX_FMT_VAAPI) {
            AVFilterContext* up = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("hwupload"), "upload");
            if (!up) {
                FreeGraph();
                return false;
            }
            up->hw_device_ctx = av_buffer_ref(device);
            if (avfilter_init_str(up, nullptr) < 0 || avfilter_link(last, 0, up, 0) < 0) {
                FreeGraph();
                return false;
            }
            last = up;
        }
        char options[256];
        std::snprintf(options, sizeof(options), "w=%u:h=%u:format=%s:out_color_matrix=bt709:out_range=limited",
                      config.width, config.height, tenBit ? "p010" : "nv12");
        AVFilterContext* scale = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("scale_vaapi"), "convert");
        graphOut = avfilter_graph_alloc_filter(graph, avfilter_get_by_name("buffersink"), "out");
        if (!scale || !graphOut || avfilter_init_str(scale, options) < 0 || avfilter_link(last, 0, scale, 0) < 0 ||
            avfilter_init_str(graphOut, nullptr) < 0 || avfilter_link(scale, 0, graphOut, 0) < 0 ||
            avfilter_graph_config(graph, nullptr) < 0) {
            RC_LOGE("VAAPI: the conversion to %s doesn't work on this GPU", tenBit ? "P010" : "NV12");
            FreeGraph();
            return false;
        }
        graphFormat = inFormat;
        graphWidth = w;
        graphHeight = h;
        return true;
    }

    // Through the graph: one converted surface out per picture in.
    AVFrame* Convert(AVFrame* in) {
        if (av_buffersrc_add_frame_flags(graphIn, in, AV_BUFFERSRC_FLAG_PUSH) < 0) return nullptr;
        AVFrame* out = av_frame_alloc();
        if (out && av_buffersink_get_frame(graphOut, out) < 0) av_frame_free(&out);
        return out;
    }

    bool Send(AVFrame* frame, bool forceKeyframe, EncodedFrame& out) {
        frame->pts = pts++;
        frame->pict_type = forceKeyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        int err = avcodec_send_frame(ctx, frame);
        if (err < 0) {
            RC_LOGW("%s: sending a frame failed (%d)", ctx->codec->name, err);
            return false;
        }
        out.data.clear();
        out.keyframe = false;
        while ((err = avcodec_receive_packet(ctx, packet)) >= 0) {
            out.data.insert(out.data.end(), packet->data, packet->data + packet->size);
            if (packet->flags & AV_PKT_FLAG_KEY) out.keyframe = true;
            av_packet_unref(packet);
        }
        if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) RC_LOGW("%s: encoding failed (%d)", ctx->codec->name, err);
        if (out.data.empty()) return false;
        if (out.keyframe && !HasParameterSets(out.data, config.codec)) {
            if (ctx->extradata_size > 0) {
                out.data.insert(out.data.begin(), ctx->extradata, ctx->extradata + ctx->extradata_size);
            } else if (!loggedNoParameterSets) {
                loggedNoParameterSets = true;
                RC_LOGW("%s: a keyframe came without parameter sets", ctx->codec->name);
            }
        }
        out.timestamp = ++frames;
        return true;
    }

    // 32-bit pixels in memory, BGR0 or RGB0 byte order.
    bool EncodeMemory(const uint8_t* pixels, int stride, AVPixelFormat format, bool forceKeyframe, EncodedFrame& out) {
        if (!ctx || !pixels) return false;
        ApplyPendingBitrate();
        if (!ctx) return false;
        const int w = int(config.width), h = int(config.height);

        if (backend == Api::Nvenc && format != AV_PIX_FMT_BGR0) {
            // The encoder was opened for B, G, R order: swap R and B (rare: VR frames come as
            // DMA-BUF, which only lands here when mapped on NVIDIA).
            swizzled.resize(size_t(w) * h * 4);
            for (int y = 0; y < h; ++y) {
                const uint8_t* s = pixels + size_t(y) * stride;
                uint8_t* d = swizzled.data() + size_t(y) * w * 4;
                for (int x = 0; x < w; ++x, s += 4, d += 4) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    d[3] = s[3];
                }
            }
            pixels = swizzled.data();
            stride = w * 4;
            format = AV_PIX_FMT_BGR0;
        }

        AVFrame* frame = av_frame_alloc();
        if (!frame) return false;
        frame->format = format;
        frame->width = w;
        frame->height = h;
        frame->data[0] = const_cast<uint8_t*>(pixels);
        frame->linesize[0] = stride;
        frame->buf[0] = av_buffer_create(const_cast<uint8_t*>(pixels), size_t(stride) * h, NoFree, nullptr,
                                         AV_BUFFER_FLAG_READONLY);
        frame->color_range = AVCOL_RANGE_JPEG;
        frame->colorspace = AVCOL_SPC_RGB;
        bool ok = false;
        switch (backend) {
            case Api::Vaapi:
                if (BuildGraph(format, nullptr, config.width, config.height)) {
                    if (AVFrame* surface = Convert(frame)) {
                        ok = Send(surface, forceKeyframe, out);
                        av_frame_free(&surface);
                    }
                }
                break;
            case Api::Nvenc: ok = Send(frame, forceKeyframe, out); break;
            case Api::Software: {
                if (!sws || swsFrom != format) {
                    sws_freeContext(sws);
                    sws = sws_getContext(w, h, format, w, h, ctx->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);
                    swsFrom = format;
                    if (!sws) break;
                    // RGB in at full range, BT.709 limited range out.
                    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
                    sws_setColorspaceDetails(sws, bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
                }
                if (!yuv) {
                    yuv = av_frame_alloc();
                    yuv->format = ctx->pix_fmt;
                    yuv->width = w;
                    yuv->height = h;
                    if (av_frame_get_buffer(yuv, 0) < 0) {
                        av_frame_free(&yuv);
                        break;
                    }
                }
                if (av_frame_make_writable(yuv) < 0) break;
                const uint8_t* src[1] = {pixels};
                const int srcStride[1] = {stride};
                sws_scale(sws, src, srcStride, 0, h, yuv->data, yuv->linesize);
                yuv->color_range = AVCOL_RANGE_MPEG;
                yuv->colorspace = AVCOL_SPC_BT709;
                ok = Send(yuv, forceKeyframe, out);
                break;
            }
            case Api::None: break;
        }
        av_frame_free(&frame);
        return ok;
    }

    // VAAPI: the image becomes a VA surface (no copy) and the video processor converts it.
    bool EncodeDmaBufVaapi(const DmaBufFrame& f, AVPixelFormat format, bool forceKeyframe, EncodedFrame& out) {
        if (!mapFrames || mapFormat != format || mapWidth != f.width || mapHeight != f.height) {
            av_buffer_unref(&mapFrames);
            mapFrames = av_hwframe_ctx_alloc(device);
            if (!mapFrames) return false;
            auto* fc = reinterpret_cast<AVHWFramesContext*>(mapFrames->data);
            fc->format = AV_PIX_FMT_VAAPI;
            // RGB: scale_vaapi reads this to know it converts from RGB.
            fc->sw_format = format;
            fc->width = int(f.width);
            fc->height = int(f.height);
            if (av_hwframe_ctx_init(mapFrames) < 0) {
                av_buffer_unref(&mapFrames);
                RC_LOGE("VAAPI: can't make surfaces for DMA-BUF images");
                return false;
            }
            mapFormat = format;
            mapWidth = f.width;
            mapHeight = f.height;
            FreeGraph();  // it referenced the old surfaces' context
        }

        auto* desc = static_cast<AVDRMFrameDescriptor*>(av_mallocz(sizeof(AVDRMFrameDescriptor)));
        if (!desc) return false;
        AVDRMLayerDescriptor& layer = desc->layers[0];
        desc->nb_layers = 1;
        layer.format = f.drmFourcc;
        layer.nb_planes = std::clamp(f.planeCount, 1, int(AV_DRM_MAX_PLANES));
        for (int p = 0; p < layer.nb_planes; ++p) {
            const auto& plane = f.planes[p];
            int object = -1;
            for (int o = 0; o < desc->nb_objects; ++o)
                if (desc->objects[o].fd == plane.fd) object = o;
            if (object < 0) {
                object = desc->nb_objects++;
                const off_t size = lseek(plane.fd, 0, SEEK_END);
                desc->objects[object].fd = plane.fd;
                desc->objects[object].size =
                    size > 0 ? size_t(size) : size_t(plane.offset) + size_t(plane.pitch) * f.height;
                desc->objects[object].format_modifier = f.modifier;
            }
            layer.planes[p].object_index = object;
            layer.planes[p].offset = ptrdiff_t(plane.offset);
            layer.planes[p].pitch = ptrdiff_t(plane.pitch);
        }

        AVFrame* drm = av_frame_alloc();
        AVFrame* va = av_frame_alloc();
        bool ok = false;
        if (drm && va) {
            drm->format = AV_PIX_FMT_DRM_PRIME;
            drm->width = int(f.width);
            drm->height = int(f.height);
            drm->data[0] = reinterpret_cast<uint8_t*>(desc);
            drm->buf[0] = av_buffer_create(reinterpret_cast<uint8_t*>(desc), sizeof(*desc),
                                           [](void*, uint8_t* d) { av_free(d); }, nullptr, 0);
            desc = nullptr;  // drm->buf[0] owns it now
            va->format = AV_PIX_FMT_VAAPI;
            va->hw_frames_ctx = av_buffer_ref(mapFrames);
            const int err = av_hwframe_map(va, drm, AV_HWFRAME_MAP_READ);
            if (err < 0) {
                RC_LOGW("VAAPI: importing a DMA-BUF image failed (%d; format %08x, modifier %llx)", err, f.drmFourcc,
                        static_cast<unsigned long long>(f.modifier));
            } else {
                va->color_range = AVCOL_RANGE_JPEG;
                va->colorspace = AVCOL_SPC_RGB;
                if (BuildGraph(AV_PIX_FMT_VAAPI, mapFrames, f.width, f.height)) {
                    if (AVFrame* surface = Convert(va)) {
                        ok = Send(surface, forceKeyframe, out);
                        av_frame_free(&surface);
                    }
                }
            }
        }
        av_free(desc);
        av_frame_free(&va);  // unmaps (the surface goes; the fds stay the caller's)
        av_frame_free(&drm);
        return ok;
    }

    // NVENC and software: map the image and read it, which works for linear images only. A tiled
    // one would need the GPU to untile it (on NVIDIA: CUDA's external memory).
    bool EncodeDmaBufMapped(const DmaBufFrame& f, AVPixelFormat format, bool forceKeyframe, EncodedFrame& out) {
        if (f.modifier != DRM_FORMAT_MOD_LINEAR) {
            if (!loggedTiled) {
                loggedTiled = true;
                RC_LOGE("%s can't read a tiled DMA-BUF image (modifier %llx): it needs a linear one",
                        EncoderName(backend, config.codec), static_cast<unsigned long long>(f.modifier));
            }
            return false;
        }
        const auto& plane = f.planes[0];
        const off_t end = lseek(plane.fd, 0, SEEK_END);
        const size_t size = end > 0 ? size_t(end) : size_t(plane.offset) + size_t(plane.pitch) * f.height;
        if (size < size_t(plane.offset) + size_t(plane.pitch) * config.height) return false;
        void* map = mmap(nullptr, size, PROT_READ, MAP_SHARED, plane.fd, 0);
        if (map == MAP_FAILED) {
            RC_LOGW("Mapping a DMA-BUF image failed: %s", std::strerror(errno));
            return false;
        }
        // Tells the exporter we read now, so it flushes what the GPU still holds.
        dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        ioctl(plane.fd, DMA_BUF_IOCTL_SYNC, &sync);
        const bool ok = EncodeMemory(static_cast<const uint8_t*>(map) + plane.offset, int(plane.pitch), format,
                                     forceKeyframe, out);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        ioctl(plane.fd, DMA_BUF_IOCTL_SYNC, &sync);
        munmap(map, size);
        return ok;
    }
};


// Opens `backend` for a codec at a small size and encodes one black frame: only an encoder that
// gives back a bitstream counts (VAAPI drivers list profiles they then fail on).
bool TryEncoder(Api backend, rc::VideoCodec codec, bool tenBit) {
    t_quietFfmpeg = true;
    EncoderCore impl;
    EncoderConfig c;
    c.codec = codec;
    c.width = 640;
    c.height = 360;
    c.fps = 60;
    c.bitrateKbps = 5000;
    c.tenBit = tenBit;
    bool ok = impl.Open(backend, c);
    if (ok) {
        std::vector<uint8_t> black(size_t(c.width) * c.height * 4, 0);
        EncodedFrame out;
        ok = impl.EncodeMemory(black.data(), int(c.width) * 4, AV_PIX_FMT_BGR0, true, out) && out.keyframe;
    }
    impl.Close();
    t_quietFfmpeg = false;
    return ok;
}

EncoderCaps Probe(UINT vendorId) {
    RouteFfmpegLog();
    std::vector<Api> order;
    if (vendorId == 0x10DE) order = {Api::Nvenc, Api::Vaapi};
    else order = {Api::Vaapi, Api::Nvenc};
    order.push_back(Api::Software);

    for (const Api b : order) {
        EncoderCaps caps;
        caps.backend = BackendKey(b);
        caps.h264 = TryEncoder(b, rc::VideoCodec::H264, false);
        caps.hevc = TryEncoder(b, rc::VideoCodec::HEVC, false);
        caps.av1 = b != Api::Software && TryEncoder(b, rc::VideoCodec::AV1, false);
        if (!caps.h264 && !caps.hevc && !caps.av1) continue;
        caps.hevc10 = caps.hevc && TryEncoder(b, rc::VideoCodec::HEVC, true);
        caps.av110 = caps.av1 && TryEncoder(b, rc::VideoCodec::AV1, true);
        const std::string where = b == Api::Vaapi ? " on " + RenderNode() : std::string();
        RC_LOGI("Encoder: %s%s: H.264 %s, HEVC %s%s, AV1 %s%s", b == Api::Vaapi ? "VAAPI" : b == Api::Nvenc ? "NVENC" : "software",
                where.c_str(), caps.h264 ? "yes" : "no", caps.hevc ? "yes" : "no",
                caps.hevc10 ? " (10-bit too)" : "", caps.av1 ? "yes" : "no", caps.av110 ? " (10-bit too)" : "");
        if (b == Api::Software) RC_LOGW("Encoder: no hardware encoder works here, encoding on the CPU (slow, high latency)");
        return caps;
    }
    RC_LOGE("Encoder: neither a hardware encoder nor libx264 / libx265 work: this PC can't stream video");
    EncoderCaps none;
    none.backend = "none";
    return none;
}

}  // namespace

struct FfmpegEncoder::Impl : EncoderCore {};

EncoderCaps ProbeEncoderCaps(UINT vendorId) {
    static std::once_flag once;
    static EncoderCaps caps;
    std::call_once(once, [&] { caps = Probe(vendorId); });
    return caps;
}

FfmpegEncoder::FfmpegEncoder() : m_impl(std::make_unique<Impl>()) {}
FfmpegEncoder::~FfmpegEncoder() = default;

bool FfmpegEncoder::Init(const EncoderConfig& config) {
    RouteFfmpegLog();
    m_config = config;
    const EncoderCaps caps = ProbeEncoderCaps(DescribeGpu().vendorId);
    const Api backend = caps.backend == "vaapi"      ? Api::Vaapi
                        : caps.backend == "nvenc"    ? Api::Nvenc
                        : caps.backend == "software" ? Api::Software
                                                     : Api::None;
    EncoderConfig c = config;
    if (c.tenBit && !(c.codec == rc::VideoCodec::HEVC ? caps.hevc10 : c.codec == rc::VideoCodec::AV1 && caps.av110))
        c.tenBit = false;
    m_impl->wantKbps = 0;
    bool ok = backend != Api::None && m_impl->Open(backend, c);
    if (!ok && backend != Api::Software && c.codec != rc::VideoCodec::AV1) {
        RC_LOGW("Encoder: %s %s didn't open; encoding on the CPU instead", Name(), CodecName(c.codec));
        c.tenBit = false;  // not probed in software
        ok = m_impl->Open(Api::Software, c);
    }
    if (!ok) {
        RC_LOGE("Encoder: no %s encoder could be opened", CodecName(c.codec));
        m_impl->Close();
        return false;
    }
    m_config.tenBit = m_impl->tenBit;
    RC_LOGI("Encoder: %s %s %ux%u @ %.0f fps, %u kbps%s%s", Name(), CodecName(c.codec), c.width, c.height, c.fps,
            c.bitrateKbps, m_impl->tenBit ? ", 10-bit" : "", m_impl->lowPower ? ", low power" : "");
    return true;
}

bool FfmpegEncoder::EncodeBgra(const uint8_t* pixels, int stride, bool forceKeyframe, EncodedFrame& out) {
    return m_impl->EncodeMemory(pixels, stride, AV_PIX_FMT_BGR0, forceKeyframe, out);
}

bool FfmpegEncoder::EncodeRgba(const uint8_t* pixels, int stride, bool forceKeyframe, EncodedFrame& out) {
    return m_impl->EncodeMemory(pixels, stride, AV_PIX_FMT_RGB0, forceKeyframe, out);
}

bool FfmpegEncoder::EncodeDmaBuf(const DmaBufFrame& frame, bool forceKeyframe, EncodedFrame& out) {
    if (!m_impl->ctx || frame.planeCount < 1 || frame.planes[0].fd < 0) return false;
    const AVPixelFormat format = MemoryFormat(frame.drmFourcc);
    if (format == AV_PIX_FMT_NONE) {
        RC_LOGW("Encoder: DMA-BUF format %08x isn't supported", frame.drmFourcc);
        return false;
    }
    if (m_impl->backend == Api::Vaapi) {
        m_impl->ApplyPendingBitrate();
        return m_impl->ctx && m_impl->EncodeDmaBufVaapi(frame, format, forceKeyframe, out);
    }
    return m_impl->EncodeDmaBufMapped(frame, format, forceKeyframe, out);
}

bool FfmpegEncoder::TakesDmaBuf() const { return m_impl->backend == Api::Vaapi; }

void FfmpegEncoder::SetBitrate(uint32_t kbps) {
    m_config.bitrateKbps = kbps;
    m_impl->wantKbps = kbps;
}

const char* FfmpegEncoder::Name() const {
    switch (m_impl->backend) {
        case Api::Vaapi: return "VAAPI";
        case Api::Nvenc: return "NVENC";
        case Api::Software: return m_config.codec == rc::VideoCodec::H264 ? "x264" : "x265";
        case Api::None: break;
    }
    return "FFmpeg";
}

const char* FfmpegEncoder::Backend() const { return BackendKey(m_impl->backend); }

}  // namespace rcs
