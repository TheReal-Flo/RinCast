// net/http.h on Linux and macOS, with libcurl. Same behaviour as the WinHTTP version: JSON headers unless
// extraHeaders replaces them, and those requests (router UPnP control) skip any proxy.
#include "net/http.h"

#include "rc/platform.h"

#ifdef RINCAST_HAVE_CURL
#include <curl/curl.h>
#endif

namespace rcs {

json HttpResponse::Json() const {
    try {
        return json::parse(body);
    } catch (...) {
        return json::object();
    }
}

#ifdef RINCAST_HAVE_CURL

namespace {

size_t Append(char* data, size_t size, size_t count, void* user) {
    auto* body = static_cast<std::string*>(user);
    if (body->size() > (4u << 20)) return 0;  // as on Windows: 4 MB at most
    body->append(data, size * count);
    return size * count;
}

}  // namespace

HttpResponse HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                         const std::string& bearer, int timeoutMs, const std::string& extraHeaders) {
    static const bool initialized = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    HttpResponse res;
    CURL* curl = initialized ? curl_easy_init() : nullptr;
    if (!curl) return res;
    curl_slist* headers = nullptr;
    if (extraHeaders.empty()) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json");
    } else {
        // CRLF-terminated lines, as WinHTTP takes them.
        size_t start = 0;
        while (start < extraHeaders.size()) {
            size_t end = extraHeaders.find("\r\n", start);
            if (end == std::string::npos) end = extraHeaders.size();
            if (end > start) headers = curl_slist_append(headers, extraHeaders.substr(start, end - start).c_str());
            start = end + 2;
        }
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    }
    if (!bearer.empty()) headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "RinCast-Streamer/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, long(timeoutMs));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (!body.empty()) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Append);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    if (curl_easy_perform(curl) == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        res.status = int(status);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res;
}

#else

HttpResponse HttpRequest(const std::string& method, const std::string& url, const std::string&, const std::string&,
                         int, const std::string&) {
    static bool said = false;
    if (!said) RC_LOGW("Built without libcurl: no account sign-in and no router port mapping (%s %s)", method.c_str(), url.c_str());
    said = true;
    return {};
}

#endif

}  // namespace rcs
