// Small WinHTTP wrapper used for the RinCast account API and router UPnP control.
#pragma once

#include <string>

#include "../util.h"

namespace rcs {

struct HttpResponse {
    int status = 0;  // 0 = network error
    std::string body;
    json Json() const;
};

// extraHeaders replaces the default JSON headers (CRLF-terminated lines); such requests
// (router UPnP control) also bypass any system proxy.
HttpResponse HttpRequest(const std::string& method, const std::string& url, const std::string& body,
                         const std::string& bearer, int timeoutMs, const std::string& extraHeaders = {});

}  // namespace rcs
