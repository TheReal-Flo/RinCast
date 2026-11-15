// xdg-desktop-portal over D-Bus (GDBus), for the ScreenCast and RemoteDesktop portals. The portals
// answer a method through the Response signal of a Request object, and they tell when a session
// ends through its Closed signal: those signals are delivered on a GLib main loop of our own, on
// a thread of its own with a GMainContext of its own (the GTK window owns the default one).
#pragma once

#include <gio/gio.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace rcs::portal {

constexpr const char* kBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kObjectPath = "/org/freedesktop/portal/desktop";

// The session bus, or null (no D-Bus session: no portals).
GDBusConnection* Bus();

// A fresh handle token ("rincast_12"), for handle_token and session_handle_token.
std::string NewToken();

// A portal interface's "version" property, 0 when the portal doesn't offer the interface.
uint32_t InterfaceVersion(const char* interface);
// Another uint32 property of the interface ("AvailableCursorModes"), or `fallback`.
uint32_t UintProperty(const char* interface, const char* property, uint32_t fallback);

// Calls `method` of `interface` with `parameters` (floating, consumed; its options must hold
// handle_token = `token`) and waits for the Request's Response, while `running` and at most
// timeoutMs. Returns the response code (0 success, 1 cancelled by the user, 2 failed) with its
// results in `results` (to g_variant_unref), or -1 when the call failed or nobody answered.
int Request(const char* interface, const char* method, GVariant* parameters, const std::string& token,
            GVariant** results, const std::atomic<bool>& running, int timeoutMs);

// Calls `method` and takes the file descriptor it returns ("(h)": OpenPipeWireRemote,
// ConnectToEIS). -1 on failure.
int CallForFd(const char* interface, const char* method, GVariant* parameters);

// Calls `onClosed` (on the portal's thread) when the session at `sessionHandle` closes. Returns a
// subscription for Unsubscribe.
unsigned SubscribeClosed(const std::string& sessionHandle, std::function<void()> onClosed);
void Unsubscribe(unsigned subscription);
// Closes the portal session (the compositor's sharing indicator goes away).
void CloseSession(const std::string& sessionHandle);

// The restore token kept in DataDir() under `name`, so the portal asks once; "" if none.
std::string LoadToken(const std::string& name);
void SaveToken(const std::string& name, const std::string& token);

}  // namespace rcs::portal
