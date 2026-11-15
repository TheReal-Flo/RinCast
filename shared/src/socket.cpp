#include "rc/socket.h"

#include <cctype>
#include <cstring>

#include "rc/platform.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#ifndef IPV6_ADD_MEMBERSHIP
#ifdef IPV6_JOIN_GROUP
#define IPV6_ADD_MEMBERSHIP IPV6_JOIN_GROUP
#endif
#endif
#endif
#ifdef __ANDROID__
#include <android/multinetwork.h>
#endif

namespace rc {

#ifdef _WIN32
static constexpr uintptr_t kInvalid = ~uintptr_t(0);
#define RC_SOCK(s) static_cast<SOCKET>(s)
#else
static constexpr int kInvalid = -1;
#define RC_SOCK(s) (s)
#endif

void NetInit() {
#ifdef _WIN32
    static bool initialized = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)initialized;
#endif
}

// ---------------------------------------------------------------------------
// NetAddress
// ---------------------------------------------------------------------------

static const uint8_t kV4MappedPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

bool NetAddress::valid() const {
    if (port == 0) return false;
    if (!v6) return ip != 0;
    for (uint8_t b : ip6)
        if (b) return true;
    return false;
}

bool NetAddress::operator==(const NetAddress& o) const {
    if (v6 != o.v6 || port != o.port) return false;
    return v6 ? std::memcmp(ip6, o.ip6, 16) == 0 && scope == o.scope : ip == o.ip;
}

bool NetAddress::IsGlobalV6() const { return v6 && (ip6[0] & 0xE0) == 0x20; }

bool NetAddress::IsLinkLocalV6() const { return v6 && ip6[0] == 0xFE && (ip6[1] & 0xC0) == 0x80; }

NetAddress NetAddress::AllNodesV6(uint16_t p, uint32_t scope) {
    NetAddress a;
    a.v6 = true;
    a.ip6[0] = 0xFF;
    a.ip6[1] = 0x02;
    a.ip6[15] = 0x01;
    a.port = p;
    a.scope = scope;
    return a;
}

std::string NetAddress::IpString() const {
    if (v6) {
        char buf[INET6_ADDRSTRLEN] = {};
        in6_addr a{};
        std::memcpy(&a, ip6, 16);
        inet_ntop(AF_INET6, &a, buf, sizeof(buf));
        return buf;
    }
    return StrFormat("%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

std::string NetAddress::ToString() const {
    if (!v6) return IpString() + ":" + std::to_string(port);
    return "[" + IpString() + (scope ? "%" + std::to_string(scope) : "") + "]:" + std::to_string(port);
}

static bool FromSockaddr(const sockaddr* sa, NetAddress& out) {
    if (sa->sa_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(sa);
        out = {};
        out.ip = ntohl(a->sin_addr.s_addr);
        out.port = ntohs(a->sin_port);
        return true;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(sa);
        out = {};
        out.port = ntohs(a->sin6_port);
        const auto* b = reinterpret_cast<const uint8_t*>(&a->sin6_addr);
        if (std::memcmp(b, kV4MappedPrefix, 12) == 0) {  // IPv4 seen through a dual-stack socket
            out.ip = (uint32_t(b[12]) << 24) | (uint32_t(b[13]) << 16) | (uint32_t(b[14]) << 8) | b[15];
        } else {
            out.v6 = true;
            std::memcpy(out.ip6, b, 16);
            if (out.IsLinkLocalV6()) out.scope = a->sin6_scope_id;
        }
        return true;
    }
    return false;
}

bool NetAddress::Parse(const std::string& host, uint16_t p, NetAddress& out) {
    NetInit();
    std::string h = host;
    if (h.size() > 2 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    if (h.empty()) return false;  // getaddrinfo("") would resolve to this machine
    in_addr a4{};
    if (inet_pton(AF_INET, h.c_str(), &a4) == 1) {
        out = {};
        out.ip = ntohl(a4.s_addr);
        out.port = p;
        return true;
    }
    in6_addr a6{};
    if (inet_pton(AF_INET6, h.c_str(), &a6) == 1) {
        out = {};
        out.v6 = true;
        std::memcpy(out.ip6, &a6, 16);
        out.port = p;
        return true;
    }
    const auto all = ResolveAll(h, p);
    for (const auto& a : all)
        if (!a.v6) {
            out = a;
            return true;
        }
    if (!all.empty()) {
        out = all.front();
        return true;
    }
    return false;
}

bool NetAddress::ParseEndpoint(const std::string& hostPort, NetAddress& out) {
    const auto colon = hostPort.rfind(':');
    if (colon == std::string::npos || colon + 1 >= hostPort.size()) return false;
    std::string host = hostPort.substr(0, colon);
    // A bare IPv6 literal without brackets has more than one colon and no port.
    if (host.find(':') != std::string::npos && !(host.front() == '[' && host.back() == ']')) return false;
    int port = 0;
    for (size_t i = colon + 1; i < hostPort.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(hostPort[i]))) return false;
        port = port * 10 + (hostPort[i] - '0');
        if (port > 65535) return false;
    }
    return port > 0 && Parse(host, uint16_t(port), out);
}

std::vector<NetAddress> NetAddress::ResolveAll(const std::string& host, uint16_t p) {
    NetInit();
    std::vector<NetAddress> out;
    if (host.empty()) return out;
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0) return out;
    for (auto* r = result; r; r = r->ai_next) {
        NetAddress a;
        if (!FromSockaddr(r->ai_addr, a)) continue;
        a.port = p;
        bool dup = false;
        for (const auto& b : out) dup |= b == a;
        if (!dup && a.valid()) out.push_back(a);
    }
    freeaddrinfo(result);
    return out;
}

