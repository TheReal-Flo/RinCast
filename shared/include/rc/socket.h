// Minimal cross-platform UDP socket. IPv4 everywhere; streaming sockets are
// dual-stack so remote sessions can also use IPv6 (often the only direct path
// when IPv4 sits behind a carrier NAT, e.g. DS-Lite or mobile networks).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rc {

struct NetAddress {
    uint32_t ip = 0;    // IPv4, host byte order (unused when v6)
    uint16_t port = 0;  // host byte order
    bool v6 = false;
    uint8_t ip6[16] = {};
    uint32_t scope = 0;  // IPv6 link-local: the interface it's on

    bool valid() const;
    bool operator==(const NetAddress& o) const;
    bool operator!=(const NetAddress& o) const { return !(*this == o); }
    std::string ToString() const;  // "1.2.3.4:5" or "[2001:db8::1]:5"
    std::string IpString() const;
    bool IsGlobalV6() const;        // 2000::/3
    bool IsLinkLocalV6() const;     // fe80::/10: on this link only, e.g. a USB cable to a headset
    // Literal IPv4/IPv6 or hostname (IPv4 preferred).
    static bool Parse(const std::string& host, uint16_t port, NetAddress& out);
    // "1.2.3.4:5" or "[v6]:5".
    static bool ParseEndpoint(const std::string& hostPort, NetAddress& out);
    // Every IPv4 and IPv6 address a hostname resolves to.
    static std::vector<NetAddress> ResolveAll(const std::string& host, uint16_t port);
    static NetAddress Broadcast(uint16_t port);
    // ff02::1, every device on the link of interface `scope`.
    static NetAddress AllNodesV6(uint16_t port, uint32_t scope);
};

struct NetInterface {
    std::string name;
    uint32_t ip;         // host order
    uint32_t netmask;    // host order
    // VPN / tunnel / virtual-machine adapter (Tailscale, WireGuard, NordLynx, WARP,
    // Hyper-V/WSL, VMware, VirtualBox) or a CGNAT/VPN-range address. Such paths are
    // never the local network, so they are not offered as direct LAN routes.
    bool isVirtual = false;
    // Of those, one that other devices can reach this one on: not a virtual machine's adapter,
    // and with a real address. For typing into a headset.
    bool isVpn = false;
    uint32_t broadcast() const { return (ip & netmask) | ~netmask; }
};

// True if packets to this IPv4 address (host order) would leave through a VPN or other tunnel.
// Such a path has less room per packet and is often slower than the local network.
bool RoutesThroughTunnel(uint32_t ip);

// The other addresses of a subnet, nearest to ip first, at most maxHosts (host order). For asking
// each directly on networks that drop broadcasts.
std::vector<uint32_t> SubnetHosts(uint32_t ip, uint32_t netmask, size_t maxHosts);

// True for 10/8, 172.16/12 and 192.168/16 (host order).
bool IsPrivateLan(uint32_t ip);
// True for 100.64.0.0/10 (carrier NAT, also used by Tailscale).
bool IsCgnat(uint32_t ip);
// Enumerates up, non-loopback IPv4 interfaces.
std::vector<NetInterface> ListInterfaces();
// Global (2000::/3) IPv6 addresses of physical adapters, with `port` filled in.
std::vector<NetAddress> ListGlobalIpv6(uint16_t port);

struct NetInterfaceV6 {
    std::string name;         // "Ethernet 3", "usb0"
    std::string description;  // Windows: the driver's, e.g. "UsbNcm Host Device"
    std::string adapterId;    // Windows: the adapter's GUID, "{...}"
    uint32_t index = 0;       // the scope of its link-local addresses
    bool usbNcm = false;      // networking over a USB cable, as a Quest offers it
};
// Up, non-loopback interfaces with IPv6.
std::vector<NetInterfaceV6> ListInterfacesV6();

class UdpSocket {
   public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Binds to the wildcard address on `port` (0 = ephemeral). Broadcast sockets
    // are IPv4-only; all others are dual-stack where the system supports it.
    bool Open(uint16_t port, bool broadcast = false, bool reuse = false);
    // IPv6 only: next to an IPv4 socket on the same port, for link-local discovery.
    bool OpenV6(uint16_t port, bool reuse = false);
    // Also receive what's sent to an IPv6 multicast group on one interface.
    bool JoinGroupV6(const NetAddress& group, uint32_t ifIndex);
#ifdef __ANDROID__
    // Through one Android network (the USB cable's), not the default one.
    bool BindToNetwork(uint64_t networkHandle);
#endif
    void Close();
    bool IsOpen() const;
    bool DualStack() const { return m_dualStack; }

    void SetBuffers(int sendBytes, int recvBytes);
    // DSCP/TOS marking (e.g. 0xB8 = EF for voice/video). Best effort.
    void SetTos(int tos);

    bool SendTo(const NetAddress& to, const void* data, size_t size);
    // Returns bytes received, 0 on timeout, -1 on error/closed.
    int RecvFrom(void* buffer, size_t capacity, NetAddress& from, int timeoutMs);

    uint16_t LocalPort() const;

   private:
#ifdef _WIN32
    uintptr_t m_socket = ~uintptr_t(0);
#else
    int m_socket = -1;
#endif
    bool m_dualStack = false;
    bool m_v6Only = false;
};

void NetInit();  // WSAStartup on Windows; no-op elsewhere.

}  // namespace rc
