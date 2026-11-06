#include "port_mapper.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <cstring>
#include <vector>

#include "http.h"
#include "rc/crypto.h"
#include "rc/platform.h"

#pragma comment(lib, "iphlpapi.lib")

namespace rcs {

namespace {

constexpr uint16_t kPcpPort = 5351;  // shared by PCP and NAT-PMP
constexpr uint32_t kLifetime = 3600;

bool IsPublicV4(uint32_t ip) {
    const uint8_t a = uint8_t(ip >> 24);
    return ip != 0 && !rc::IsPrivateLan(ip) && !rc::IsCgnat(ip) && a != 127 && a != 0 && (ip >> 16) != 0xA9FE &&
           a < 224;
}

void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}
void Put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
uint16_t Get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t Get32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
void PutV4Mapped(uint8_t* p, uint32_t ip) {
    std::memset(p, 0, 10);
    p[10] = p[11] = 0xFF;
    Put32(p + 12, ip);
}

// One request/response round trip to the gateway with RFC-style retransmission.
int Exchange(uint32_t gateway, const uint8_t* req, size_t reqSize, uint8_t* resp, size_t cap, uint8_t expectOp) {
    rc::UdpSocket s;
    if (!s.Open(0, true)) return 0;  // IPv4 socket
    const rc::NetAddress gw{gateway, kPcpPort};
    int timeout = 250;
    for (int attempt = 0; attempt < 4; attempt++, timeout *= 2) {
        s.SendTo(gw, req, reqSize);
        const int64_t deadline = rc::NowNs() + int64_t(timeout) * 1'000'000;
        while (rc::NowNs() < deadline) {
            rc::NetAddress from;
            const int n = s.RecvFrom(resp, cap, from, timeout);
            if (n >= 4 && from.ip == gateway && resp[1] == expectOp) return n;
            if (n > 0 && from.ip == gateway && resp[0] != req[0]) return -1;  // other protocol version: unsupported
        }
    }
    return 0;
}

std::string Between(const std::string& s, const std::string& open, const std::string& close, size_t from = 0,
                    size_t* end = nullptr) {
    const size_t a = s.find(open, from);
    if (a == std::string::npos) return {};
    const size_t b = s.find(close, a + open.size());
    if (b == std::string::npos) return {};
    if (end) *end = b + close.size();
    return s.substr(a + open.size(), b - a - open.size());
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

void PortMapper::Start(uint16_t internalPort) {
    if (m_running.exchange(true)) return;
    m_internalPort = internalPort;
    rc::RandomBytes(m_pcpNonce, sizeof(m_pcpNonce));
    m_thread = std::thread([this] { Run(); });
}

void PortMapper::Stop() {
    if (!m_running.exchange(false)) return;
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

bool PortMapper::Mapped(rc::NetAddress& out) const {
    std::lock_guard lock(m_mutex);
    if (m_method.empty() || !m_external.valid()) return false;
    out = m_external;
    return true;
}

std::string PortMapper::Status() const {
    std::lock_guard lock(m_mutex);
    return m_status;
}

bool PortMapper::Discover() {
    MIB_IPFORWARDROW row{};
    IPAddr probe = 0;
    inet_pton(AF_INET, "1.1.1.1", &probe);
    if (GetBestRoute(probe, 0, &row) != NO_ERROR || row.dwForwardNextHop == 0) return false;
    const uint32_t gateway = ntohl(row.dwForwardNextHop);
    // Local address on the interface that carries the default route.
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    }
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size) != NO_ERROR) return false;
    uint32_t local = 0;
    for (auto* a = adapters; a && !local; a = a->Next) {
        if (a->IfIndex != row.dwForwardIfIndex) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                local = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
                break;
            }
    }
    // Only a router on our LAN can map ports for us (not a VPN tunnel's next hop).
    if (!local || !rc::IsPrivateLan(local) || !rc::IsPrivateLan(gateway)) return false;
    std::lock_guard lock(m_mutex);
    m_gateway = gateway;
    m_localIp = local;
    return true;
}

// --- PCP (RFC 6887) -----------------------------------------------------------

bool PortMapper::MapPcp(uint32_t lifetime) {
    uint8_t req[60] = {};
    req[0] = 2;  // version
    req[1] = 1;  // MAP request
    Put32(req + 4, lifetime);
    PutV4Mapped(req + 8, m_localIp);
    std::memcpy(req + 24, m_pcpNonce, 12);
    req[36] = 17;  // UDP
    Put16(req + 40, m_internalPort);
    Put16(req + 42, m_internalPort);  // suggest the same external port
    PutV4Mapped(req + 44, 0);
    uint8_t resp[1100];
    const int n = Exchange(m_gateway, req, sizeof(req), resp, sizeof(resp), 0x81);
    if (n < 60 || resp[3] != 0 || std::memcmp(resp + 24, m_pcpNonce, 12) != 0) return false;
    if (lifetime == 0) return true;
    rc::NetAddress ext{Get32(resp + 56), Get16(resp + 42)};
    std::lock_guard lock(m_mutex);
    m_external = ext;
    m_method = "PCP";
    return true;
}

