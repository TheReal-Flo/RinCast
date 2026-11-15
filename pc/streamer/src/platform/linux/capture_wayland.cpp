// Desktop capture on Wayland: the xdg-desktop-portal ScreenCast session shares a monitor (the
// user picks it in the portal's dialog once; the restore token brings the same choice back), and
// its PipeWire stream delivers the pictures. DMA-BUF images go to the encoder without a copy when
// the compositor offers them (VAAPI imports them); shared memory is the fallback.
#include <fcntl.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <mutex>

#include "platform/linux/bgra_scale.h"
#include "platform/linux/linux_capture.h"
#include "platform/linux/portal_linux.h"
#include "rc/platform.h"

namespace rcs {

namespace {

constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";

constexpr uint32_t Fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}
constexpr uint32_t kDrmXrgb8888 = Fourcc('X', 'R', '2', '4');  // B, G, R, X in memory: PipeWire's BGRx
constexpr uint32_t kDrmArgb8888 = Fourcc('A', 'R', '2', '4');  // BGRA
constexpr uint64_t kModLinear = 0;

// Which DMA-BUF layouts we ask for. The encoder can't tell us which tiled layouts it imports, and
// a compositor allocates what both sides list: linear images import everywhere (VAAPI on AMD and
// Intel, CUDA on NVIDIA), at a small cost on the compositor's side.
constexpr uint64_t kModifiers[] = {kModLinear};

constexpr spa_video_format kFormats[] = {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA};

// One portal dialog at a time: two streams starting together would otherwise show two dialogs
// at once, and the user couldn't tell which monitor each one is for.
std::mutex g_dialogMutex;

GVariant* Options(const std::string& token) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
    return g_variant_builder_end(&b);
}

// The session handle from CreateSession's results ("s" in older portals, "o" in newer ones).
std::string SessionHandle(GVariant* results) {
    std::string out;
    if (GVariant* v = g_variant_lookup_value(results, "session_handle", nullptr)) {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
            out = g_variant_get_string(v, nullptr);
        g_variant_unref(v);
    }
    return out;
}

struct spa_pod* BuildFormat(spa_pod_builder* b, spa_video_format format, uint32_t width, uint32_t height, float fps,
                            const uint64_t* modifiers, size_t modifierCount, bool fixedModifier) {
    spa_pod_frame object, choice;
    spa_pod_builder_push_object(b, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
    if (modifierCount) {
        if (fixedModifier) {
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
            spa_pod_builder_long(b, int64_t(modifiers[0]));
        } else {
            spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
            spa_pod_builder_push_choice(b, &choice, SPA_CHOICE_Enum, 0);
            spa_pod_builder_long(b, int64_t(modifiers[0]));  // the default
            for (size_t i = 0; i < modifierCount; i++) spa_pod_builder_long(b, int64_t(modifiers[i]));
            spa_pod_builder_pop(b, &choice);
        }
    }
    spa_rectangle size{width, height}, minSize{1, 1}, maxSize{16384, 16384};
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&size, &minSize, &maxSize), 0);
    // 0/1 is "variable": compositors send a picture when something changed.
    spa_fraction rate{uint32_t(fps + 0.5f), 1}, minRate{0, 1}, maxRate{1000, 1};
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &minRate, &maxRate), 0);
    return static_cast<spa_pod*>(spa_pod_builder_pop(b, &object));
}

class WaylandCapture : public LinuxCapture {
   public:
    ~WaylandCapture() override { Stop(); }

    bool Start(const MonitorInfo& monitor, uint32_t width, uint32_t height, float fps,
               const std::atomic<bool>& running) override {
        Stop();
        m_width = width;
        m_height = height;
        m_fps = fps;
        m_ended = false;
        if (!OpenPortal(monitor, running)) {
            Stop();
            return false;
        }
        if (!ConnectStream()) {
            Stop();
            return false;
        }
        return true;
    }

