// Monado's compositor renders its final image, both eyes side by side, into this target: one of
// the streamer's frame images (the VR bridge's slots), which the streamer then encodes and sends
// with the pose the image was rendered for. It plays the part the SteamVR driver's Present plays:
//
//   - Images: on Linux the streamer's Vulkan images, whose memory comes over a Unix socket
//     (pc/common/vr_bridge_images.h); on Windows the streamer's D3D11 textures, opened by name,
//     with their keyed mutex taken around the rendering.
//   - Pacing: frames are timed to the vsyncs the streamer steers to the headset's display, and
//     while a headset streams, a frame waits until the streamer took the one before (the game
//     renders at the rate the encoder keeps up with).
//   - A fourth image of its own takes the frame when every slot is busy, so the compositor never
//     waits on the streamer.
#include <algorithm>
#include <cstring>
#include <string>

#include "rincast_monado.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include "main/comp_compositor.h"
#include "main/comp_target.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_misc.h"
#include "vk/vk_helpers.h"

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>

#include "vr_bridge_images.h"
#endif

namespace rcm {

namespace {

constexpr uint32_t kScratch = rcvr::kFrameSlots;  // the image used when no slot is free
constexpr uint32_t kImageCount = rcvr::kFrameSlots + 1;

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct Target {
    comp_target base;
    bool vulkanReady = false;
    Image images[kImageCount];
    comp_target_image exported[kImageCount];
    bool haveImages = false;
    VkFormat viewFormat = VK_FORMAT_R8G8B8A8_SRGB;

    // Per frame.
    int acquired = -1;  // image index; a bridge slot when < kScratch
    int64_t frameId = 0;
    int64_t lastVsyncNs = 0;
    int64_t wakeNs = 0;
    int64_t budgetNs = 3'000'000;  // compositor time from wake-up to the frame being done
    uint64_t frameIndex = 0;
    int heldSlot = -1;
    uint64_t heldFrame = 0;
    int64_t holdOffUntilNs = 0;
    VkSemaphore renderComplete = VK_NULL_HANDLE;
    VkSemaphore acquireDone = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer acquireCmd[kImageCount] = {};
    VkCommandBuffer releaseCmd[kImageCount] = {};

#ifdef _WIN32
    wchar_t importedName[64] = {};  // textureNames[0] of the set imported
#else
    int sock = -1;
    rcvr::Images pending{};  // the newest set the streamer sent, not yet imported
    int pendingFds[rcvr::kFrameSlots] = {-1, -1, -1};
    uint32_t importedGeneration = 0;
#endif
};

Target* T(comp_target* ct) { return reinterpret_cast<Target*>(ct); }
vk_bundle* Vk(comp_target* ct) { return &ct->c->base.vk; }

// ---------------------------------------------------------------- the streamer's images

#ifndef _WIN32

void ClosePending(Target* t) {
    for (int& fd : t->pendingFds) {
        if (fd >= 0) close(fd);
        fd = -1;
    }
}

// Connects to the streamer's socket (once) and takes in any new set of images it sent.
void PollImages(Target* t) {
    if (t->sock < 0) {
        sockaddr_un addr;
        if (!rcvr::FillAddress(addr)) return;
        const int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (s < 0) return;
        if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close(s);
            return;
        }
        const rcvr::Hello hello{rcvr::kHelloMagic, uint32_t(getpid())};
        if (!rcvr::SendWithFds(s, &hello, sizeof(hello), nullptr, 0)) {
            close(s);
            return;
        }
        t->sock = s;
    }
    for (;;) {
        rcvr::Images im{};
        int fds[4], count = 0;
        const ssize_t n = rcvr::ReceiveWithFds(t->sock, &im, sizeof(im), fds, 4, count);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) {
            U_LOG_W("RinCast: lost the streamer's image socket");
            close(t->sock);
            t->sock = -1;
            return;
        }
        if (n != ssize_t(sizeof(im)) || im.magic != rcvr::kImagesMagic || count != rcvr::kFrameSlots) {
            for (int i = 0; i < count; i++) close(fds[i]);
            continue;
        }
        ClosePending(t);
        t->pending = im;
        std::copy(fds, fds + rcvr::kFrameSlots, t->pendingFds);
    }
}

