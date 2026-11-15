// RinCast Streamer on Linux: one instance per user, a GTK window showing the same web UI as on
// Windows (pc/streamer/ui) in WebKitGTK, and a tray icon (StatusNotifierItem, through
// libayatana-appindicator). Without a display, or with --headless, it runs in the terminal
// instead (platform/posix/main_terminal.cpp).
//
// The UI talks to the streamer through WebView2's bridge on Windows: window.chrome.webview's
// postMessage and its "message" events. A script injected before the page's own provides the
// same object here, on top of a WebKit script message handler.
#include <fcntl.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <libayatana-appindicator/app-indicator.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>
#include <webkit2/webkit2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app.h"
#include "net/stream_server.h"
#include "platform/posix/platform_main.h"
#include "rc/platform.h"
#include "util.h"

using namespace rcs;

namespace {

constexpr const char* kAppId = "app.rincast.Streamer";
constexpr const char* kScheme = "rincast";
constexpr const char* kStartPage = "rincast://app/index.html";

GtkApplication* g_app = nullptr;
GtkWidget* g_window = nullptr;
WebKitWebView* g_view = nullptr;
AppIndicator* g_indicator = nullptr;
GtkWidget* g_disconnectItem = nullptr;
std::filesystem::path g_uiRoot;
bool g_quitting = false;
bool g_trayHintShown = false;

// WebView2's window.chrome.webview, for the page: messages go to the "rincast" handler as text,
// and the streamer's come back as "message" events whose data is the parsed JSON, as
// PostWebMessageAsJson delivers them.
constexpr const char* kBridgeScript = R"JS(
(() => {
  if (window.chrome && window.chrome.webview) return;
  const target = new EventTarget();
  const handler = window.webkit.messageHandlers.rincast;
  window.chrome = window.chrome || {};
  window.chrome.webview = {
    postMessage(message) { handler.postMessage(typeof message === "string" ? message : JSON.stringify(message)); },
    addEventListener(type, listener, options) { target.addEventListener(type, listener, options); },
    removeEventListener(type, listener, options) { target.removeEventListener(type, listener, options); },
    rincastDeliver(data) { target.dispatchEvent(new MessageEvent("message", { data })); },
  };
})();
)JS";

// The ui folder next to the executable, or in the source tree when run from a build folder there.
std::filesystem::path FindUiRoot() {
    std::filesystem::path ui = ExeDir() / "ui";
    if (!std::filesystem::exists(ui / "index.html")) ui = ExeDir() / ".." / ".." / "ui";  // dev tree
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(ui, ec);
    return ec ? ui : canonical;
}

const char* MimeType(const std::filesystem::path& file) {
    const std::string ext = file.extension().string();
    if (ext == ".html") return "text/html";
    if (ext == ".js" || ext == ".mjs") return "text/javascript";  // ES modules need a script type
    if (ext == ".css") return "text/css";
    if (ext == ".json") return "application/json";
    if (ext == ".png") return "image/png";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".webp") return "image/webp";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".wasm") return "application/wasm";
    return "application/octet-stream";
}

// rincast://app/<path>: a file of the ui folder, and nothing outside it.
void OnUiRequest(WebKitURISchemeRequest* request, gpointer) {
    const char* rawPath = webkit_uri_scheme_request_get_path(request);
    char* decoded = g_uri_unescape_string(rawPath ? rawPath : "", nullptr);
    std::string rel = decoded ? decoded : "";
    g_free(decoded);
    while (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    if (rel.empty()) rel = "index.html";
    const std::filesystem::path file = (g_uiRoot / rel).lexically_normal();
    const std::string root = g_uiRoot.lexically_normal().string();
    std::vector<uint8_t> data;
    if (file.string().rfind(root + "/", 0) != 0 || !ReadFileBytes(file, data)) {
        GError* error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s not found", rel.c_str());
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    GBytes* bytes = g_bytes_new(data.data(), data.size());
    GInputStream* stream = g_memory_input_stream_new_from_bytes(bytes);
    webkit_uri_scheme_request_finish(request, stream, gint64(data.size()), MimeType(file));
    g_object_unref(stream);
    g_bytes_unref(bytes);
}

void OnScriptMessage(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer) {
    JSCValue* value = webkit_javascript_result_get_js_value(result);
    if (!value || !jsc_value_is_string(value)) return;
    char* text = jsc_value_to_string(value);
    if (text) App::Get().HandleUiMessage(text);
    g_free(text);
}

void RunScript(const std::string& script) {
    if (!g_view) return;
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(g_view, script.c_str(), gssize(script.size()), nullptr, nullptr, nullptr,
                                        nullptr, nullptr);
#else
    webkit_web_view_run_javascript(g_view, script.c_str(), nullptr, nullptr, nullptr);
#endif
}

// Links leave the app for the browser; only our own pages load in the window.
gboolean OnDecidePolicy(WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) {
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) return FALSE;
    WebKitNavigationAction* action =
        webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
    const char* uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    const std::string target = uri ? uri : "";
    const bool ours = target.rfind(std::string(kScheme) + ":", 0) == 0 || target.rfind("about:", 0) == 0;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION || !ours) {
        if (!ours && !target.empty()) OpenExternal(target);
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }
    return FALSE;
}