    int Wait(int64_t timeoutNs, CapturedPicture& out) override {
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::nanoseconds(timeoutNs), [&] { return m_pending || m_ended; });
            if (m_ended) return -1;
            if (!m_pending) return 0;
        }
        // In PipeWire's lock, then ours (the order its callbacks take them in): a buffer can't
        // be removed while it's handed over.
        pw_thread_loop_lock(m_loop);
        bool got = false;
        {
            std::lock_guard lock(m_mutex);
            if (pw_buffer* buffer = std::exchange(m_pending, nullptr)) got = TakePicture(buffer);
        }
        pw_thread_loop_unlock(m_loop);
        if (!got) return 0;
        out = m_picture;
        return 1;
    }

    void Stop() override {
        if (m_loop) {
            pw_thread_loop_lock(m_loop);
            if (m_stream) {
                spa_hook_remove(&m_streamListener);
                pw_stream_disconnect(m_stream);
                pw_stream_destroy(m_stream);
                m_stream = nullptr;
            }
            if (m_core) {
                spa_hook_remove(&m_coreListener);
                pw_core_disconnect(m_core);
                m_core = nullptr;
            }
            pw_thread_loop_unlock(m_loop);
            pw_thread_loop_stop(m_loop);
            if (m_context) pw_context_destroy(m_context);
            m_context = nullptr;
            pw_thread_loop_destroy(m_loop);
            m_loop = nullptr;
        }
        m_pending = m_held = nullptr;
        CloseHeldFds();
        if (m_remoteFd >= 0) close(m_remoteFd);
        m_remoteFd = -1;
        portal::Unsubscribe(m_closedSubscription);
        m_closedSubscription = 0;
        portal::CloseSession(m_session);
        m_session.clear();
        m_cursorX = m_cursorY = -1;
    }

    void Cursor(int& x, int& y) override {
        x = m_cursorX;
        y = m_cursorY;
    }

    void PreferMemory() override {
        if (m_preferMemory.exchange(true) || !m_loop) return;
        RC_LOGI("Wayland capture: the encoder can't take DMA-BUF pictures; asking for shared memory");
        pw_thread_loop_lock(m_loop);
        Renegotiate();
        pw_thread_loop_unlock(m_loop);
    }

   private:
    // --- The portal ---------------------------------------------------------------------------

    bool OpenPortal(const MonitorInfo& monitor, const std::atomic<bool>& running) {
        const uint32_t version = portal::InterfaceVersion(kScreenCast);
        if (!version) {
            static bool said = false;
            if (!said) RC_LOGW("Wayland capture: no screen-cast portal (install xdg-desktop-portal and your desktop's backend)");
            said = true;
            return false;
        }
        std::lock_guard dialog(g_dialogMutex);
        if (!running) return false;
        GVariant* results = nullptr;

        std::string token = portal::NewToken();
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(portal::NewToken().c_str()));
        if (portal::Request(kScreenCast, "CreateSession", g_variant_new("(a{sv})", &b), token, &results, running, 30000) != 0) {
            if (results) g_variant_unref(results);
            RC_LOGW("Wayland capture: the portal didn't create a session");
            return false;
        }
        m_session = SessionHandle(results);
        g_variant_unref(results);
        if (m_session.empty()) return false;

        // The cursor drawn into the pictures (as on Windows) where the portal can; else its
        // position as metadata, so the headset still knows where it is.
        const uint32_t cursorModes = version >= 2 ? portal::UintProperty(kScreenCast, "AvailableCursorModes", 0) : 0;
        const uint32_t cursorMode = (cursorModes & 2) ? 2 : (cursorModes & 4) ? 4 : 0;
        m_cursorMetadata = cursorMode == 4;
        const std::string tokenName = "screencast-" + monitor.id;
        const std::string restore = version >= 4 ? portal::LoadToken(tokenName) : "";

        token = portal::NewToken();
        g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
        g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token.c_str()));
        g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(1));  // monitors
        g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(FALSE));
        if (cursorMode) g_variant_builder_add(&b, "{sv}", "cursor_mode", g_variant_new_uint32(cursorMode));
        if (version >= 4) {
            g_variant_builder_add(&b, "{sv}", "persist_mode", g_variant_new_uint32(2));  // until revoked
            if (!restore.empty()) g_variant_builder_add(&b, "{sv}", "restore_token", g_variant_new_string(restore.c_str()));
        }
        if (portal::Request(kScreenCast, "SelectSources", g_variant_new("(oa{sv})", m_session.c_str(), &b), token, &results,
                            running, 30000) != 0) {
            if (results) g_variant_unref(results);
            RC_LOGW("Wayland capture: the portal refused the source selection");
            return false;
        }
        if (results) g_variant_unref(results);

        if (restore.empty())
            RC_LOGI("Wayland capture: choose %s (%dx%d) in the screen-sharing dialog on the PC", monitor.name.c_str(),
                    monitor.width, monitor.height);
        token = portal::NewToken();
        // The dialog: the user may take a while.
        const int code = portal::Request(kScreenCast, "Start", g_variant_new("(os@a{sv})", m_session.c_str(), "", Options(token)),
                                         token, &results, running, 5 * 60 * 1000);
        if (code != 0) {
            if (results) g_variant_unref(results);
            if (code == 1) RC_LOGW("Wayland capture: screen sharing was declined");
            else if (running) RC_LOGW("Wayland capture: screen sharing didn't start");
            // A token that no longer works would fail the same way next time.
            if (code == 2 && !restore.empty()) portal::SaveToken(tokenName, "");
            return false;
        }
        m_node = 0;
        if (GVariant* streams = g_variant_lookup_value(results, "streams", G_VARIANT_TYPE("a(ua{sv})"))) {
            GVariantIter iter;
            g_variant_iter_init(&iter, streams);
            uint32_t node = 0;
            GVariant* props = nullptr;
            if (g_variant_iter_next(&iter, "(u@a{sv})", &node, &props)) {
                m_node = node;
                int32_t w = 0, h = 0;
                if (g_variant_lookup(props, "size", "(ii)", &w, &h))
                    RC_LOGI("Wayland capture: %s is PipeWire node %u (%dx%d)", monitor.name.c_str(), node, w, h);
                g_variant_unref(props);
            }
            g_variant_unref(streams);
        }
        const char* newToken = nullptr;
        if (g_variant_lookup(results, "restore_token", "&s", &newToken) && newToken) portal::SaveToken(tokenName, newToken);
        g_variant_unref(results);
        if (!m_node) {
            RC_LOGW("Wayland capture: the portal shared no stream");
            return false;
        }

        m_closedSubscription = portal::SubscribeClosed(m_session, [this] {
            RC_LOGI("Wayland capture: the screen-cast session was closed");
            std::lock_guard lock(m_mutex);
            m_ended = true;
            m_cv.notify_all();
        });

        GVariantBuilder empty;
        g_variant_builder_init(&empty, G_VARIANT_TYPE_VARDICT);
        m_remoteFd = portal::CallForFd(kScreenCast, "OpenPipeWireRemote", g_variant_new("(oa{sv})", m_session.c_str(), &empty));
        return m_remoteFd >= 0;
    }

    // --- PipeWire -------------------------------------------------------------------------------

    bool ConnectStream() {
        static const bool initialized = [] {
            pw_init(nullptr, nullptr);
            return true;
        }();
        (void)initialized;

        // Here, where the class is complete, so the callbacks can reach its members.
        static const pw_core_events coreEvents = {
            .version = PW_VERSION_CORE_EVENTS,
            .error = [](void* self, uint32_t id, int seq, int res, const char* message) {
                static_cast<WaylandCapture*>(self)->OnCoreError(id, seq, res, message);
            },
        };
        static const pw_stream_events streamEvents = {
            .version = PW_VERSION_STREAM_EVENTS,
            .state_changed = [](void* self, pw_stream_state old, pw_stream_state state, const char* error) {
                static_cast<WaylandCapture*>(self)->OnStateChanged(old, state, error);
            },
            .param_changed = [](void* self, uint32_t id, const spa_pod* param) {
                static_cast<WaylandCapture*>(self)->OnParamChanged(id, param);
            },
            .remove_buffer = [](void* self, pw_buffer* buffer) { static_cast<WaylandCapture*>(self)->OnRemoveBuffer(buffer); },
            .process = [](void* self) { static_cast<WaylandCapture*>(self)->OnProcess(); },
        };

        m_loop = pw_thread_loop_new("rincast-capture", nullptr);
        if (!m_loop) return false;
        m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
        if (!m_context || pw_thread_loop_start(m_loop) < 0) return false;

        pw_thread_loop_lock(m_loop);
        m_core = pw_context_connect_fd(m_context, std::exchange(m_remoteFd, -1), nullptr, 0);  // takes the fd
        if (!m_core) {
            pw_thread_loop_unlock(m_loop);
            RC_LOGW("Wayland capture: can't connect to PipeWire");
            return false;
        }
        pw_core_add_listener(m_core, &m_coreListener, &coreEvents, this);
        m_stream = pw_stream_new(m_core, "RinCast desktop",
                                 pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                   PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        if (!m_stream) {
            pw_thread_loop_unlock(m_loop);
            return false;
        }
        pw_stream_add_listener(m_stream, &m_streamListener, &streamEvents, this);
        uint8_t buffer[4096];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[8];
        const uint32_t count = Offers(&b, params);
        const int result = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, m_node,
                                             pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params,
                                             count);
        pw_thread_loop_unlock(m_loop);
        if (result < 0) {
            RC_LOGW("Wayland capture: can't connect the PipeWire stream: %s", spa_strerror(result));
            return false;
        }
        return true;
    }

    // What we take: DMA-BUF (with the modifiers above) first, unless the encoder said no, then
    // shared memory.
    uint32_t Offers(spa_pod_builder* b, const spa_pod** params) {
        uint32_t n = 0;
        if (!m_preferMemory)
            for (spa_video_format f : kFormats)
                params[n++] = BuildFormat(b, f, m_width, m_height, m_fps, kModifiers, std::size(kModifiers), false);
        for (spa_video_format f : kFormats) params[n++] = BuildFormat(b, f, m_width, m_height, m_fps, nullptr, 0, false);
        return n;
    }

    // In PipeWire's lock.
    void Renegotiate() {
        if (!m_stream) return;
        uint8_t buffer[4096];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[8];
        const uint32_t count = Offers(&b, params);
        pw_stream_update_params(m_stream, params, count);
    }

    void OnParamChanged(uint32_t id, const spa_pod* param) {
        if (!param || id != SPA_PARAM_Format) return;
        uint32_t mediaType = 0, mediaSubtype = 0;
        if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video ||
            mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
            return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;

        const spa_pod_prop* modifier = spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier);
        if (modifier && (modifier->flags & SPA_POD_PROP_FLAG_DONT_FIXATE)) {
            // The compositor left the modifier to us: pick one of ours it offered, and say so.
            uint32_t count = 0, choice = 0;
            const spa_pod* values = spa_pod_get_values(&modifier->value, &count, &choice);
            uint64_t picked = kModLinear;
            if (values && SPA_POD_TYPE(values) == SPA_TYPE_Long && count > 0) {
                const auto* offered = static_cast<const int64_t*>(SPA_POD_BODY(values));
                picked = uint64_t(offered[0]);
                for (uint32_t i = 0; i < count; i++)
                    if (uint64_t(offered[i]) == kModLinear) picked = kModLinear;
            }
            uint8_t buffer[4096];
            spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            const spa_pod* params[9];
            params[0] = BuildFormat(&b, info.format, info.size.width, info.size.height, m_fps, &picked, 1, true);
            const uint32_t count2 = Offers(&b, params + 1);
            pw_stream_update_params(m_stream, params, count2 + 1);
            return;
        }

        const bool dmabuf = modifier != nullptr;
        if (dmabuf && (info.size.width != m_width || info.size.height != m_height) && !m_preferMemory) {
            // The compositor shares the monitor at its own size and the stream wants another:
            // only pictures in memory can be scaled here.
            RC_LOGI("Wayland capture: the portal shares %ux%u for a %ux%u stream; scaling in memory", info.size.width,
                    info.size.height, m_width, m_height);
            m_preferMemory = true;
            Renegotiate();
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            m_format = info;
            m_dmabuf = dmabuf;
        }
        RC_LOGI("Wayland capture: %ux%u %s, %s", info.size.width, info.size.height,
                info.format == SPA_VIDEO_FORMAT_BGRA ? "BGRA" : "BGRx", dmabuf ? "DMA-BUF" : "shared memory");

        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_pod* params[3];
        uint32_t n = 0;
        const int dataTypes = dmabuf ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
        // One buffer stays with us (the picture repeated when nothing changes): ask for spares.
        params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 3, 16),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(dataTypes)));
        params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
            SPA_PARAM_META_size, SPA_POD_Int(int(sizeof(spa_meta_header)))));
        if (m_cursorMetadata) {
            // Position only: the bitmap is asked for at its smallest, since it isn't drawn.
            const int size = int(sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + 64 * 64 * 4);
            params[n++] = static_cast<const spa_pod*>(spa_pod_builder_add_object(
                &b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
                SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(size, int(sizeof(spa_meta_cursor)), 1 << 20)));
        }
        pw_stream_update_params(m_stream, params, n);
    }

    void OnProcess() {
        pw_buffer* newest = nullptr;
        while (pw_buffer* b = pw_stream_dequeue_buffer(m_stream)) {
            if (newest) pw_stream_queue_buffer(m_stream, newest);
            newest = b;
        }
        if (!newest) return;
        spa_buffer* sb = newest->buffer;
        if (m_cursorMetadata) ReadCursor(sb);
        bool picture = sb->n_datas > 0 && sb->datas[0].chunk && !(sb->datas[0].chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);
        if (auto* header = static_cast<spa_meta_header*>(spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(spa_meta_header))))
            if (header->flags & SPA_META_HEADER_FLAG_CORRUPTED) picture = false;
        if (picture) {
            const spa_data& d = sb->datas[0];
            // A buffer without a size carries only a cursor update.
            if (d.type == SPA_DATA_DmaBuf) picture = d.fd >= 0;
            else picture = d.data && d.chunk->size > 0;
        }
        if (!picture) {
            pw_stream_queue_buffer(m_stream, newest);
            return;
        }
        std::lock_guard lock(m_mutex);
        if (m_pending) pw_stream_queue_buffer(m_stream, m_pending);
        m_pending = newest;
        m_cv.notify_all();
    }

    // The cursor's position (metadata mode), in the stream's pixels.
    void ReadCursor(spa_buffer* sb) {
        auto* cursor = static_cast<spa_meta_cursor*>(spa_buffer_find_meta_data(sb, SPA_META_Cursor, sizeof(spa_meta_cursor)));
        if (!cursor) return;
        if (!spa_meta_cursor_is_valid(cursor)) return;  // unchanged
        const uint32_t w = m_format.size.width, h = m_format.size.height;
        const int32_t x = cursor->position.x, y = cursor->position.y;
        if (!w || !h || x < 0 || y < 0 || uint32_t(x) >= w || uint32_t(y) >= h) {
            m_cursorX = m_cursorY = -1;
            return;
        }
        m_cursorX = int(int64_t(x) * m_width / w);
        m_cursorY = int(int64_t(y) * m_height / h);
    }

    // In both locks. Fills m_picture from `buffer`, and gives the buffer held before back.
    bool TakePicture(pw_buffer* buffer) {
        const spa_buffer* sb = buffer->buffer;
        const spa_data& d = sb->datas[0];
        const uint32_t w = m_format.size.width, h = m_format.size.height;
        CapturedPicture p;
        p.captureNs = rc::NowNs();
        bool ok = false;
        if (d.type == SPA_DATA_DmaBuf) {
            // Held until the next picture comes, so the pacer can encode it again: the compositor
            // renders into the other buffers meanwhile. Our own copies of the descriptors keep
            // the image alive even if PipeWire drops the buffer (renegotiation) mid-encode.
            CloseHeldFds();
            p.width = w;
            p.height = h;
            p.isDmaBuf = true;
            p.dmabuf.width = w;
            p.dmabuf.height = h;
            p.dmabuf.drmFourcc = m_format.format == SPA_VIDEO_FORMAT_BGRA ? kDrmArgb8888 : kDrmXrgb8888;
            p.dmabuf.modifier = m_format.modifier;
            p.dmabuf.planeCount = int(std::min<uint32_t>(sb->n_datas, 4));
            for (int i = 0; i < p.dmabuf.planeCount; i++) {
                const spa_data& plane = sb->datas[i];
                p.dmabuf.planes[i].fd = fcntl(int(plane.fd), F_DUPFD_CLOEXEC, 3);
                p.dmabuf.planes[i].offset = plane.chunk->offset;
                p.dmabuf.planes[i].pitch = uint32_t(plane.chunk->stride);
                m_heldFds[i] = p.dmabuf.planes[i].fd;
            }
            ok = p.dmabuf.planes[0].fd >= 0 && w == m_width && h == m_height;
            if (m_held) pw_stream_queue_buffer(m_stream, m_held);
            m_held = buffer;
        } else {
            // Memory pictures are copied (scaled when needed) into our own, and the buffer goes
            // back at once: a mapped buffer can't be kept safely across a renegotiation.
            const uint8_t* src = static_cast<const uint8_t*>(d.data) + d.chunk->offset;
            const int stride = d.chunk->stride > 0 ? d.chunk->stride : int(w) * 4;
            if (d.chunk->offset + uint64_t(stride) * h <= d.maxsize && w && h) {
                if (w == m_width && h == m_height) {
                    m_copy.resize(size_t(stride) * h);
                    std::memcpy(m_copy.data(), src, m_copy.size());
                    p.stride = stride;
                } else {
                    ScaleBgra(src, int(w), int(h), stride, m_copy, int(m_width), int(m_height));
                    p.stride = int(m_width) * 4;
                }
                p.width = m_width;
                p.height = m_height;
                p.pixels = m_copy.data();
                ok = true;
            }
            pw_stream_queue_buffer(m_stream, buffer);
            if (m_held) pw_stream_queue_buffer(m_stream, m_held);
            m_held = nullptr;
        }
        if (ok) m_picture = p;
        return ok;
    }

    void CloseHeldFds() {
        for (int& fd : m_heldFds) {
            if (fd >= 0) close(fd);
            fd = -1;
        }
    }

    void OnStateChanged(pw_stream_state old, pw_stream_state state, const char* error) {
        if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_CONNECTING)) {
            RC_LOGW("Wayland capture: the PipeWire stream ended%s%s", error ? ": " : "", error ? error : "");
            std::lock_guard lock(m_mutex);
            m_ended = true;
            m_cv.notify_all();
        }
    }

    void OnRemoveBuffer(pw_buffer* buffer) {
        std::lock_guard lock(m_mutex);
        if (m_pending == buffer) m_pending = nullptr;
        if (m_held == buffer) m_held = nullptr;
    }

    void OnCoreError(uint32_t id, int seq, int res, const char* message) {
        if (id != PW_ID_CORE) return;
        RC_LOGW("Wayland capture: PipeWire error: %s", message ? message : spa_strerror(res));
        std::lock_guard lock(m_mutex);
        m_ended = true;
        m_cv.notify_all();
    }

    uint32_t m_width = 0, m_height = 0;
    float m_fps = 60;

    std::string m_session;
    unsigned m_closedSubscription = 0;
    uint32_t m_node = 0;
    int m_remoteFd = -1;
    bool m_cursorMetadata = false;

    pw_thread_loop* m_loop = nullptr;
    pw_context* m_context = nullptr;
    pw_core* m_core = nullptr;
    pw_stream* m_stream = nullptr;
    spa_hook m_streamListener{};
    spa_hook m_coreListener{};

    std::atomic<bool> m_preferMemory{false};
    std::mutex m_mutex;  // after PipeWire's lock where both are taken
    std::condition_variable m_cv;
    spa_video_info_raw m_format{};
    bool m_dmabuf = false;
    bool m_ended = false;
    pw_buffer* m_pending = nullptr;  // the newest picture, not taken yet
    pw_buffer* m_held = nullptr;     // the DMA-BUF picture handed out last
    int m_heldFds[4] = {-1, -1, -1, -1};
    std::vector<uint8_t> m_copy;
    CapturedPicture m_picture;

    std::atomic<int> m_cursorX{-1}, m_cursorY{-1};
};

}  // namespace

std::unique_ptr<LinuxCapture> CreateWaylandCapture() { return std::make_unique<WaylandCapture>(); }

}  // namespace rcs