// Whether the images in use are the streamer's current ones.
bool ImagesCurrent(Target* t) {
    const rcvr::Shared* s = Hub::Get().Shared();
    return t->haveImages && t->importedGeneration == uint32_t(s->imageGeneration);
}

bool CanImport(Target* t) {
    PollImages(t);
    const rcvr::Shared* s = Hub::Get().Shared();
    return t->pendingFds[0] >= 0 && t->pending.generation == uint32_t(s->imageGeneration);
}

bool ImportSlot(Target* t, vk_bundle* vk, int i, Image& out) {
    VkExternalMemoryImageCreateInfo external;
    VkImageFormatListCreateInfo formatList;
    VkFormat formats[2];
    const VkImageCreateInfo info = rcvr::SlotImageInfo(t->pending, external, formatList, formats);
    if (vk->vkCreateImage(vk->device, &info, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk->vkGetImageMemoryRequirements(vk->device, out.image, &req);
    req.size = std::max<VkDeviceSize>(req.size, t->pending.allocationSize[i]);
    VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr,
                                   VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, t->pendingFds[i]};
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &import, out.image,
                                            VK_NULL_HANDLE};
    if (vk_alloc_and_bind_image_memory(vk, out.image, &req, &dedicated, "rincast import", &out.memory) != VK_SUCCESS)
        return false;
    t->pendingFds[i] = -1;  // Vulkan owns it now
    return true;
}

bool ImportAll(Target* t, vk_bundle* vk) {
    for (int i = 0; i < rcvr::kFrameSlots; i++)
        if (!ImportSlot(t, vk, i, t->images[i])) {
            U_LOG_E("RinCast: couldn't import the streamer's frame image %d", i);
            return false;
        }
    t->importedGeneration = t->pending.generation;
    ClosePending(t);
    return true;
}

uint32_t ImageWidth(Target* t) { return t->pending.width; }
uint32_t ImageHeight(Target* t) { return t->pending.height; }

#else  // _WIN32

bool ImagesCurrent(Target* t) {
    const rcvr::Shared* s = Hub::Get().Shared();
    return t->haveImages && std::wcsncmp(t->importedName, s->textureNames[0], 64) == 0;
}

bool CanImport(Target* t) {
    const rcvr::Shared* s = Hub::Get().Shared();
    return s->textureNames[0][0] && s->textureWidth && s->textureHeight;
}

// The streamer's D3D11 texture, opened by its name.
bool ImportSlot(Target* t, vk_bundle* vk, int i, Image& out) {
    const rcvr::Shared* s = Hub::Get().Shared();
    VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 2, formats};
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, &formatList,
                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
    info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {s->textureWidth, s->textureHeight, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk->vkCreateImage(vk->device, &info, nullptr, &out.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk->vkGetImageMemoryRequirements(vk->device, out.image, &req);
    VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, nullptr,
                                            VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, nullptr,
                                            s->textureNames[i]};
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &import, out.image,
                                            VK_NULL_HANDLE};
    return vk_alloc_and_bind_image_memory(vk, out.image, &req, &dedicated, "rincast import", &out.memory) == VK_SUCCESS;
}

bool ImportAll(Target* t, vk_bundle* vk) {
    const rcvr::Shared* s = Hub::Get().Shared();
    for (int i = 0; i < rcvr::kFrameSlots; i++)
        if (!ImportSlot(t, vk, i, t->images[i])) {
            U_LOG_E("RinCast: couldn't open the streamer's frame texture %d", i);
            return false;
        }
    std::wmemcpy(t->importedName, s->textureNames[0], 64);
    return true;
}

uint32_t ImageWidth(Target*) { return Hub::Get().Shared()->textureWidth; }
uint32_t ImageHeight(Target*) { return Hub::Get().Shared()->textureHeight; }