NetAddress NetAddress::Broadcast(uint16_t p) { return NetAddress{0xFFFFFFFFu, p}; }

// ---------------------------------------------------------------------------
// Interfaces
// ---------------------------------------------------------------------------

bool IsPrivateLan(uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 20) == ((172u << 4) | 1) || (ip >> 16) == ((192u << 8) | 168);
}

bool IsCgnat(uint32_t ip) { return (ip >> 22) == ((100u << 2) | 1); }  // 100.64.0.0/10 (Tailscale etc.)

static bool LooksVirtual(const std::string& text) {
    std::string t = text;
    for (char& ch : t) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    // Long names match anywhere; short ones (tun0, wg0, "VPN") only at the start of a word.
    for (const char* k : {"tailscale", "wireguard", "nordlynx", "wintun", "cloudflare", "zerotier", "hamachi", "openvpn",
                          "tap-windows", "hyper-v", "vethernet", "vmware", "virtualbox", "docker"})
        if (t.find(k) != std::string::npos) return true;
    for (const char* k : {"tun", "tap", "wg", "vpn", "wsl", "warp", "utun", "ppp"}) {
        for (size_t p = t.find(k); p != std::string::npos; p = t.find(k, p + 1))
            if (p == 0 || !std::isalnum(static_cast<unsigned char>(t[p - 1]))) return true;
    }
    return false;
}

static bool LooksLikeVm(const std::string& text) {
    std::string t = text;
    for (char& ch : t) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* k : {"hyper-v", "vethernet", "vmware", "virtualbox", "docker", "wsl"})
        if (t.find(k) != std::string::npos) return true;
    return false;
}

static bool IsLinkLocal(uint32_t ip) { return (ip >> 16) == 0xA9FE; }  // 169.254/16: an adapter with no real address

#ifdef _WIN32
// Calls fn(adapter, isVirtual) for each up, non-loopback adapter of `family`.
template <typename Fn>
static void ForEachAdapter(ULONG family, Fn fn) {
    NetInit();
    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(family, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    }
    if (GetAdaptersAddresses(family, flags, nullptr, adapters, &size) != NO_ERROR) return;
    for (auto* a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        char name[256], desc[256];
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name), nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof(desc), nullptr, nullptr);
        const bool isVirtual = LooksVirtual(name) || LooksVirtual(desc) ||
                               (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211);
        fn(a, std::string(name), isVirtual);
    }
}
#endif

