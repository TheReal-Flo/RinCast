// portmap-test: asks the router for a UDP port mapping the way RinCast Streamer
// does (PCP, NAT-PMP, then UPnP), prints the result and removes it again.
//
//   portmap-test [internalPort]   (default 38799, a port nothing listens on)
#include <cstdio>
#include <cstdlib>

#include "net/port_mapper.h"
#include "rc/platform.h"

int main(int argc, char** argv) {
    const uint16_t port = uint16_t(argc > 1 ? std::atoi(argv[1]) : 38799);
    rcs::PortMapper mapper;
    mapper.Start(port);
    for (int i = 0; i < 200 && mapper.Status() == "not started"; i++) rc::SleepMs(100);
    std::printf("status: %s\n", mapper.Status().c_str());
    rc::NetAddress a;
    if (mapper.Mapped(a)) std::printf("public endpoint: %s -> local port %u\n", a.ToString().c_str(), port);
    mapper.Stop();  // removes the mapping
    std::printf("mapping removed\n");
    return 0;
}