// The page's own UI has no use for the browser's menu (Back, Reload, ...); debug builds keep it
// for the inspector.
gboolean OnContextMenu(WebKitWebView*, WebKitContextMenu*, GdkEvent*, WebKitHitTestResult*, gpointer) {
#ifdef NDEBUG
    return TRUE;
#else
    return FALSE;
#endif
}

GtkWidget* CreateWebView() {
    const std::string dataDir = (DataDir() / "webkit").string();
    const std::string cacheDir = (DataDir() / "webkit-cache").string();
    WebKitWebsiteDataManager* data = webkit_website_data_manager_new("base-data-directory", dataDir.c_str(),
                                                                     "base-cache-directory", cacheDir.c_str(), nullptr);
    WebKitWebContext* context = webkit_web_context_new_with_website_data_manager(data);
    g_object_unref(data);
    webkit_web_context_register_uri_scheme(context, kScheme, OnUiRequest, nullptr, nullptr);
    // Secure and CORS-enabled: the UI is ES modules, which a plain custom scheme can't load.
    WebKitSecurityManager* security = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_secure(security, kScheme);
    webkit_security_manager_register_uri_scheme_as_cors_enabled(security, kScheme);

    WebKitUserContentManager* content = webkit_user_content_manager_new();
    WebKitUserScript* bridge = webkit_user_script_new(kBridgeScript, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(content, bridge);
    webkit_user_script_unref(bridge);
    g_signal_connect(content, "script-message-received::rincast", G_CALLBACK(OnScriptMessage), nullptr);
    webkit_user_content_manager_register_script_message_handler(content, "rincast");

    GtkWidget* view =
        GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", context, "user-content-manager", content, nullptr));
    g_object_unref(context);
    g_object_unref(content);
    WebKitSettings* settings = webkit_web_view_get_settings(WEBKIT_WEB_VIEW(view));
#ifdef NDEBUG
    webkit_settings_set_enable_developer_extras(settings, FALSE);
#else
    webkit_settings_set_enable_developer_extras(settings, TRUE);
#endif
    webkit_settings_set_enable_back_forward_navigation_gestures(settings, FALSE);
    const GdkRGBA black{0, 0, 0, 1};
    webkit_web_view_set_background_color(WEBKIT_WEB_VIEW(view), &black);
    g_signal_connect(view, "decide-policy", G_CALLBACK(OnDecidePolicy), nullptr);
    g_signal_connect(view, "context-menu", G_CALLBACK(OnContextMenu), nullptr);
    return view;
}

// Whether a panel shows StatusNotifierItems (KDE, most others; GNOME only with the AppIndicator
// extension). Without one the tray icon is invisible.
bool TrayHostAvailable() {
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (!bus) return false;
    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.kde.StatusNotifierWatcher", "/StatusNotifierWatcher", "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.kde.StatusNotifierWatcher", "IsStatusNotifierHostRegistered"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    bool available = false;
    if (reply) {
        GVariant* value = nullptr;
        g_variant_get(reply, "(v)", &value);
        if (value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) available = g_variant_get_boolean(value);
        if (value) g_variant_unref(value);
        g_variant_unref(reply);
    }
    g_object_unref(bus);
    return available;
}

void ShowMainWindow();

// Closing hides the window and keeps streaming in the background, as on Windows; the tray icon
// brings it back. Without a tray to come back from, it's minimized instead: hidden, it could only
// be reached again by starting the streamer a second time.
gboolean OnDelete(GtkWidget* window, GdkEvent*, gpointer) {
    if (g_quitting) return FALSE;
    if (!TrayHostAvailable()) {
        gtk_window_iconify(GTK_WINDOW(window));
        return TRUE;
    }
    gtk_widget_hide(window);
    if (!g_trayHintShown) {
        g_trayHintShown = true;
        GNotification* note = g_notification_new("RinCast is still running");
        g_notification_set_body(note, "Your headset can connect at any time. Quit from the tray icon.");
        g_application_send_notification(G_APPLICATION(g_app), "background", note);
        g_object_unref(note);
    }
    return TRUE;
}

void CreateMainWindow() {
    g_window = gtk_application_window_new(g_app);
    gtk_window_set_title(GTK_WINDOW(g_window), "RinCast Streamer");
    // The size of what's inside: Streaming fits without scrolling, status bar included.
    gtk_window_set_default_size(GTK_WINDOW(g_window), 900, 690);
    gtk_widget_set_size_request(g_window, 760, 560);
    gtk_window_set_position(GTK_WINDOW(g_window), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_from_file(GTK_WINDOW(g_window), (g_uiRoot / "mark.png").c_str(), nullptr);
    g_signal_connect(g_window, "delete-event", G_CALLBACK(OnDelete), nullptr);
    GtkWidget* view = CreateWebView();
    g_view = WEBKIT_WEB_VIEW(view);
    gtk_container_add(GTK_CONTAINER(g_window), view);
    gtk_widget_show(view);
    webkit_web_view_load_uri(g_view, kStartPage);
}

void ShowMainWindow() {
    if (g_quitting) return;
    if (!g_window) CreateMainWindow();
    gtk_widget_show(g_window);
    gtk_window_deiconify(GTK_WINDOW(g_window));
    gtk_window_present(GTK_WINDOW(g_window));
}

void Quit() {
    g_quitting = true;
    g_application_quit(G_APPLICATION(g_app));
}

// libayatana-appindicator3 is marked deprecated in newer releases (in favour of a GLib-only
// library not yet in every distribution); it still works, and it is what Ubuntu 24.04 and
// Debian 13 ship.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
void CreateTrayIcon() {
    g_indicator = app_indicator_new("rincast-streamer", "mark", APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    // The icon is ui/mark.png, looked up by name in that folder.
    app_indicator_set_icon_theme_path(g_indicator, g_uiRoot.c_str());
    app_indicator_set_title(g_indicator, "RinCast Streamer");
    GtkWidget* menu = gtk_menu_new();
    GtkWidget* open = gtk_menu_item_new_with_label("Open RinCast");
    g_disconnectItem = gtk_menu_item_new_with_label("Disconnect headset");
    GtkWidget* quit = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(open, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer) { ShowMainWindow(); }), nullptr);
    g_signal_connect(g_disconnectItem, "activate",
                     G_CALLBACK(+[](GtkMenuItem*, gpointer) { App::Get().HandleUiMessage(R"({"type":"disconnect"})"); }),
                     nullptr);
    g_signal_connect(quit, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer) { Quit(); }), nullptr);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), g_disconnectItem);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);
    gtk_widget_show_all(menu);
    gtk_widget_hide(g_disconnectItem);  // only while a headset is connected (OnTick)
    app_indicator_set_menu(g_indicator, GTK_MENU(menu));
    app_indicator_set_secondary_activate_target(g_indicator, open);  // middle click
    app_indicator_set_status(g_indicator, APP_INDICATOR_STATUS_ACTIVE);
}
G_GNUC_END_IGNORE_DEPRECATIONS

