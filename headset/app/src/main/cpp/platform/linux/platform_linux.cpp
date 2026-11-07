// Platform services on Linux (the Steam Frame, running SteamOS). What Android's activity provides
// comes from files and libcurl here; what only Android has (its UI sounds, text to speech, the
// USB network, runtime permissions) is left out.
#include "../jni_bridge.h"

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>

#include "rc/platform.h"

namespace rv {

namespace {

std::string g_filesDir, g_deviceId, g_model;

std::string ReadLine(const char* path) {
    std::ifstream f(path);
    std::string line;
    std::getline(f, line);
    while (!line.empty() && (line.back() == '\0' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
    return line;
}

void MakeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0700);
}

// Steam starts the program with nowhere obvious for its output, so the log also goes to a file:
// rincast.log next to the settings, with the previous run's as rincast.previous.log.
std::FILE* g_log = nullptr;
std::mutex g_logMutex;

void LogSink(rc::LogLevel level, const char* message) {
    static const char* names[] = {"D", "I", "W", "E"};
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
    std::lock_guard lock(g_logMutex);
    std::fprintf(stderr, "[%s] %s\n", names[int(level)], message);
    if (g_log) {
        std::fprintf(g_log, "%s %s %s\n", stamp, names[int(level)], message);
        std::fflush(g_log);
    }
}

size_t Collect(char* data, size_t size, size_t count, void* user) {
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

}  // namespace

void Platform::Init(android_app*) {
    // $XDG_DATA_HOME/rincast, normally ~/.local/share/rincast.
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) g_filesDir = std::string(xdg) + "/rincast";
    else g_filesDir = std::string(std::getenv("HOME") ? std::getenv("HOME") : "/tmp") + "/.local/share/rincast";
    MakeDirs(g_filesDir);
    std::rename((g_filesDir + "/rincast.log").c_str(), (g_filesDir + "/rincast.previous.log").c_str());
    g_log = std::fopen((g_filesDir + "/rincast.log").c_str(), "w");
    rc::SetLogSink(LogSink);
    RC_LOGI("RinCast %s for Linux", RINCAST_VERSION_STRING);

    // A random ID made once, like the Android app's: it names this headset to the PC, nothing more.
    const std::string idPath = g_filesDir + "/device-id";
    g_deviceId = ReadLine(idPath.c_str());
    if (g_deviceId.size() != 32) {
        uint8_t raw[16];
        rc::RandomBytes(raw, sizeof(raw));
        g_deviceId = rc::HexEncode(raw, sizeof(raw));
        std::ofstream(idPath) << g_deviceId << "\n";
    }

    // The device tree names the hardware ("Valve Steam Frame" or similar); else the PC's DMI name.
    g_model = ReadLine("/sys/firmware/devicetree/base/model");
    if (g_model.empty()) g_model = ReadLine("/sys/devices/virtual/dmi/id/product_name");
    if (g_model.empty()) g_model = "Linux headset";
    curl_global_init(CURL_GLOBAL_DEFAULT);
    RC_LOGI("Linux platform: %s, files in %s", g_model.c_str(), g_filesDir.c_str());
}

std::string Platform::DeviceId() { return g_deviceId; }
std::string Platform::DeviceModel() { return g_model; }
std::string Platform::FilesDir() { return g_filesDir; }
// The language from the environment as a BCP 47 tag, like Android gives it: de_DE.UTF-8 -> de-DE,
// zh_TW -> zh-TW. Empty (and so English) for C and POSIX.
std::string Platform::SystemLocale() {
    const char* vars[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    for (const char* v : vars) {
        const char* value = std::getenv(v);
        if (!value || !*value) continue;
        std::string tag(value);
        tag = tag.substr(0, tag.find_first_of(".@"));
        if (tag == "C" || tag == "POSIX") return "";
        for (char& c : tag)
            if (c == '_') c = '-';
        return tag;
    }
    return "";
}
bool Platform::HasMicPermission() { return true; }  // no runtime permissions on SteamOS
void Platform::SetStreamingLocks(bool) {}
void Platform::PlaySound(Sound) {}
void Platform::RequestPermissions(const std::vector<std::string>&) {}
void Platform::SetUsbNetwork(bool) {}
Platform::UsbNetwork Platform::GetUsbNetwork() { return {}; }
void Platform::Speak(const std::string&) {}

HttpResult Platform::Http(const std::string& method, const std::string& url, const std::string& body,
                          const std::string& bearer, int timeoutMs) {
    HttpResult result;
    CURL* curl = curl_easy_init();
    if (!curl) return result;
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "User-Agent: RinCast-Headset/1.0");
    if (!body.empty()) headers = curl_slist_append(headers, "Content-Type: application/json");
    const std::string auth = "Authorization: Bearer " + bearer;
    if (!bearer.empty()) headers = curl_slist_append(headers, auth.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (!body.empty() || method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, long(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    if (curl_easy_perform(curl) == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        result.status = int(status);
    } else {
        result.body.clear();
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return result;
}

}  // namespace rv