bool RoutesThroughTunnel(uint32_t ip) {
    if (IsCgnat(ip)) return true;
    NetInit();
    // Connecting a UDP socket sends nothing: it only asks the system which address it would use.
    const auto s = socket(AF_INET, SOCK_DGRAM, 0);
    uint32_t local = 0;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(9);
    to.sin_addr.s_addr = htonl(ip);
    sockaddr_in self{};
#ifdef _WIN32
    int size = sizeof(self);
    if (s != INVALID_SOCKET) {
#else
    socklen_t size = sizeof(self);
    if (s >= 0) {
#endif
        if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0 && getsockname(s, reinterpret_cast<sockaddr*>(&self), &size) == 0)
            local = ntohl(self.sin_addr.s_addr);
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
    }
    if (!local) return false;
    // Android can hide the adapter list, but an address like this gives it away by itself.
    if (IsCgnat(local)) return true;
    for (const auto& itf : ListInterfaces())
        if (itf.ip == local) return itf.isVirtual;
    return false;
}

std::vector<uint32_t> SubnetHosts(uint32_t ip, uint32_t netmask, size_t maxHosts) {
    std::vector<uint32_t> out;
    const uint32_t network = ip & netmask, last = network | ~netmask;
    // Both ends are excluded: they're the network's own address and its broadcast.
    if (last - network < 2) return out;
    for (uint32_t d = 1; out.size() < maxHosts; d++) {
        const bool below = ip - network > d, above = last - ip > d;
        if (!below && !above) break;
        if (below) out.push_back(ip - d);
        if (above && out.size() < maxHosts) out.push_back(ip + d);
    }
    return out;
}

std::vector<NetInterface> ListInterfaces() {
    std::vector<NetInterface> out;
#ifdef _WIN32
    ForEachAdapter(AF_INET, [&](IP_ADAPTER_ADDRESSES* a, const std::string& name, bool isVirtual) {
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            NetInterface ni;
            ni.name = name;
            ni.ip = ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr);
            ni.isVirtual = isVirtual || IsCgnat(ni.ip);
            // The friendly name can be just "Ethernet 3", so the description counts too.
            char desc[256];
            WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof(desc), nullptr, nullptr);
            ni.isVpn = ni.isVirtual && !LooksLikeVm(name) && !LooksLikeVm(desc) && !IsLinkLocal(ni.ip);
            const ULONG prefix = u->OnLinkPrefixLength;
            ni.netmask = prefix == 0 ? 0 : (0xFFFFFFFFu << (32 - prefix));
            out.push_back(ni);
        }
    });
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (auto* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK)) continue;
        NetInterface ni;
        ni.name = it->ifa_name;
        ni.ip = ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr);
        ni.isVirtual = IsCgnat(ni.ip) || LooksVirtual(ni.name);
        ni.isVpn = ni.isVirtual && !LooksLikeVm(ni.name) && !IsLinkLocal(ni.ip);
        ni.netmask = it->ifa_netmask ? ntohl(reinterpret_cast<sockaddr_in*>(it->ifa_netmask)->sin_addr.s_addr)
                                     : 0xFFFFFF00u;
        out.push_back(ni);
    }
    freeifaddrs(list);
#endif
    return out;
}

std::vector<NetInterfaceV6> ListInterfacesV6() {
    std::vector<NetInterfaceV6> out;
    // A Quest's USB networking shows up as Windows' inbox CDC-NCM driver, or as an NCM gadget.
    auto isNcm = [](std::string t) {
        for (char& ch : t) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return t.find("usbncm") != std::string::npos || t.find("ncm") != std::string::npos;
    };
#ifdef _WIN32
    ForEachAdapter(AF_INET6, [&](IP_ADAPTER_ADDRESSES* a, const std::string& name, bool) {
        if (!a->Ipv6IfIndex) return;
        NetInterfaceV6 ni;
        ni.name = name;
        char desc[256];
        WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof(desc), nullptr, nullptr);
        ni.description = desc;
        ni.adapterId = a->AdapterName ? a->AdapterName : "";
        ni.index = a->Ipv6IfIndex;
        ni.usbNcm = isNcm(ni.description);
        out.push_back(ni);
    });
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (auto* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET6) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK) || (it->ifa_flags & IFF_POINTOPOINT)) continue;
        const uint32_t index = if_nametoindex(it->ifa_name);
        bool seen = false;
        for (const auto& o : out) seen = seen || o.index == index;
        if (!index || seen) continue;
        NetInterfaceV6 ni;
        ni.name = it->ifa_name;
        ni.index = index;
        ni.usbNcm = isNcm(ni.name) || ni.name.rfind("usb", 0) == 0;
        out.push_back(ni);
    }
    freeifaddrs(list);
