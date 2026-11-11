#include "jni_bridge.h"

#include <android_native_app_glue.h>

#include <algorithm>
#include <cstdio>

#include "rc/platform.h"

namespace rv {

namespace {
JavaVM* g_vm = nullptr;
jobject g_activity = nullptr;
std::string g_deviceId, g_model, g_filesDir;

std::string ToString(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out(chars ? chars : "");
    env->ReleaseStringUTFChars(s, chars);
    env->DeleteLocalRef(s);
    return out;
}

std::string CallString(const char* method) {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, method, "()Ljava/lang/String;");
    std::string out = ToString(env, static_cast<jstring>(env->CallObjectMethod(g_activity, m)));
    env->DeleteLocalRef(cls);
    return out;
}
}  // namespace

JniScope::JniScope() {
    if (g_vm->GetEnv(reinterpret_cast<void**>(&m_env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        g_vm->AttachCurrentThread(&m_env, nullptr);
        m_attached = true;
    }
}

JniScope::~JniScope() {
    if (m_attached) g_vm->DetachCurrentThread();
}

void Platform::Init(android_app* app) {
    g_vm = app->activity->vm;
    g_activity = app->activity->clazz;
    g_deviceId = CallString("getRinCastDeviceId");
    g_model = CallString("getDeviceModel");
    g_filesDir = CallString("getFilesPath");
}

JavaVM* Platform::Vm() { return g_vm; }
jobject Platform::Activity() { return g_activity; }
std::string Platform::DeviceId() { return g_deviceId; }
std::string Platform::DeviceModel() { return g_model; }
std::string Platform::FilesDir() { return g_filesDir; }
std::string Platform::SystemLocale() { return CallString("getSystemLocale"); }

bool Platform::HasMicPermission() {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "hasMicPermission", "()Z");
    const bool ok = env->CallBooleanMethod(g_activity, m);
    env->DeleteLocalRef(cls);
    return ok;
}

void Platform::SetStreamingLocks(bool enable) {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "setStreamingLocks", "(Z)V");
    env->CallVoidMethod(g_activity, m, jboolean(enable));
    env->DeleteLocalRef(cls);
}

void Platform::PlaySound(Sound sound) {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "playUiSound", "(I)V");
    env->CallVoidMethod(g_activity, m, jint(sound));
    env->DeleteLocalRef(cls);
}

void Platform::RequestPermissions(const std::vector<std::string>& permissions) {
    std::string list;
    for (const auto& p : permissions) list += (list.empty() ? "" : ",") + p;
    if (list.empty()) return;
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "requestPermissionList", "(Ljava/lang/String;)V");
    jstring s = env->NewStringUTF(list.c_str());
    env->CallVoidMethod(g_activity, m, s);
    env->DeleteLocalRef(s);
    env->DeleteLocalRef(cls);
}

void Platform::SetUsbNetwork(bool on) {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "setUsbNetwork", "(Z)V");
    env->CallVoidMethod(g_activity, m, jboolean(on));
    env->DeleteLocalRef(cls);
}

Platform::UsbNetwork Platform::GetUsbNetwork() {
    UsbNetwork u;
    const std::string s = CallString("usbNetworkState");
    unsigned long long handle = 0;
    int state = 0, kbps = 0, used = 0;
    if (std::sscanf(s.c_str(), "%d,%llu,%d,%n", &state, &handle, &kbps, &used) >= 3) {
        u.state = UsbNetwork::State(std::clamp(state, 0, 3));
        u.handle = handle;
        u.kbps = kbps;
        if (used > 0 && size_t(used) <= s.size()) u.interface = s.substr(size_t(used));
    }
    return u;
}

void Platform::Speak(const std::string& text) {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "speak", "(Ljava/lang/String;)V");
    jstring s = env->NewStringUTF(text.c_str());
    env->CallVoidMethod(g_activity, m, s);
    env->DeleteLocalRef(s);
    env->DeleteLocalRef(cls);
}

bool Platform::ScreenshotStorageOk() {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "screenshotStorageOk", "()Z");
    const jboolean ok = env->CallBooleanMethod(g_activity, m);
    env->DeleteLocalRef(cls);
    return ok;
}

void Platform::OpenStorageSettings() {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(cls, "openStorageSettings", "()V");
    env->CallVoidMethod(g_activity, m);
    env->DeleteLocalRef(cls);
}

HttpResult Platform::Http(const std::string& method, const std::string& url, const std::string& body,
                          const std::string& bearer, int timeoutMs) {
    JniScope scope;
    JNIEnv* env = scope.env();
    jclass cls = env->GetObjectClass(g_activity);
    jmethodID m = env->GetMethodID(
        cls, "httpRequest", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;I)Ljava/lang/String;");
    jstring jm = env->NewStringUTF(method.c_str());
    jstring ju = env->NewStringUTF(url.c_str());
    jstring jb = env->NewStringUTF(body.c_str());
    jstring jt = env->NewStringUTF(bearer.c_str());
    const std::string raw = ToString(env, static_cast<jstring>(env->CallObjectMethod(g_activity, m, jm, ju, jb, jt, timeoutMs)));
    env->DeleteLocalRef(jm);
    env->DeleteLocalRef(ju);
    env->DeleteLocalRef(jb);
    env->DeleteLocalRef(jt);
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return {};
    }
    HttpResult r;
    const size_t nl = raw.find('\n');
    r.status = std::atoi(raw.substr(0, nl).c_str());
    r.body = nl == std::string::npos ? "" : raw.substr(nl + 1);
    return r;
}

}  // namespace rv
