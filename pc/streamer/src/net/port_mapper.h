// Asks the home router to forward a public UDP port to the stream port, so a
// headset on another network can reach this PC directly even when hole
// punching alone fails. Tries PCP (RFC 6887), NAT-PMP (RFC 6886) and UPnP IGD,
// renews the mapping before it expires and removes it on Stop().
//
// The mapping is only offered as a candidate when the router's external
// address is really public (not itself behind a carrier NAT).
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "rc/socket.h"

namespace rcs {

class PortMapper {
   public:
    ~PortMapper() { Stop(); }
    void Start(uint16_t internalPort);
    void Stop();
    // Public endpoint forwarded to us, if a mapping is active.
    bool Mapped(rc::NetAddress& out) const;
    std::string Status() const;  // for logs / UI: "PCP 1.2.3.4:38711", "unavailable", ...

   private:
    void Run();
    bool MapPcp(uint32_t lifetime);
    bool MapNatPmp(uint32_t lifetime);
    bool MapUpnp(uint32_t lifetime);
    void Unmap();
    bool Discover();  // gateway + local address of the default route

    uint16_t m_internalPort = 0;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;

    uint32_t m_gateway = 0;   // host order
    uint32_t m_localIp = 0;   // host order
    std::string m_method;     // "PCP" | "NAT-PMP" | "UPnP" | ""
    rc::NetAddress m_external;
    uint8_t m_pcpNonce[12] = {};
    std::string m_upnpControlUrl, m_upnpService;
    bool m_upnpPermanent = false;
    std::string m_status = "not started";
    std::string m_reason;  // why the last attempt failed (for the status text)
};

}  // namespace rcs