#endif
    return out;
}

std::vector<NetAddress> ListGlobalIpv6(uint16_t port) {
    std::vector<NetAddress> out;
    auto add = [&](const sockaddr* sa) {
        NetAddress a;
        if (!FromSockaddr(sa, a) || !a.IsGlobalV6()) return;
        a.port = port;
        for (const auto& b : out)
            if (b == a) return;
        out.push_back(a);
    };
#ifdef _WIN32
    ForEachAdapter(AF_INET6, [&](IP_ADAPTER_ADDRESSES* a, const std::string&, bool isVirtual) {
        if (isVirtual) return;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            if (u->Address.lpSockaddr->sa_family == AF_INET6 && u->DadState == IpDadStatePreferred)
                add(u->Address.lpSockaddr);
    });
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (auto* it = list; it; it = it->ifa_next) {
        if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET6) continue;
        if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK) || LooksVirtual(it->ifa_name)) continue;
        add(it->ifa_addr);
    }
    freeifaddrs(list);
#endif
    return out;
}

// ---------------------------------------------------------------------------
// UdpSocket
// ---------------------------------------------------------------------------

UdpSocket::~UdpSocket() { Close(); }

bool UdpSocket::Open(uint16_t port, bool broadcast, bool reuse) {
    NetInit();
    Close();
    // Dual-stack (one IPv6 socket that also carries IPv4 as ::ffff:a.b.c.d) unless
    // broadcasting, which is IPv4-only; plain IPv4 if the system has no IPv6.
    auto s = broadcast ? socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP) : socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    m_dualStack = !broadcast;
#ifdef _WIN32
    if (s == INVALID_SOCKET && m_dualStack) {
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        m_dualStack = false;
    }
    if (s == INVALID_SOCKET) return false;
    m_socket = static_cast<uintptr_t>(s);
    // Without this, an ICMP port unreachable makes the next recvfrom fail with WSAECONNRESET.
    BOOL newBehavior = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &newBehavior, sizeof(newBehavior), nullptr, 0, &bytes,
             nullptr, nullptr);
#else
    if (s < 0 && m_dualStack) {
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        m_dualStack = false;
    }
    if (s < 0) return false;
    m_socket = s;
#endif
    int one = 1, zero = 0;
    if (reuse) setsockopt(RC_SOCK(m_socket), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    if (broadcast)
        setsockopt(RC_SOCK(m_socket), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
    int bound;
    if (m_dualStack) {
        setsockopt(RC_SOCK(m_socket), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&zero), sizeof(zero));
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = htons(port);
        bound = bind(RC_SOCK(m_socket), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        bound = bind(RC_SOCK(m_socket), reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    }
    if (bound != 0) {
        RC_LOGW("UDP bind to port %u failed", port);
        Close();
        return false;
    }
    return true;
}

bool UdpSocket::OpenV6(uint16_t port, bool reuse) {
    NetInit();
    Close();
    auto s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return false;
    m_socket = static_cast<uintptr_t>(s);
    BOOL newBehavior = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &newBehavior, sizeof(newBehavior), nullptr, 0, &bytes,
             nullptr, nullptr);
#else
    if (s < 0) return false;
    m_socket = s;
#endif
    m_dualStack = false;
    m_v6Only = true;
    int one = 1;
    if (reuse) setsockopt(RC_SOCK(m_socket), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    setsockopt(RC_SOCK(m_socket), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);
    if (bind(RC_SOCK(m_socket), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        RC_LOGW("UDP bind to [::]:%u failed", port);
        Close();
        return false;
    }
    return true;
}

bool UdpSocket::JoinGroupV6(const NetAddress& group, uint32_t ifIndex) {
    if (!IsOpen() || !group.v6) return false;
    ipv6_mreq m{};
    std::memcpy(&m.ipv6mr_multiaddr, group.ip6, 16);
    m.ipv6mr_interface = ifIndex;
    return setsockopt(RC_SOCK(m_socket), IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&m), sizeof(m)) == 0;
}

#ifdef __ANDROID__
bool UdpSocket::BindToNetwork(uint64_t networkHandle) {
    return IsOpen() && android_setsocknetwork(net_handle_t(networkHandle), m_socket) == 0;
}
#endif

void UdpSocket::Close() {
    if (m_socket == kInvalid) return;
#ifdef _WIN32
    closesocket(RC_SOCK(m_socket));
#else
    shutdown(m_socket, SHUT_RDWR);
    close(m_socket);
#endif
    m_socket = kInvalid;
    m_v6Only = false;
}

bool UdpSocket::IsOpen() const { return m_socket != kInvalid; }

void UdpSocket::SetBuffers(int sendBytes, int recvBytes) {
    if (!IsOpen()) return;
    setsockopt(RC_SOCK(m_socket), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sendBytes), sizeof(int));
    setsockopt(RC_SOCK(m_socket), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&recvBytes), sizeof(int));
}

