// A connection to the sound server through the PulseAudio API (PulseAudio itself, or PipeWire's
// pipewire-pulse), run by the thread that owns it: every wait has a time limit, so a thread that
// streams can still be stopped when the server stalls or has nothing to give.
#pragma once

#include <pulse/pulseaudio.h>

#include "rc/platform.h"

namespace rcs {

class PulseLoop {
   public:
    PulseLoop() = default;
    PulseLoop(const PulseLoop&) = delete;
    PulseLoop& operator=(const PulseLoop&) = delete;
    ~PulseLoop() { Disconnect(); }

    // Connects as `name` ("RinCast Streamer"), waiting up to timeoutMs for the server.
    bool Connect(const char* name, int timeoutMs = 3000) {
        Disconnect();
        m_loop = pa_mainloop_new();
        if (!m_loop) return false;
        pa_proplist* props = pa_proplist_new();
        pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "RinCast Streamer");
        pa_proplist_sets(props, PA_PROP_APPLICATION_ID, "app.rincast.Streamer");
        pa_proplist_sets(props, PA_PROP_APPLICATION_ICON_NAME, "rincast-streamer");
        m_context = pa_context_new_with_proplist(pa_mainloop_get_api(m_loop), name, props);
        pa_proplist_free(props);
        if (!m_context || pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            Disconnect();
            return false;
        }
        const int64_t deadline = rc::NowNs() + int64_t(timeoutMs) * 1'000'000;
        while (rc::NowNs() < deadline) {
            const pa_context_state_t state = pa_context_get_state(m_context);
            if (state == PA_CONTEXT_READY) return true;
            if (!PA_CONTEXT_IS_GOOD(state)) break;
            Iterate(50);
        }
        Disconnect();
        return false;
    }

    void Disconnect() {
        if (m_context) {
            pa_context_disconnect(m_context);
            pa_context_unref(m_context);
            m_context = nullptr;
        }
        if (m_loop) {
            pa_mainloop_free(m_loop);
            m_loop = nullptr;
        }
    }

    bool Ok() const { return m_context && pa_context_get_state(m_context) == PA_CONTEXT_READY; }
    pa_context* Context() const { return m_context; }

    // Runs callbacks for at most timeoutMs.
    void Iterate(int timeoutMs) {
        if (!m_loop) return;
        if (pa_mainloop_prepare(m_loop, timeoutMs * 1000) < 0) return;
        if (pa_mainloop_poll(m_loop) < 0) return;
        pa_mainloop_dispatch(m_loop);
    }

    // Runs the loop until `op` is done, then lets it go. False on a timeout or a lost server.
    bool Wait(pa_operation* op, int timeoutMs = 3000) {
        if (!op) return false;
        const int64_t deadline = rc::NowNs() + int64_t(timeoutMs) * 1'000'000;
        while (pa_operation_get_state(op) == PA_OPERATION_RUNNING && rc::NowNs() < deadline && Ok()) Iterate(50);
        const bool done = pa_operation_get_state(op) == PA_OPERATION_DONE;
        if (!done) pa_operation_cancel(op);
        pa_operation_unref(op);
        return done;
    }

   private:
    pa_mainloop* m_loop = nullptr;
    pa_context* m_context = nullptr;
};

}  // namespace rcs