gboolean OnTick(gpointer) {
    App::Get().Tick();
    if (g_disconnectItem) gtk_widget_set_visible(g_disconnectItem, App::Get().Server().ActiveSession() != nullptr);
    return G_SOURCE_CONTINUE;
}

// The native folder dialog (the desktop's own through the portal where there is one), at `start`
// when that folder exists. "" = cancelled. On the GTK thread, where the UI's messages arrive.
std::string PickFolder(const std::string& start) {
    if (!g_main_context_is_owner(g_main_context_default())) return "";
    GtkFileChooserNative* dialog =
        gtk_file_chooser_native_new("Choose a folder", g_window ? GTK_WINDOW(g_window) : nullptr,
                                    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Select", "_Cancel");
    std::error_code ec;
    if (!start.empty() && std::filesystem::is_directory(start, ec))
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), start.c_str());
    std::string picked;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        if (char* file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog))) {
            picked = file;
            g_free(file);
        }
    }
    g_object_unref(dialog);
    return picked;
}

// One streamer per user also where D-Bus (and with it GApplication's uniqueness) is missing.
// Held for the process's life; false when another one holds it.
bool TakeInstanceLock() {
    std::string dir;
    if (const char* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime) dir = runtime;
    const std::string path =
        dir.empty() ? "/tmp/rincast-streamer-" + std::to_string(getuid()) + ".lock" : dir + "/rincast-streamer.lock";
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return true;  // can't tell: run
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) return true;  // the fd stays open: the lock is ours until exit
    close(fd);
    return false;
}

