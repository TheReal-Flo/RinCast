// xdg-desktop-portal requests and sessions (portal_linux.h).
#include "platform/linux/portal_linux.h"

#include <gio/gunixfdlist.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include "rc/platform.h"
#include "util.h"

namespace rcs::portal {

namespace {

// The portal's main loop: started on first use, runs for the life of the process.
GMainContext* LoopContext() {
    static GMainContext* context = [] {
        GMainContext* c = g_main_context_new();
        std::thread([c] {
            g_main_context_push_thread_default(c);
            GMainLoop* loop = g_main_loop_new(c, FALSE);
            g_main_loop_run(loop);
        }).detach();
        return c;
    }();
    return context;
}

// Runs `fn` on the portal's thread and waits for it. Signal subscriptions deliver to the context
// that was the thread's default when they were made, so they are made there.
void RunOnLoop(std::function<void()> fn) {
    struct Job {
        std::function<void()> fn;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    g_main_context_invoke_full(
        LoopContext(), G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto& j = *static_cast<std::shared_ptr<Job>*>(data);
            j->fn();
            std::lock_guard lock(j->mutex);
            j->done = true;
            j->cv.notify_all();
            return G_SOURCE_REMOVE;
        },
        new std::shared_ptr<Job>(job), [](gpointer data) { delete static_cast<std::shared_ptr<Job>*>(data); });
    std::unique_lock lock(job->mutex);
    job->cv.wait(lock, [&] { return job->done; });
}

// ":1.42" -> "1_42", as request object paths have it.
std::string SenderPart() {
    std::string s = g_dbus_connection_get_unique_name(Bus());
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    for (char& c : s)
        if (c == '.') c = '_';
    return s;
}

struct Response {
    std::mutex mutex;
    std::condition_variable cv;
    bool answered = false;
    uint32_t code = 2;
    GVariant* results = nullptr;
};

void OnResponse(GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters, gpointer data) {
    auto& r = *static_cast<std::shared_ptr<Response>*>(data);
    std::lock_guard lock(r->mutex);
    if (r->answered) return;
    g_variant_get(parameters, "(u@a{sv})", &r->code, &r->results);
    r->answered = true;
    r->cv.notify_all();
}

std::filesystem::path TokenPath(const std::string& name) {
    std::string file = "portal-";
    for (char c : name) file += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return DataDir() / (file + ".token");
}

}  // namespace

GDBusConnection* Bus() {
    static GDBusConnection* bus = [] {
        GError* error = nullptr;
        GDBusConnection* b = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
        if (!b) {
            RC_LOGW("Portal: no D-Bus session bus (%s)", error ? error->message : "?");
            g_clear_error(&error);
        }
        return b;
    }();
    return bus;
}

std::string NewToken() {
    static std::atomic<unsigned> counter{0};
    return "rincast_" + std::to_string(++counter) + "_" + std::to_string(unsigned(rc::NowNs() & 0xFFFF));
}

uint32_t UintProperty(const char* interface, const char* property, uint32_t fallback) {
    if (!Bus()) return fallback;
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(Bus(), kBusName, kObjectPath, "org.freedesktop.DBus.Properties", "Get",
                                                  g_variant_new("(ss)", interface, property), G_VARIANT_TYPE("(v)"),
                                                  G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &error);
    if (!reply) {
        g_clear_error(&error);
        return fallback;
    }
    GVariant* value = nullptr;
    g_variant_get(reply, "(v)", &value);
    uint32_t out = fallback;
    if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32)) out = g_variant_get_uint32(value);
    if (value) g_variant_unref(value);
    g_variant_unref(reply);
    return out;
}

uint32_t InterfaceVersion(const char* interface) { return UintProperty(interface, "version", 0); }