// Takes (acquire) or gives back (release) a slot texture's keyed mutex, around the rendering.
VkResult KeyedMutex(Target* t, vk_bundle* vk, int slot, bool acquire, VkSemaphore wait, VkSemaphore signal,
                    VkFence fence) {
    const uint64_t key = 0;
    const uint32_t timeout = 20;  // ms
    VkWin32KeyedMutexAcquireReleaseInfoKHR km{VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
    if (slot >= 0) {
        if (acquire) {
            km.acquireCount = 1;
            km.pAcquireSyncs = &t->images[slot].memory;
            km.pAcquireKeys = &key;
            km.pAcquireTimeouts = &timeout;
        } else {
            km.releaseCount = 1;
            km.pReleaseSyncs = &t->images[slot].memory;
            km.pReleaseKeys = &key;
        }
    }
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, slot >= 0 ? &km : nullptr};
    submit.waitSemaphoreCount = wait ? 1 : 0;
    submit.pWaitSemaphores = &wait;
    submit.pWaitDstStageMask = &stage;
    submit.signalSemaphoreCount = signal ? 1 : 0;
    submit.pSignalSemaphores = &signal;
    return vk_cmd_submit_locked(vk, vk->main_queue, 1, &submit, fence);
}

#endif

void DestroyImages(Target* t, vk_bundle* vk) {
    if (!t->vulkanReady) return;
    vk->vkDeviceWaitIdle(vk->device);
    for (Image& im : t->images) {
        if (im.view) vk->vkDestroyImageView(vk->device, im.view, nullptr);
        if (im.image) vk->vkDestroyImage(vk->device, im.image, nullptr);
        if (im.memory) vk->vkFreeMemory(vk->device, im.memory, nullptr);
        im = Image{};
    }
    t->haveImages = false;
    t->base.images = nullptr;
    t->base.image_count = 0;
}

bool MakeScratch(Target* t, vk_bundle* vk, uint32_t width, uint32_t height) {
    Image& im = t->images[kScratch];
    VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, nullptr, 2, formats};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &formatList};
    info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk->vkCreateImage(vk->device, &info, nullptr, &im.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vk->vkGetImageMemoryRequirements(vk->device, im.image, &req);
    return vk_alloc_and_bind_image_memory(vk, im.image, &req, nullptr, "rincast scratch", &im.memory) == VK_SUCCESS;
}

// Ownership barriers (Linux): the images are shared with the streamer's own Vulkan device, so
// each frame takes the image over from it ("external") and hands it back once rendered.
bool RecordBarriers(Target* t, vk_bundle* vk) {
    const uint32_t family = vk->main_queue->family_index;
    if (!t->pool) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = family;
        if (vk->vkCreateCommandPool(vk->device, &pci, nullptr, &t->pool) != VK_SUCCESS) return false;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, t->pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, kImageCount};
        if (vk->vkAllocateCommandBuffers(vk->device, &cai, t->acquireCmd) != VK_SUCCESS ||
            vk->vkAllocateCommandBuffers(vk->device, &cai, t->releaseCmd) != VK_SUCCESS)
            return false;
    } else {
        vk->vkResetCommandPool(vk->device, t->pool, 0);
    }
    for (uint32_t i = 0; i < kImageCount; i++) {
        const bool shared = i < kScratch;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image = t->images[i].image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // Take over: the old contents don't matter, the compositor draws all of it.
        vk->vkBeginCommandBuffer(t->acquireCmd[i], &begin);
        b.srcAccessMask = 0;
        b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = shared ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = shared ? family : VK_QUEUE_FAMILY_IGNORED;
        vk->vkCmdPipelineBarrier(t->acquireCmd[i], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vk->vkEndCommandBuffer(t->acquireCmd[i]);
        // Hand back, in GENERAL, where the streamer reads it.
        vk->vkBeginCommandBuffer(t->releaseCmd[i], &begin);
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = shared ? family : VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = shared ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
        vk->vkCmdPipelineBarrier(t->releaseCmd[i], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vk->vkEndCommandBuffer(t->releaseCmd[i]);
    }
    return true;
}

// ---------------------------------------------------------------- comp_target

bool InitPreVulkan(comp_target*) { return true; }

bool InitPostVulkan(comp_target* ct, uint32_t, uint32_t) {
    Target* t = T(ct);
    vk_bundle* vk = Vk(ct);
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk->vkCreateSemaphore(vk->device, &sci, nullptr, &t->renderComplete) != VK_SUCCESS ||
        vk->vkCreateSemaphore(vk->device, &sci, nullptr, &t->acquireDone) != VK_SUCCESS ||
        vk->vkCreateFence(vk->device, &fci, nullptr, &t->fence) != VK_SUCCESS)
        return false;
    // The renderer waits for acquireDone (the slot taken over) and signals renderComplete, which
    // present waits for before handing the frame to the streamer.
    ct->semaphores.present_complete = t->acquireDone;
    ct->semaphores.render_complete = t->renderComplete;
    ct->semaphores.render_complete_is_timeline = false;
    t->vulkanReady = true;
    return true;
}