// --- NAT-PMP (RFC 6886) -------------------------------------------------------

bool PortMapper::MapNatPmp(uint32_t lifetime) {
    uint8_t resp[64];
    uint32_t externalIp = 0;
    if (lifetime) {
        const uint8_t addrReq[2] = {0, 0};
        const int n = Exchange(m_gateway, addrReq, sizeof(addrReq), resp, sizeof(resp), 128);
        if (n < 12 || Get16(resp + 2) != 0) return false;
        externalIp = Get32(resp + 8);
    }
    uint8_t req[12] = {};
    req[1] = 1;  // map UDP
    Put16(req + 4, m_internalPort);
    Put16(req + 6, lifetime ? m_internalPort : 0);
    Put32(req + 8, lifetime);
    const int n = Exchange(m_gateway, req, sizeof(req), resp, sizeof(resp), 129);
    if (n < 16 || Get16(resp + 2) != 0) return false;
    if (lifetime == 0) return true;
    std::lock_guard lock(m_mutex);
    m_external = rc::NetAddress{externalIp, Get16(resp + 10)};
    m_method = "NAT-PMP";
    return true;
}

// --- UPnP IGD -------------------------------------------------------------------

bool PortMapper::MapUpnp(uint32_t lifetime) {
    if (m_upnpControlUrl.empty()) {
        // SSDP search on the LAN interface.
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return false;
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(m_localIp);
        bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));
        in_addr ifaddr{};
        ifaddr.s_addr = htonl(m_localIp);
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&ifaddr), sizeof(ifaddr));
        sockaddr_in ssdp{};
        ssdp.sin_family = AF_INET;
        ssdp.sin_port = htons(1900);
        inet_pton(AF_INET, "239.255.255.250", &ssdp.sin_addr);
        for (const char* st : {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                               "urn:schemas-upnp-org:device:InternetGatewayDevice:2"}) {
            const std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n") +
                                    "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: " + st + "\r\n\r\n";
            sendto(s, msg.data(), int(msg.size()), 0, reinterpret_cast<sockaddr*>(&ssdp), sizeof(ssdp));
        }
        std::vector<std::string> locations;
        const int64_t deadline = rc::NowNs() + 2'500'000'000LL;
        char buf[2048];
        while (rc::NowNs() < deadline) {
            WSAPOLLFD pfd{s, POLLRDNORM, 0};
            if (WSAPoll(&pfd, 1, 200) <= 0) continue;
            const int n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n <= 0) continue;
            buf[n] = 0;
            const std::string reply(buf);
            const std::string low = Lower(reply);
            const size_t at = low.find("\nlocation:");
            if (at == std::string::npos) continue;
            size_t b = at + 10, e = reply.find('\r', b);
            if (e == std::string::npos) e = reply.find('\n', b);
            std::string loc = reply.substr(b, e - b);
            while (!loc.empty() && loc.front() == ' ') loc.erase(loc.begin());
            if (!loc.empty()) locations.push_back(loc);
        }
        closesocket(s);
        for (const std::string& loc : locations) {
            const HttpResponse desc = HttpRequest("GET", loc, "", "", 3000, "Accept: text/xml\r\n");
            if (desc.status != 200) continue;
            // First WAN connection service with a control URL.
            for (const char* svc : {"urn:schemas-upnp-org:service:WANIPConnection:2",
                                    "urn:schemas-upnp-org:service:WANIPConnection:1",
                                    "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
                const size_t at = desc.body.find(std::string("<serviceType>") + svc + "</serviceType>");
                if (at == std::string::npos) continue;
                std::string control = Between(desc.body, "<controlURL>", "</controlURL>", at);
                if (control.empty()) continue;
                if (control.rfind("http", 0) != 0) {
                    const size_t hostEnd = loc.find('/', loc.find("://") + 3);
                    control = loc.substr(0, hostEnd) + (control.front() == '/' ? "" : "/") + control;
                }
                m_upnpControlUrl = control;
                m_upnpService = svc;
                break;
            }
            if (!m_upnpControlUrl.empty()) break;
        }
        if (m_upnpControlUrl.empty()) return false;
    }
    auto soap = [&](const std::string& action, const std::string& args) {
        const std::string body = "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                                 "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                                 action + " xmlns:u=\"" + m_upnpService + "\">" + args + "</u:" + action +
                                 "></s:Body></s:Envelope>";
        const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + m_upnpService +
                                    "#" + action + "\"\r\n";
        return HttpRequest("POST", m_upnpControlUrl, body, "", 4000, headers);
    };
    const std::string port = std::to_string(m_internalPort);
    if (lifetime == 0) {
        soap("DeletePortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + port +
                                      "</NewExternalPort><NewProtocol>UDP</NewProtocol>");
        return true;
    }
    in_addr la{};
    la.s_addr = htonl(m_localIp);
    char localStr[32];
    inet_ntop(AF_INET, &la, localStr, sizeof(localStr));
    auto add = [&](uint32_t lease) {
        return soap("AddPortMapping", "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + port +
                                          "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + port +
                                          "</NewInternalPort><NewInternalClient>" + localStr +
                                          "</NewInternalClient><NewEnabled>1</NewEnabled>"
                                          "<NewPortMappingDescription>RinCast</NewPortMappingDescription>"
                                          "<NewLeaseDuration>" +
                                          std::to_string(lease) + "</NewLeaseDuration>");
    };
    // An empty external address means the router has no public IPv4 of its own (DS-Lite
    // or another carrier NAT): a mapping could never be reached, so do not even add one.
    const HttpResponse ip = soap("GetExternalIPAddress", "");
    rc::NetAddress ext;
    if (ip.status != 200 || !rc::NetAddress::Parse(Between(ip.body, "<NewExternalIPAddress>", "</NewExternalIPAddress>"),
                                                    m_internalPort, ext)) {
        m_reason = "the router has no public IPv4 address (DS-Lite / carrier NAT); IPv6 is used instead";
        return false;
    }
    HttpResponse r = add(m_upnpPermanent ? 0 : lifetime);
    if (r.status != 200 && r.body.find("<errorCode>725</errorCode>") != std::string::npos) {  // only permanent leases
        m_upnpPermanent = true;
        r = add(0);
    }
    if (r.status != 200) {
        m_reason = r.body.find("<errorCode>606</errorCode>") != std::string::npos
                       ? "the router refuses automatic port forwarding for this PC (FRITZ!Box: Internet > Permit "
                         "Access > this PC > allow independent port sharing)"
                       : "the router rejected the UPnP port mapping";
        return false;
    }
    std::lock_guard lock(m_mutex);
    m_external = ext;
    m_method = "UPnP";
    return true;
}

