#include "http.h"

#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

namespace rcs {

json HttpResponse::Json() const {
    try {
        return json::parse(body);
    } catch (...) {
        return json::object();
    }
}

HttpResponse HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                         const std::string& bearer, int timeoutMs, const std::string& extraHeaders) {
    HttpResponse res;
    std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    wchar_t extra[1024] = {};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 1024;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return res;
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET session = WinHttpOpen(L"RinCast-Streamer/1.0",
                                    extraHeaders.empty() ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return res;
    WinHttpSetTimeouts(session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    HINTERNET request = nullptr;
    if (connect) {
        const std::wstring fullPath = std::wstring(path) + extra;
        request = WinHttpOpenRequest(connect, Widen(method).c_str(), fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    }
    if (request) {
        std::wstring headers = extraHeaders.empty() ? std::wstring(L"Content-Type: application/json\r\nAccept: application/json\r\n")
                                                    : Widen(extraHeaders);
        if (!bearer.empty()) headers += L"Authorization: Bearer " + Widen(bearer) + L"\r\n";
        BOOL ok = WinHttpSendRequest(request, headers.c_str(), DWORD(-1), body.empty() ? nullptr : (LPVOID)body.data(),
                                     DWORD(body.size()), DWORD(body.size()), 0);
        if (ok) ok = WinHttpReceiveResponse(request, nullptr);
        if (ok) {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &size,
                                nullptr);
            res.status = int(status);
            for (;;) {
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), avail, &read)) break;
                res.body.append(chunk.data(), read);
                if (res.body.size() > (4u << 20)) break;
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return res;
}

}  // namespace rcs