bool CheckReady(comp_target* ct) {
    Target* t = T(ct);
    return t->vulkanReady && (ImagesCurrent(t) || CanImport(t));
}

void CreateImages(comp_target* ct, const comp_target_create_images_info* info) {
    Target* t = T(ct);
    vk_bundle* vk = Vk(ct);
    DestroyImages(t, vk);
    // sRGB views where the compositor takes them (its graphics path): the image then holds what
    // the encoder expects, gamma-encoded 8-bit colour.
    t->viewFormat = VK_FORMAT_UNDEFINED;
    for (uint32_t i = 0; i < info->format_count && t->viewFormat == VK_FORMAT_UNDEFINED; i++)
        if (info->formats[i] == VK_FORMAT_R8G8B8A8_SRGB || info->formats[i] == VK_FORMAT_R8G8B8A8_UNORM)
            t->viewFormat = info->formats[i];
    if (t->viewFormat == VK_FORMAT_UNDEFINED) {
        U_LOG_E("RinCast: the compositor wants none of the frame images' formats");
        return;
    }
    if (!CanImport(t) || !ImportAll(t, vk)) return;
    const uint32_t width = ImageWidth(t), height = ImageHeight(t);
    if (!MakeScratch(t, vk, width, height)) return;
    for (uint32_t i = 0; i < kImageCount; i++) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = t->images[i].image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = t->viewFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk->vkCreateImageView(vk->device, &vci, nullptr, &t->images[i].view) != VK_SUCCESS) return;
        t->exported[i] = {t->images[i].image, t->images[i].view};
    }
    if (!RecordBarriers(t, vk)) return;
    ct->images = t->exported;
    ct->image_count = kImageCount;
    ct->width = width;
    ct->height = height;
    ct->format = t->viewFormat;
    ct->final_layout = VK_IMAGE_LAYOUT_GENERAL;
    t->haveImages = true;
    U_LOG_I("RinCast: rendering into the streamer's frame images, %ux%u", width, height);
}

bool HasImages(comp_target* ct) { return T(ct)->haveImages; }

VkResult Acquire(comp_target* ct, uint32_t* outIndex) {
    Target* t = T(ct);
    vk_bundle* vk = Vk(ct);
    if (!ImagesCurrent(t)) return VK_ERROR_OUT_OF_DATE_KHR;  // the streamer made new ones
    const int slot = Hub::Get().Bridge().AcquireSlot();
    uint32_t index = slot >= 0 ? uint32_t(slot) : kScratch;
#ifdef _WIN32
    VkResult r = slot >= 0 ? KeyedMutex(t, vk, slot, true, VK_NULL_HANDLE, t->acquireDone, VK_NULL_HANDLE) : VK_TIMEOUT;
    if (r != VK_SUCCESS) {
        // The streamer holds the texture (or no slot was free): render into our own this time.
        if (slot >= 0) Hub::Get().Bridge().AbandonSlot(slot);
        index = kScratch;
        r = KeyedMutex(t, vk, -1, true, VK_NULL_HANDLE, t->acquireDone, VK_NULL_HANDLE);
    }
#else
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    (void)stage;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &t->acquireCmd[index];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &t->acquireDone;
    const VkResult r = vk_cmd_submit_locked(vk, vk->main_queue, 1, &submit, VK_NULL_HANDLE);
#endif
    if (r != VK_SUCCESS) return r;
    t->acquired = int(index);
    *outIndex = index;
    return VK_SUCCESS;
}

