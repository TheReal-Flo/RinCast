// The VR bridge's frame images on Linux: see vr_images_linux.h and pc/common/vr_bridge_images.h.
#include "platform/linux/vr_images_linux.h"

#include <drm_fourcc.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#include "gpu.h"
#include "rc/platform.h"
#include "vr_bridge_images.h"

namespace rcs {

namespace {

constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

}  // namespace

struct VrImages::Impl {
    // Vulkan, created with the first images and kept until Destroy.
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    bool dmaBufExport = false;
    uint8_t deviceUuid[16] = {};
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    VkPhysicalDeviceMemoryProperties memoryProps{};

    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        int opaqueFd = -1;  // what producers import
        int dmaBufFd = -1;  // what VAAPI imports (linear images only)
    } slots[rcvr::kFrameSlots];
    rcvr::Images description{};
    bool haveImages = false;

    // Read-back for encoders that take memory.
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    bool bufferCoherent = true;

    // The socket producers connect to.
    std::mutex mutex;  // images, description, clients
    std::vector<int> clients;
    int listenFd = -1;
    std::thread server;
    std::atomic<bool> serving{false};

    ~Impl() { DestroyAll(); }

    int32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want) const {
        for (uint32_t i = 0; i < memoryProps.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (memoryProps.memoryTypes[i].propertyFlags & want) == want) return int32_t(i);
        return -1;
    }

    // The GPU the encoder runs on (RenderNode), so frames never cross GPUs.
    bool InitVulkan() {
        if (device) return true;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "RinCast Streamer";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
            RC_LOGE("VR: no Vulkan on this PC (vkCreateInstance failed); VR streaming needs a Vulkan driver");
            return false;
        }
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());

        struct stat st{};
        const std::string node = RenderNode();
        const bool haveNode = !node.empty() && stat(node.c_str(), &st) == 0;
        const UINT vendor = DescribeGpu().vendorId;
        int best = -1, bestRank = -1;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &extCount, exts.data());
            VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            const bool hasDrm = HasExtension(exts, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
            if (hasDrm) props.pNext = &drm;
            vkGetPhysicalDeviceProperties2(devices[i], &props);
            if (!HasExtension(exts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) continue;
            int rank = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
            if (props.properties.vendorID == vendor) rank += 2;
            if (haveNode && hasDrm && drm.hasRender && makedev(drm.renderMajor, drm.renderMinor) == st.st_rdev) rank += 4;
            if (rank > bestRank) best = int(i), bestRank = rank;
        }
        if (best < 0) {
            RC_LOGE("VR: no GPU with Vulkan external memory found");
            return false;
        }
        physical = devices[best];

        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(physical, &props);
        std::memcpy(deviceUuid, id.deviceUUID, sizeof(deviceUuid));
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProps);

        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
        queueFamily = UINT32_MAX;
        for (uint32_t i = 0; i < familyCount; i++)
            if (families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
                queueFamily = i;
                break;
            }
        if (queueFamily == UINT32_MAX) return false;

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, exts.data());
        std::vector<const char*> enable{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
        dmaBufExport = HasExtension(exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        if (dmaBufExport) enable.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);

        const float priority = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(enable.size());
        dci.ppEnabledExtensionNames = enable.data();
        if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) {
            RC_LOGE("VR: vkCreateDevice failed");
            return false;
        }
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
        RC_LOGI("VR: frame images on %s", props.properties.deviceName);
        return getMemoryFd != nullptr;
    }

    // Whether images of this description can be made and exported with `handles`.
    bool Supported(VkImageTiling tiling, VkExternalMemoryHandleTypeFlagBits handle) const {
        VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
        VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 2, formats};
        VkPhysicalDeviceExternalImageFormatInfo ext{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &list,
                                                    handle};
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ext};
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = tiling;
        info.usage = kUsage;
        info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        VkExternalImageFormatProperties extProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 props{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &extProps};
        if (vkGetPhysicalDeviceImageFormatProperties2(physical, &info, &props) != VK_SUCCESS) return false;
        const auto features = extProps.externalMemoryProperties.externalMemoryFeatures;
        return (features & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
               props.imageFormatProperties.maxExtent.width >= description.width &&
               props.imageFormatProperties.maxExtent.height >= description.height;
    }

    void FreeImages() {
        for (auto& s : slots) {
            if (s.opaqueFd >= 0) close(s.opaqueFd);
            if (s.dmaBufFd >= 0) close(s.dmaBufFd);
            if (s.image) vkDestroyImage(device, s.image, nullptr);
            if (s.memory) vkFreeMemory(device, s.memory, nullptr);
            s = Slot{};
        }
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (bufferMemory) vkFreeMemory(device, bufferMemory, nullptr);
        buffer = VK_NULL_HANDLE;
        bufferMemory = VK_NULL_HANDLE;
        mapped = nullptr;
        haveImages = false;
    }

    bool MakeImages(uint32_t width, uint32_t height) {
        FreeImages();
        rcvr::Images& d = description;
        d = {};
        d.magic = rcvr::kImagesMagic;
        d.width = width;
        d.height = height;
        d.format = VK_FORMAT_R8G8B8A8_UNORM;
        d.usage = kUsage;
        d.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        // Linear images the encoder can take as they are (a DMA-BUF), where the GPU renders to
        // them; optimal ones otherwise, read back for encoding.
        const bool linear = Supported(VK_IMAGE_TILING_LINEAR, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
        d.tiling = linear ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
        d.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (linear && dmaBufExport && Supported(VK_IMAGE_TILING_LINEAR, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT))
            d.handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        if (!linear && !Supported(VK_IMAGE_TILING_OPTIMAL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)) {
            RC_LOGE("VR: this GPU can't share images between processes");
            return false;
        }

        for (int i = 0; i < rcvr::kFrameSlots; i++) {
            Slot& s = slots[i];
            VkExternalMemoryImageCreateInfo external;
            VkImageFormatListCreateInfo formatList;
            VkFormat formats[2];
            const VkImageCreateInfo info = rcvr::SlotImageInfo(d, external, formatList, formats);
            if (vkCreateImage(device, &info, nullptr, &s.image) != VK_SUCCESS) return false;
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device, s.image, &req);
            const int32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (type < 0) return false;
            // Dedicated, as the importing side allocates it too: both must say the same.
            VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, s.image};
            VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, &dedicated,
                                                  VkExternalMemoryHandleTypeFlags(d.handleTypes)};
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &exportInfo, req.size, uint32_t(type)};
            if (vkAllocateMemory(device, &alloc, nullptr, &s.memory) != VK_SUCCESS ||
                vkBindImageMemory(device, s.image, s.memory, 0) != VK_SUCCESS) {
                RC_LOGE("VR: couldn't allocate the frame images (%ux%u)", width, height);
                return false;
            }
            d.allocationSize[i] = req.size;
            VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr, s.memory,
                                     VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
            if (getMemoryFd(device, &get, &s.opaqueFd) != VK_SUCCESS) return false;
            if (d.handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
                get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                if (getMemoryFd(device, &get, &s.dmaBufFd) != VK_SUCCESS) s.dmaBufFd = -1;
            }
            if (linear) {
                const VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
                VkSubresourceLayout layout;
                vkGetImageSubresourceLayout(device, s.image, &sub, &layout);
                d.rowPitch = uint32_t(layout.rowPitch);
            }
        }
        haveImages = true;
        RC_LOGI("VR: frame images %ux%u, %s%s", width, height, linear ? "linear" : "optimal tiling",
                slots[0].dmaBufFd >= 0 ? ", encoded without a copy where the encoder takes DMA-BUF" : "");
        return true;
    }

    // ---- socket ----

    void SendImages(int client) {
        if (!haveImages) return;
        int fds[rcvr::kFrameSlots];
        for (int i = 0; i < rcvr::kFrameSlots; i++) fds[i] = slots[i].opaqueFd;
        if (!rcvr::SendWithFds(client, &description, sizeof(description), fds, rcvr::kFrameSlots))
            RC_LOGW("VR: couldn't hand the frame images to the VR runtime");
    }

    void StartServer() {
        if (serving) return;
        sockaddr_un addr;
        if (!rcvr::FillAddress(addr)) return;
        unlink(addr.sun_path);  // one left by a killed run
        listenFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (listenFd < 0 || bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listenFd, 4) != 0) {
            RC_LOGE("VR: can't listen on %s (%s)", addr.sun_path, std::strerror(errno));
            if (listenFd >= 0) close(listenFd);
            listenFd = -1;
            return;
        }
        chmod(addr.sun_path, 0600);
        serving = true;
        server = std::thread([this] { Serve(); });
    }

    void Serve() {
        while (serving) {
            std::vector<pollfd> fds{{listenFd, POLLIN, 0}};
            {
                std::lock_guard lock(mutex);
                for (int c : clients) fds.push_back({c, POLLIN, 0});
            }
            if (poll(fds.data(), fds.size(), 200) <= 0) continue;
            if (fds[0].revents & POLLIN) {
                const int c = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
                if (c >= 0) {
                    // Only our own user's processes: the socket file is 0600, and this checks it.
                    ucred cred{};
                    socklen_t len = sizeof(cred);
                    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.uid == getuid()) {
                        std::lock_guard lock(mutex);
                        clients.push_back(c);
                    } else {
                        close(c);
                    }
                }
            }
            for (size_t i = 1; i < fds.size(); i++) {
                if (!fds[i].revents) continue;
                rcvr::Hello hello{};
                int got[4], gotCount = 0;
                const ssize_t n = rcvr::ReceiveWithFds(fds[i].fd, &hello, sizeof(hello), got, 4, gotCount);
                for (int k = 0; k < gotCount; k++) close(got[k]);
                std::lock_guard lock(mutex);
                if (n == ssize_t(sizeof(hello)) && hello.magic == rcvr::kHelloMagic) {
                    RC_LOGI("VR: the VR runtime (pid %u) connected for the frame images", hello.pid);
                    SendImages(fds[i].fd);
                } else if (n <= 0 || (fds[i].revents & (POLLHUP | POLLERR))) {
                    std::erase(clients, fds[i].fd);
                    close(fds[i].fd);
                }
            }
        }
    }

    void StopServer() {
        serving = false;
        if (server.joinable()) server.join();
        std::lock_guard lock(mutex);
        for (int c : clients) close(c);
        clients.clear();
        if (listenFd >= 0) {
            close(listenFd);
            listenFd = -1;
            sockaddr_un addr;
            if (rcvr::FillAddress(addr)) unlink(addr.sun_path);
        }
    }

    // ---- read-back ----

    bool EnsureReadBack() {
        if (buffer) return true;
        if (!pool) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = queueFamily;
            if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) return false;
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool,
                                            VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
            if (vkAllocateCommandBuffers(device, &cai, &cmd) != VK_SUCCESS) return false;
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS) return false;
        }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = VkDeviceSize(description.width) * description.height * 4;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (vkCreateBuffer(device, &bci, nullptr, &buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buffer, &req);
        // Cached host memory reads fast from the CPU; coherent alone is often uncached.
        int32_t type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (type < 0) type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        if (type < 0) return false;
        bufferCoherent = memoryProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, uint32_t(type)};
        if (vkAllocateMemory(device, &alloc, nullptr, &bufferMemory) != VK_SUCCESS ||
            vkBindBufferMemory(device, buffer, bufferMemory, 0) != VK_SUCCESS ||
            vkMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
            return false;
        return true;
    }

    const uint8_t* ReadBack(int slot, int& stride) {
        if (!haveImages || slot < 0 || slot >= rcvr::kFrameSlots || !EnsureReadBack()) return nullptr;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &begin);
        // The producer rendered it on its own device and left it in GENERAL: take it over from
        // there for the copy, and give it back after.
        VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        acquire.dstQueueFamilyIndex = queueFamily;
        acquire.image = slots[slot].image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &acquire);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {description.width, description.height, 1};
        vkCmdCopyImageToBuffer(cmd, slots[slot].image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &region);
        VkImageMemoryBarrier release = acquire;
        release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        release.dstAccessMask = 0;
        release.srcQueueFamilyIndex = queueFamily;
        release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = buffer;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 1, &host, 1, &release);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkResetFences(device, 1, &fence);
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) return nullptr;
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, 100'000'000) != VK_SUCCESS) return nullptr;
        if (!bufferCoherent) {
            VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, bufferMemory, 0, VK_WHOLE_SIZE};
            vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        stride = int(description.width * 4);
        return static_cast<const uint8_t*>(mapped);
    }

    void DestroyAll() {
        StopServer();
        if (device) {
            vkDeviceWaitIdle(device);
            FreeImages();
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
        fence = VK_NULL_HANDLE;
        pool = VK_NULL_HANDLE;
        cmd = VK_NULL_HANDLE;
        device = VK_NULL_HANDLE;
        instance = VK_NULL_HANDLE;
        physical = VK_NULL_HANDLE;
    }
};