bool HasArg(int argc, char** argv, const char* arg) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], arg) == 0) return true;
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const char* x11 = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const bool display = (x11 && *x11) || (wayland && *wayland);
    if (HasArg(argc, argv, "--headless") || !display) return RunInTerminal(argc, argv);
    const bool minimized = HasArg(argc, argv, "--minimized");
    if (!gtk_init_check(&argc, &argv)) {
        std::fprintf(stderr, "RinCast Streamer: no display to open a window on, running in the terminal\n");
        return RunInTerminal(argc, argv);
    }
    signal(SIGPIPE, SIG_IGN);

    // A second start shows the running streamer's window and ends: GApplication passes it the
    // "activate" over D-Bus. Our own options aren't GApplication's, so it only sees argv[0].
    g_app = gtk_application_new(kAppId, GApplicationFlags(0));
    GError* error = nullptr;
    if (!g_application_register(G_APPLICATION(g_app), nullptr, &error)) {
        RC_LOGW("Window: no single-instance service (%s)", error ? error->message : "?");
        g_clear_error(&error);
    } else if (g_application_get_is_remote(G_APPLICATION(g_app))) {
        const int status = g_application_run(G_APPLICATION(g_app), 1, argv);
        g_object_unref(g_app);
        return status;
    }
    if (!TakeInstanceLock()) {
        std::fprintf(stderr, "RinCast Streamer is already running.\n");
        g_object_unref(g_app);
        return 0;
    }

    if (!PreparePlatform()) return 1;
    if (!App::Get().Init()) {
        GtkWidget* box = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                                "RinCast Streamer could not start its network service. Is another copy running?");
        gtk_window_set_title(GTK_WINDOW(box), "RinCast Streamer");
        gtk_dialog_run(GTK_DIALOG(box));
        gtk_widget_destroy(box);
        g_object_unref(g_app);
        return 1;
    }

    g_uiRoot = FindUiRoot();
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);
    // From any thread: the page is only touched on the GTK thread.
    App::Get().SetUiSink([](const std::string& json) {
        g_idle_add(
            [](gpointer data) -> gboolean {
                std::unique_ptr<std::string> text(static_cast<std::string*>(data));
                RunScript("window.chrome.webview.rincastDeliver(" + *text + ");");
                return G_SOURCE_REMOVE;
            },
            new std::string(json));
    });
    App::Get().ShowWindowRequest = [] {
        g_idle_add(
            [](gpointer) -> gboolean {
                ShowMainWindow();
                return G_SOURCE_REMOVE;
            },
            nullptr);
    };
    App::Get().FolderPicker = PickFolder;

    // "activate": the first start (here) and every later one (from the second instance).
    static bool started = false;
    static bool startMinimized = false;
    startMinimized = minimized;
    g_trayHintShown = minimized;
    g_signal_connect(g_app, "activate", G_CALLBACK(+[](GApplication*, gpointer) {
                         if (!started) {
                             started = true;
                             CreateTrayIcon();
                             if (!g_window) CreateMainWindow();
                             if (startMinimized) return;
                         }
                         ShowMainWindow();
                     }),
                     nullptr);
    // Running with the window hidden: the application doesn't end with its last visible window.
    g_application_hold(G_APPLICATION(g_app));
    g_timeout_add_seconds(1, OnTick, nullptr);
    auto onSignal = +[](gpointer) -> gboolean {
        Quit();
        return G_SOURCE_REMOVE;
    };
    g_unix_signal_add(SIGINT, onSignal, nullptr);
    g_unix_signal_add(SIGTERM, onSignal, nullptr);

    const int status = g_application_run(G_APPLICATION(g_app), 1, argv);

    // As on Windows: the UI goes first, then the service.
    App::Get().SetUiSink(nullptr);
    App::Get().ShowWindowRequest = nullptr;
    App::Get().FolderPicker = nullptr;
    if (g_window) gtk_widget_destroy(g_window);
    g_window = nullptr;
    g_view = nullptr;
    if (g_indicator) g_object_unref(g_indicator);
    App::Get().Shutdown();
    g_object_unref(g_app);
    return status;
}