VkResult Present(comp_target* ct, VkQueue, uint32_t index, uint64_t, int64_t, int64_t) {
    Target* t = T(ct);
    vk_bundle* vk = Vk(ct);
    const int slot = index < kScratch ? int(index) : -1;
    t->acquired = -1;
    vk->vkResetFences(vk->device, 1, &t->fence);
#ifdef _WIN32
    VkResult r = KeyedMutex(t, vk, slot, false, t->renderComplete, VK_NULL_HANDLE, t->fence);
#else
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &t->renderComplete;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &t->releaseCmd[index];
    VkResult r = vk_cmd_submit_locked(vk, vk->main_queue, 1, &submit, t->fence);
#endif
    if (r == VK_SUCCESS) r = vk->vkWaitForFences(vk->device, 1, &t->fence, VK_TRUE, 200'000'000);
    if (r != VK_SUCCESS) {
        if (slot >= 0) Hub::Get().Bridge().AbandonSlot(slot);
        return r == VK_TIMEOUT ? VK_SUCCESS : r;
    }
    // The compositor's time for this frame, for the next one's wake-up.
    const int64_t now = os_monotonic_get_ns();
    if (t->wakeNs) t->budgetNs = (t->budgetNs * 7 + std::clamp<int64_t>(now - t->wakeNs, 500'000, 20'000'000)) / 8;
    if (slot < 0) return VK_SUCCESS;  // rendered into our own image: nobody takes it

    rcvr::Shared* s = Hub::Get().Shared();
    rcvr::TrackingSnapshot rendered = Hub::Get().RenderedWith();
    if (!rendered.sequence) Hub::Get().Tracking(rendered);
    rcvr::FrameSlot& fs = s->slots[slot];
    fs.frameIndex = ++t->frameIndex;
    fs.trackingSequence = rendered.sequence;
    fs.headsetTargetNs = rendered.headsetTargetNs;
    fs.submitPcNs = rcvr::QpcNowNs();
    fs.renderPose = rendered.hmd;
    std::memcpy(fs.fovTan, Hub::Get().Display().fovTan, sizeof(fs.fovTan));
    s->renderedEyeWidth = LONG(ct->width / 2);
    s->renderedEyeHeight = LONG(ct->height);
    Hub::Get().Bridge().PublishSlot(slot);
    t->heldSlot = slot;
    t->heldFrame = fs.frameIndex;
    return VK_SUCCESS;
}

VkResult WaitForPresent(comp_target*, time_duration_ns) { return VK_ERROR_EXTENSION_NOT_PRESENT; }

void Flush(comp_target*) {}

void CalcFramePacing(comp_target* ct, int64_t* outFrameId, int64_t* outWake, int64_t* outDesiredPresent,
                     int64_t* outSlop, int64_t* outPredictedDisplay) {
    Target* t = T(ct);
    Hub& hub = Hub::Get();
    hub.SetCompositorThread();
    // While a headset streams, the next frame starts once the streamer took the one before: a
    // frame it hasn't taken would only be replaced. A streamer that stops taking frames is left
    // alone for a few seconds, so the compositor never stalls on it.
    auto& bridge = hub.Bridge();
    if (t->heldSlot >= 0 && bridge.Streaming() && os_monotonic_get_ns() >= t->holdOffUntilNs &&
        !bridge.WaitTaken(t->heldSlot, t->heldFrame, std::max<int64_t>(3 * hub.PeriodNs(), 40'000'000))) {
        t->holdOffUntilNs = os_monotonic_get_ns() + 3'000'000'000LL;
        U_LOG_W("RinCast: the streamer didn't take a frame for several refreshes; not waiting for it for 3 s");
    }
    t->heldSlot = -1;
    const int64_t now = rcvr::QpcNowNs();
    const int64_t next = hub.NextVsync(now, t->lastVsyncNs);
    t->lastVsyncNs = next;
    *outFrameId = ++t->frameId;
    *outWake = std::max(now, next - t->budgetNs - 1'000'000);
    *outDesiredPresent = next;
    *outSlop = 0;
    *outPredictedDisplay = next;
}

void MarkTimingPoint(comp_target* ct, comp_target_timing_point point, int64_t, int64_t when) {
    if (point == COMP_TARGET_TIMING_POINT_WAKE_UP) T(ct)->wakeNs = when;
}

VkResult UpdateTimings(comp_target*) { return VK_SUCCESS; }
void InfoGpu(comp_target*, int64_t, int64_t, int64_t, int64_t) {}
void SetTitle(comp_target*, const char*) {}

xrt_result_t GetRefreshRates(comp_target*, uint32_t* count, float* rates) {
    *count = 1;
    rates[0] = Hub::Get().RefreshRate();
    return XRT_SUCCESS;
}
xrt_result_t GetCurrentRefreshRate(comp_target*, float* rate) {
    *rate = Hub::Get().RefreshRate();
    return XRT_SUCCESS;
}
// The headset's rate is the streamer's to choose (the headset's settings).
xrt_result_t RequestRefreshRate(comp_target*, float) { return XRT_SUCCESS; }

void Destroy(comp_target* ct) {
    Target* t = T(ct);
    vk_bundle* vk = Vk(ct);
    DestroyImages(t, vk);
    if (t->vulkanReady) {
        if (t->pool) vk->vkDestroyCommandPool(vk->device, t->pool, nullptr);
        if (t->fence) vk->vkDestroyFence(vk->device, t->fence, nullptr);
        if (t->renderComplete) vk->vkDestroySemaphore(vk->device, t->renderComplete, nullptr);
        if (t->acquireDone) vk->vkDestroySemaphore(vk->device, t->acquireDone, nullptr);
    }
#ifndef _WIN32
    ClosePending(t);
    if (t->sock >= 0) close(t->sock);
#endif
    delete t;
}

bool FactoryDetect(const comp_target_factory*, struct comp_compositor*) { return true; }

bool FactoryCreate(const comp_target_factory*, struct comp_compositor* c, comp_target** out) {
    auto* t = new Target{};
    comp_target& b = t->base;
    b.c = c;
    b.name = "RinCast";
    b.init_pre_vulkan = InitPreVulkan;
    b.init_post_vulkan = InitPostVulkan;
    b.check_ready = CheckReady;
    b.create_images = CreateImages;
    b.has_images = HasImages;
    b.acquire = Acquire;
    b.present = Present;
    b.wait_for_present = WaitForPresent;
    b.flush = Flush;
    b.calc_frame_pacing = CalcFramePacing;
    b.mark_timing_point = MarkTimingPoint;
    b.update_timings = UpdateTimings;
    b.info_gpu = InfoGpu;
    b.set_title = SetTitle;
    b.get_refresh_rates = GetRefreshRates;
    b.get_current_refresh_rate = GetCurrentRefreshRate;
    b.request_refresh_rate = RequestRefreshRate;
    b.destroy = Destroy;
    b.wait_for_present_supported = false;
    *out = &b;
    return true;
}

const char* g_deviceExtensions[] = {
#ifdef _WIN32
    VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME,
#else
    // The images' description names DMA-BUF export when the streamer's GPU has it: the same
    // description here needs the extension too.
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
#endif
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
};

const comp_target_factory g_factory = {
    "RinCast Streamer",   // name
    "rincast",            // identifier
    false,                // requires_vulkan_for_create
    false,                // is_deferred
    VK_API_VERSION_1_1,   // required_instance_version
    nullptr,              // required_instance_extensions
    0,                    // required_instance_extension_count
    g_deviceExtensions,   // optional_device_extensions
    sizeof(g_deviceExtensions) / sizeof(g_deviceExtensions[0]),
    FactoryDetect,
    FactoryCreate,
};

}  // namespace

const comp_target_factory* TargetFactory() { return &g_factory; }

}  // namespace rcm