VrImages::VrImages() : m_impl(std::make_unique<Impl>()) {}
VrImages::~VrImages() = default;

VrImages& VrImages::Get() {
    static VrImages images;
    return images;
}

bool VrImages::Create(rcvr::Shared* shared, uint32_t width, uint32_t height) {
    Impl& d = *m_impl;
    if (!d.InitVulkan()) return false;
    d.StartServer();
    // The stream thread may be reading back from the old images: wait for the GPU first.
    vkDeviceWaitIdle(d.device);
    std::lock_guard lock(d.mutex);
    std::memcpy(shared->deviceUuid, d.deviceUuid, sizeof(d.deviceUuid));
    const bool ok = d.MakeImages(width, height);
    d.description.generation = uint32_t(__atomic_add_fetch(&shared->imageGeneration, 1, __ATOMIC_SEQ_CST));
    if (ok)
        for (int c : d.clients) d.SendImages(c);
    return ok;
}

void VrImages::Destroy() { m_impl->DestroyAll(); }

uint32_t VrImages::Width() const { return m_impl->description.width; }
uint32_t VrImages::Height() const { return m_impl->description.height; }

bool VrImages::DmaBuf(int slot, DmaBufFrame& out) const {
    const Impl& d = *m_impl;
    if (!d.haveImages || slot < 0 || slot >= rcvr::kFrameSlots || d.slots[slot].dmaBufFd < 0) return false;
    out = {};
    out.width = d.description.width;
    out.height = d.description.height;
    out.drmFourcc = DRM_FORMAT_ABGR8888;  // R, G, B, A bytes
    out.modifier = DRM_FORMAT_MOD_LINEAR;
    out.planeCount = 1;
    out.planes[0].fd = d.slots[slot].dmaBufFd;
    out.planes[0].offset = 0;
    out.planes[0].pitch = d.description.rowPitch;
    return true;
}

const uint8_t* VrImages::ReadBack(int slot, int& stride) { return m_impl->ReadBack(slot, stride); }

}  // namespace rcs