void PortMapper::Unmap() {
    std::string method;
    {
        std::lock_guard lock(m_mutex);
        method = m_method;
        m_method.clear();
        m_external = {};
    }
    if (method == "PCP") MapPcp(0);
    else if (method == "NAT-PMP") MapNatPmp(0);
    else if (method == "UPnP") MapUpnp(0);
}

void PortMapper::Run() {
    int64_t retryNs = 5'000'000'000LL;
    while (m_running) {
        bool ok = Discover();
        std::string method;
        if (ok) {
            std::string current;
            {
                std::lock_guard lock(m_mutex);
                current = m_method;
            }
            // Renew with the protocol that worked; otherwise try them in order.
            ok = (current == "PCP" && MapPcp(kLifetime)) || (current == "NAT-PMP" && MapNatPmp(kLifetime)) ||
                 (current == "UPnP" && MapUpnp(kLifetime));
            m_reason.clear();
            if (!ok) ok = MapPcp(kLifetime) || MapNatPmp(kLifetime) || MapUpnp(kLifetime);
        }
        int64_t waitNs;
        {
            std::lock_guard lock(m_mutex);
            if (!ok) {
                m_method.clear();
                m_external = {};
                m_status = "unavailable: " + (m_reason.empty() ? std::string("the router offers no PCP, NAT-PMP or UPnP port mapping") : m_reason);
                waitNs = retryNs;
                retryNs = std::min<int64_t>(retryNs * 2, 600'000'000'000LL);  // back off to 10 min
            } else if (!IsPublicV4(m_external.ip)) {
                // The router itself sits behind a carrier NAT: the mapping can't be reached from outside.
                m_status = m_method + " mapping on a carrier-NAT router (" + m_external.ToString() + "), not usable";
                m_external = {};
                waitNs = 1800'000'000'000LL;
            } else {
                m_status = m_method + " " + m_external.ToString();
                waitNs = int64_t(kLifetime / 2) * 1'000'000'000LL;
                retryNs = 5'000'000'000LL;
            }
            method = m_status;
        }
        static std::string lastLogged;
        if (method != lastLogged) {
            RC_LOGI("Router port mapping: %s", method.c_str());
            lastLogged = method;
        }
        std::unique_lock lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::nanoseconds(waitNs), [&] { return !m_running; });
    }
    Unmap();
}

}  // namespace rcs
