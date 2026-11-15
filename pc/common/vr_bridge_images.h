// The VR bridge's frame images on Linux (vr_bridge.h): the streamer allocates them with Vulkan,
// exportable, and hands their memory to the producer (RinCast's Monado service) over a Unix
// socket; the producer creates the same images on the same GPU and binds that memory. Both sides
// build the image from SlotImageInfo below, so the two descriptions can't drift apart.
//
//   producer -> streamer:  Hello
//   streamer -> producer:  Images (+ kFrameSlots memory fds as SCM_RIGHTS), now and whenever
//                          the images change (Shared::imageGeneration says which set is current)
#pragma once

#ifndef _WIN32

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "vr_bridge.h"

namespace rcvr {

inline constexpr uint32_t kHelloMagic = 0x4C484352;   // 'RCHL'
inline constexpr uint32_t kImagesMagic = 0x4D494352;  // 'RCIM'

struct Hello {
    uint32_t magic;  // kHelloMagic
    uint32_t pid;
};

// The images' Vulkan description. Everything a producer needs to create an identical image.
struct Images {
    uint32_t magic;       // kImagesMagic
    uint32_t generation;  // = Shared::imageGeneration when they were made
    uint32_t width;       // both eyes side by side
    uint32_t height;
    uint32_t format;      // VkFormat: VK_FORMAT_R8G8B8A8_UNORM, viewed as UNORM or SRGB
    uint32_t tiling;      // VkImageTiling: LINEAR where the GPU can render to it (then the encoder
                          // can take the memory as a DMA-BUF), else OPTIMAL
    uint32_t usage;       // VkImageUsageFlags
    uint32_t flags;       // VkImageCreateFlags
    uint64_t allocationSize[kFrameSlots];
    uint32_t rowPitch;    // LINEAR only: bytes per row of the image's memory
    uint32_t handleTypes; // VkExternalMemoryHandleTypeFlags: OPAQUE_FD (what's sent), with DMA_BUF for
                          // the streamer's encoder when the GPU can export that too
};

// $XDG_RUNTIME_DIR/rincast-vr.sock: private to the user, like the shared memory.
inline std::string SocketPath() {
    const char* dir = std::getenv("XDG_RUNTIME_DIR");
    if (dir && *dir) return std::string(dir) + "/rincast-vr.sock";
    return "/tmp/rincast-vr-" + std::to_string(getuid()) + ".sock";
}

inline bool FillAddress(sockaddr_un& addr) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    const std::string path = SocketPath();
    if (path.size() >= sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// Sends `size` bytes with up to 4 file descriptors attached.
inline bool SendWithFds(int sock, const void* data, size_t size, const int* fds, int fdCount) {
    iovec iov{const_cast<void*>(data), size};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)] = {};
    if (fdCount > 0) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fdCount);
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * fdCount);
        std::memcpy(CMSG_DATA(c), fds, sizeof(int) * fdCount);
    }
    ssize_t n;
    do n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    return n == ssize_t(size);
}

// Receives exactly `size` bytes (one message) and the descriptors that came with it. Returns
// the bytes read, 0 when the peer closed, -1 on error (EAGAIN on a non-blocking socket included).
inline ssize_t ReceiveWithFds(int sock, void* data, size_t size, int* fds, int maxFds, int& fdCount) {
    fdCount = 0;
    iovec iov{data, size};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 4)] = {};
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    ssize_t n;
    do n = recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const int count = int((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        for (int i = 0; i < count; i++) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            if (fdCount < maxFds) fds[fdCount++] = fd;
            else close(fd);
        }
    }
    return n;
}

}  // namespace rcvr

#ifdef VK_VERSION_1_0
namespace rcvr {

// The one description of a frame image, for the streamer that allocates it and the producer that
// imports it. `external` and `formats` must outlive the returned struct's use (pNext chain).
inline VkImageCreateInfo SlotImageInfo(const Images& im, VkExternalMemoryImageCreateInfo& external,
                                       VkImageFormatListCreateInfo& formatList, VkFormat (&formats)[2]) {
    formats[0] = VK_FORMAT_R8G8B8A8_UNORM;
    formats[1] = VK_FORMAT_R8G8B8A8_SRGB;
    formatList = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 2, formats};
    external = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &formatList,
                VkExternalMemoryHandleTypeFlags(im.handleTypes)};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.pNext = &external;
    info.flags = im.flags;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VkFormat(im.format);
    info.extent = {im.width, im.height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VkImageTiling(im.tiling);
    info.usage = im.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    return info;
}

}  // namespace rcvr
#endif  // VK_VERSION_1_0

#endif  // !_WIN32
