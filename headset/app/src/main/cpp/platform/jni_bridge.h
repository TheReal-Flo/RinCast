// Services the system provides: on Android, calls into app.rincast.vr.MainActivity for those that
// have no NDK API (jni_bridge.cpp); on Linux (the Steam Frame), platform_linux.cpp.
#pragma once

#ifdef __ANDROID__
#include <jni.h>
#endif
#include <cstdint>

#include <string>
#include <vector>

struct android_app;

namespace rv {

struct HttpResult {
    int status = 0;  // 0 = network error
    std::string body;
};

class Platform {
   public:
    static void Init(android_app* app);  // null on Linux
#ifdef __ANDROID__
    static JavaVM* Vm();
    static jobject Activity();
#endif

    static std::string DeviceId();
    static std::string DeviceModel();
    static std::string FilesDir();
    static bool HasMicPermission();
    static void SetStreamingLocks(bool enable);
    enum class Sound { Click = 0, Connected = 1, Problem = 2 };
    static void PlaySound(Sound sound);
    // Asks for those that aren't granted yet; each headset family names its own.
    static void RequestPermissions(const std::vector<std::string>& permissions);
    // A USB cable to the PC as a network (Horizon OS 2.5+). The first time, the headset asks.
    static void SetUsbNetwork(bool on);
    struct UsbNetwork {
        enum State { Off, Waiting, Connected, NotAllowed } state = Off;
        uint64_t handle = 0;     // for binding sockets to it
        std::string interface;   // e.g. "usb0"
        int kbps = 0;            // USB 2: ~480000, USB 3: ~5000000
    };
    static UsbNetwork GetUsbNetwork();
    static void Speak(const std::string& text);  // needs a text-to-speech engine on the headset
    // Blocking; call from worker threads only.
    static HttpResult Http(const std::string& method, const std::string& url, const std::string& body,
                           const std::string& bearer, int timeoutMs);
};

#ifdef __ANDROID__
// Attaches the calling native thread to the JVM for the scope's lifetime.
class JniScope {
   public:
    JniScope();
    ~JniScope();
    JNIEnv* env() const { return m_env; }

   private:
    JNIEnv* m_env = nullptr;
    bool m_attached = false;
};
#endif

}  // namespace rv