void UdpSocket::SetTos(int tos) {
    if (!IsOpen()) return;
    setsockopt(RC_SOCK(m_socket), IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));
    if (m_dualStack)
        setsockopt(RC_SOCK(m_socket), IPPROTO_IPV6, IPV6_TCLASS, reinterpret_cast<const char*>(&tos), sizeof(tos));
}

bool UdpSocket::SendTo(const NetAddress& to, const void* data, size_t size) {
    if (!IsOpen()) return false;
    long sent;
    if (m_dualStack || m_v6Only) {
        if (m_v6Only && !to.v6) return false;
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(to.port);
        auto* b = reinterpret_cast<uint8_t*>(&addr.sin6_addr);
        if (to.v6) {
            std::memcpy(b, to.ip6, 16);
            addr.sin6_scope_id = to.scope;
        } else {
            std::memcpy(b, kV4MappedPrefix, 12);
            b[12] = uint8_t(to.ip >> 24);
            b[13] = uint8_t(to.ip >> 16);
            b[14] = uint8_t(to.ip >> 8);
            b[15] = uint8_t(to.ip);
        }
        sent = long(sendto(RC_SOCK(m_socket), static_cast<const char*>(data), int(size), 0,
                           reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    } else {
        if (to.v6) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(to.ip);
        addr.sin_port = htons(to.port);
        sent = long(sendto(RC_SOCK(m_socket), static_cast<const char*>(data), int(size), 0,
                           reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
    }
    return sent == long(size);
}

int UdpSocket::RecvFrom(void* buffer, size_t capacity, NetAddress& from, int timeoutMs) {
    if (!IsOpen()) return -1;
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = RC_SOCK(m_socket);
    pfd.events = POLLRDNORM;
    const int r = WSAPoll(&pfd, 1, timeoutMs);
#else
    pollfd pfd{};
    pfd.fd = m_socket;
    pfd.events = POLLIN;
    const int r = poll(&pfd, 1, timeoutMs);
#endif
    if (r == 0) return 0;
    if (r < 0) return -1;
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    const auto n = recvfrom(RC_SOCK(m_socket), static_cast<char*>(buffer), int(capacity), 0,
                            reinterpret_cast<sockaddr*>(&addr), &len);
    if (n < 0) {
#ifdef _WIN32
        const int err = WSAGetLastError();
        if (err == WSAECONNRESET || err == WSAEMSGSIZE) return 0;
#endif
        return IsOpen() ? 0 : -1;
    }
    if (!FromSockaddr(reinterpret_cast<sockaddr*>(&addr), from)) return 0;
    return int(n);
}

uint16_t UdpSocket::LocalPort() const {
    if (!IsOpen()) return 0;
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    getsockname(RC_SOCK(m_socket), reinterpret_cast<sockaddr*>(&addr), &len);
    NetAddress a;
    return FromSockaddr(reinterpret_cast<sockaddr*>(&addr), a) ? a.port : 0;
}

}  // namespace rc