int Request(const char* interface, const char* method, GVariant* parameters, const std::string& token,
            GVariant** results, const std::atomic<bool>& running, int timeoutMs) {
    *results = nullptr;
    if (!Bus()) {
        g_variant_unref(g_variant_ref_sink(parameters));
        return -1;
    }
    const std::string path = std::string(kObjectPath) + "/request/" + SenderPart() + "/" + token;
    auto response = std::make_shared<Response>();
    guint subscription = 0;
    // Subscribed before the call: the answer can come before the call returns.
    RunOnLoop([&] {
        subscription = g_dbus_connection_signal_subscribe(
            Bus(), kBusName, "org.freedesktop.portal.Request", "Response", path.c_str(), nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            OnResponse, new std::shared_ptr<Response>(response),
            [](gpointer data) { delete static_cast<std::shared_ptr<Response>*>(data); });
    });
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(Bus(), kBusName, kObjectPath, interface, method, parameters,
                                                  G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &error);
    int code = -1;
    if (!reply) {
        RC_LOGW("Portal: %s.%s failed: %s", interface, method, error ? error->message : "?");
        g_clear_error(&error);
    } else {
        g_variant_unref(reply);
        // The user may take a while to answer a dialog; Stop shouldn't wait for them.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::unique_lock lock(response->mutex);
        while (!response->answered && running && std::chrono::steady_clock::now() < deadline)
            response->cv.wait_for(lock, std::chrono::milliseconds(100));
        if (response->answered) {
            code = int(response->code);
            *results = std::exchange(response->results, nullptr);
        } else {
            lock.unlock();
            // Take the dialog away again.
            GVariant* closed = g_dbus_connection_call_sync(Bus(), kBusName, path.c_str(), "org.freedesktop.portal.Request",
                                                           "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
                                                           nullptr);
            if (closed) g_variant_unref(closed);
            if (running) RC_LOGW("Portal: no answer to %s.%s", interface, method);
        }
    }
    RunOnLoop([&] { g_dbus_connection_signal_unsubscribe(Bus(), subscription); });
    {
        std::lock_guard lock(response->mutex);
        if (response->results) g_variant_unref(std::exchange(response->results, nullptr));
    }
    return code;
}

int CallForFd(const char* interface, const char* method, GVariant* parameters) {
    if (!Bus()) {
        g_variant_unref(g_variant_ref_sink(parameters));
        return -1;
    }
    GError* error = nullptr;
    GUnixFDList* fds = nullptr;
    GVariant* reply = g_dbus_connection_call_with_unix_fd_list_sync(Bus(), kBusName, kObjectPath, interface, method,
                                                                    parameters, G_VARIANT_TYPE("(h)"),
                                                                    G_DBUS_CALL_FLAGS_NONE, 30000, nullptr, &fds,
                                                                    nullptr, &error);
    if (!reply) {
        RC_LOGW("Portal: %s.%s failed: %s", interface, method, error ? error->message : "?");
        g_clear_error(&error);
        return -1;
    }
    int32_t index = -1;
    g_variant_get(reply, "(h)", &index);
    g_variant_unref(reply);
    int fd = -1;
    if (fds) {
        fd = g_unix_fd_list_get(fds, index, &error);  // a dup, ours to close
        if (fd < 0) {
            RC_LOGW("Portal: %s.%s gave no file descriptor: %s", interface, method, error ? error->message : "?");
            g_clear_error(&error);
        }
        g_object_unref(fds);
    }
    return fd;
}

unsigned SubscribeClosed(const std::string& sessionHandle, std::function<void()> onClosed) {
    if (!Bus()) return 0;
    guint subscription = 0;
    RunOnLoop([&] {
        subscription = g_dbus_connection_signal_subscribe(
            Bus(), kBusName, "org.freedesktop.portal.Session", "Closed", sessionHandle.c_str(), nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE,
            [](GDBusConnection*, const char*, const char*, const char*, const char*, GVariant*, gpointer data) {
                (*static_cast<std::function<void()>*>(data))();
            },
            new std::function<void()>(std::move(onClosed)),
            [](gpointer data) { delete static_cast<std::function<void()>*>(data); });
    });
    return subscription;
}

void Unsubscribe(unsigned subscription) {
    if (!subscription || !Bus()) return;
    // On the portal's thread, so a callback isn't running while its data goes away.
    RunOnLoop([&] { g_dbus_connection_signal_unsubscribe(Bus(), subscription); });
}

void CloseSession(const std::string& sessionHandle) {
    if (sessionHandle.empty() || !Bus()) return;
    GVariant* reply = g_dbus_connection_call_sync(Bus(), kBusName, sessionHandle.c_str(), "org.freedesktop.portal.Session",
                                                  "Close", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (reply) g_variant_unref(reply);
}

std::string LoadToken(const std::string& name) {
    std::ifstream f(TokenPath(name));
    std::string token;
    std::getline(f, token);
    return token;
}

void SaveToken(const std::string& name, const std::string& token) {
    std::error_code ec;
    if (token.empty()) std::filesystem::remove(TokenPath(name), ec);
    else WriteFileAtomic(TokenPath(name), token + "\n");
}

}  // namespace rcs::portal
