// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "display.hpp"
namespace minimal {
struct Image {
    std::shared_ptr<Device> d;
    VkImage handle{};
    VkDeviceMemory memory{};
    VkImage scanout{};
    VkDeviceMemory scanoutMemory{};
    VkSemaphore ready{};
    VkFence retired{};
    VkCommandBuffer firstAcquire{}, reuseAcquire{};
    std::unordered_map<uint32_t, std::array<VkCommandBuffer, 2>> release;
    bool used = false;
    explicit Image(std::shared_ptr<Device> device) : d(std::move(device)) {}
    ~Image() {
        if (retired)
            d->DestroyFence(d->handle, retired, nullptr);
        if (ready)
            d->DestroySemaphore(d->handle, ready, nullptr);
        if (handle)
            d->DestroyImage(d->handle, handle, nullptr);
        if (memory)
            d->FreeMemory(d->handle, memory, nullptr);
        if (scanout)
            d->DestroyImage(d->handle, scanout, nullptr);
        if (scanoutMemory)
            d->FreeMemory(d->handle, scanoutMemory, nullptr);
    }
};
struct Swapchain {
    std::shared_ptr<Device> d;
    std::shared_ptr<Surface> surface;
    std::vector<std::unique_ptr<Image>> images;
    std::unordered_map<uint32_t, VkCommandPool> pools;
    std::unique_ptr<Presenter> presenter;
    std::mutex presentMutex;
    std::vector<VkPipelineStageFlags> waitStages;
    uint64_t serial = 0;
    bool concurrent = false;
    VkCommandBuffer command(uint32_t family) {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = pools.at(family);
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        VkCommandBuffer cb{};
        check(d->AllocateCommandBuffers(d->handle, &info, &cb), "Allocate ownership command");
        if (d->setLoaderData)
            check(d->setLoaderData(d->handle, cb), "Initialize private command dispatch");
        return cb;
    }
    void recordAcquire(VkCommandBuffer cb, VkImage image, bool first) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(d->BeginCommandBuffer(cb, &begin), "Begin acquisition command");
        if (first) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                                  0, nullptr, 0, nullptr, 1, &b);
        }
        check(d->EndCommandBuffer(cb), "End acquisition command");
    }
    void recordCopy(VkCommandBuffer cb, const Image &image, uint32_t family, bool first) {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(d->BeginCommandBuffer(cb, &begin), "Begin display copy command");
        std::array<VkImageMemoryBarrier, 2> b{};
        for (auto &barrier : b) {
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        b[0].image = image.handle;
        b[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[1].image = image.scanout;
        b[1].oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        if (!first) {
            b[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
            b[1].dstQueueFamilyIndex = concurrent ? VK_QUEUE_FAMILY_IGNORED : family;
        }
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                              0, nullptr, 0, nullptr, b.size(), b.data());
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {surface->extent.width, surface->extent.height, 1};
        d->CmdCopyImage(cb, image.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       image.scanout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[0].dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].dstAccessMask = 0;
        b[1].srcQueueFamilyIndex = concurrent ? VK_QUEUE_FAMILY_IGNORED : family;
        b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                              0, nullptr, 0, nullptr, b.size(), b.data());
        check(d->EndCommandBuffer(cb), "End display copy command");
    }
    void initialize(const VkSwapchainCreateInfoKHR &ci) {
        if (ci.flags || ci.imageArrayLayers != 1 || ci.presentMode != VK_PRESENT_MODE_FIFO_KHR ||
            ci.preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
            ci.compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ||
            ci.imageExtent.width != surface->extent.width ||
            ci.imageExtent.height != surface->extent.height)
            throw Error(VK_ERROR_FEATURE_NOT_PRESENT, "Unsupported swapchain request");
        for (auto *p = static_cast<const VkBaseInStructure *>(ci.pNext); p; p = p->pNext)
            if (p->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT)
                throw Error(VK_ERROR_FEATURE_NOT_PRESENT, "Unsupported swapchain extension");
        if (ci.minImageCount > 3)
            throw Error(VK_ERROR_INITIALIZATION_FAILED, "Minimal swapchain supports three images");
        bool rgba =
            ci.imageFormat == VK_FORMAT_R8G8B8A8_UNORM || ci.imageFormat == VK_FORMAT_R8G8B8A8_SRGB;
        if (!rgba && ci.imageFormat != VK_FORMAT_B8G8R8A8_UNORM &&
            ci.imageFormat != VK_FORMAT_B8G8R8A8_SRGB)
            throw Error(VK_ERROR_FORMAT_NOT_SUPPORTED, "Requires an 8-bit RGBA format");
        concurrent = d->enabledFamilies.size() > 1;
        for (auto family : d->enabledFamilies) {
            VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool.queueFamilyIndex = family;
            check(d->CreateCommandPool(d->handle, &pool, nullptr, &pools[family]),
                  "Create ownership pool");
        }
        presenter = std::make_unique<Presenter>(surface->display, *surface->mode, 3);
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modifierQuery.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
        modifierQuery.sharingMode =
            concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        modifierQuery.queueFamilyIndexCount = concurrent ? d->enabledFamilies.size() : 0;
        modifierQuery.pQueueFamilyIndices = concurrent ? d->enabledFamilies.data() : nullptr;
        VkPhysicalDeviceExternalImageFormatInfo externalQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalQuery.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        externalQuery.pNext = &modifierQuery;
        VkPhysicalDeviceImageFormatInfo2 formatQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        formatQuery.pNext = &externalQuery;
        formatQuery.format = ci.imageFormat;
        formatQuery.type = VK_IMAGE_TYPE_2D;
        formatQuery.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        formatQuery.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkExternalImageFormatProperties externalProperties{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        properties.pNext = &externalProperties;
        check(d->instance->fn<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
                  "vkGetPhysicalDeviceImageFormatProperties2")(d->physical, &formatQuery,
                                                               &properties),
              "Query shared image support");
        if (!(externalProperties.externalMemoryProperties.externalMemoryFeatures &
              VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
            throw Error(VK_ERROR_FORMAT_NOT_SUPPORTED, "Image memory is not exportable");
        for (unsigned i = 0; i < 3; ++i) {
            auto image = std::make_unique<Image>(d);
            uint64_t linear = DRM_FORMAT_MOD_LINEAR;
            VkImageDrmFormatModifierListCreateInfoEXT modifier{
                VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
            modifier.drmFormatModifierCount = 1;
            modifier.pDrmFormatModifiers = &linear;
            VkExternalMemoryImageCreateInfo external{
                VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
            external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            external.pNext = &modifier;
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.pNext = &external;
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = ci.imageFormat;
            info.extent = {ci.imageExtent.width, ci.imageExtent.height, 1};
            info.mipLevels = info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
            info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            info.sharingMode = modifierQuery.sharingMode;
            info.queueFamilyIndexCount = modifierQuery.queueFamilyIndexCount;
            info.pQueueFamilyIndices = modifierQuery.pQueueFamilyIndices;
            check(d->CreateImage(d->handle, &info, nullptr, &image->scanout), "Create shared image");
            VkMemoryRequirements requirements{};
            d->GetImageMemoryRequirements(d->handle, image->scanout, &requirements);
            uint32_t chosen = UINT32_MAX;
            for (uint32_t t = 0; t < d->memory.memoryTypeCount; ++t) {
                auto flags = d->memory.memoryTypes[t].propertyFlags;
                VkMemoryPropertyFlags required =
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                if ((requirements.memoryTypeBits & (1u << t)) && (flags & required) == required) {
                    if (chosen == UINT32_MAX)
                        chosen = t;
                    if (!(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                        chosen = t;
                        break;
                    }
                }
            }
            if (chosen == UINT32_MAX)
                throw Error(VK_ERROR_FEATURE_NOT_PRESENT,
                            "No exportable shared system-memory type");
            VkMemoryDedicatedAllocateInfo dedicated{
                VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicated.image = image->scanout;
            VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
            exportInfo.pNext = &dedicated;
            exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            alloc.pNext = &exportInfo;
            alloc.allocationSize = requirements.size;
            alloc.memoryTypeIndex = chosen;
            check(d->AllocateMemory(d->handle, &alloc, nullptr, &image->scanoutMemory),
                  "Allocate shared memory");
            check(d->BindImageMemory(d->handle, image->scanout, image->scanoutMemory, 0),
                  "Bind shared memory");
            VkImageSubresource plane{VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
            VkSubresourceLayout layout{};
            d->GetImageSubresourceLayout(d->handle, image->scanout, &plane, &layout);
            VkMemoryGetFdInfoKHR get{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
            get.memory = image->scanoutMemory;
            get.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            int fd = -1;
            check(d->GetMemoryFdKHR(d->handle, &get, &fd), "Export image memory");
            Fd dmaBuf(fd);
            presenter->add(fd, layout.rowPitch, layout.offset,
                           rgba ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_ARGB8888);
            // Rendering stays in native device-local optimal memory. Only the
            // finished pixels cross into the HDMI-compatible shared allocation.
            info.pNext = nullptr;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            check(d->CreateImage(d->handle, &info, nullptr, &image->handle),
                  "Create device-local render image");
            d->GetImageMemoryRequirements(d->handle, image->handle, &requirements);
            chosen = UINT32_MAX;
            for (uint32_t t = 0; t < d->memory.memoryTypeCount; ++t) {
                const auto flags = d->memory.memoryTypes[t].propertyFlags;
                if ((requirements.memoryTypeBits & (1u << t)) &&
                    (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    chosen = t;
                    if (!(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                        break;
                }
            }
            if (chosen == UINT32_MAX)
                throw Error(VK_ERROR_FEATURE_NOT_PRESENT, "No device-local render memory");
            dedicated.image = image->handle;
            alloc.pNext = &dedicated;
            alloc.allocationSize = requirements.size;
            alloc.memoryTypeIndex = chosen;
            check(d->AllocateMemory(d->handle, &alloc, nullptr, &image->memory),
                  "Allocate device-local render memory");
            check(d->BindImageMemory(d->handle, image->handle, image->memory, 0),
                  "Bind device-local render memory");
            VkExportSemaphoreCreateInfo exportSemaphore{
                VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
            exportSemaphore.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
            VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            sem.pNext = &exportSemaphore;
            check(d->CreateSemaphore(d->handle, &sem, nullptr, &image->ready),
                  "Create display-ready semaphore");
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(d->CreateFence(d->handle, &fence, nullptr, &image->retired),
                  "Create image retirement fence");
            image->firstAcquire = command(d->acquisitionFamily);
            image->reuseAcquire = command(d->acquisitionFamily);
            recordAcquire(image->firstAcquire, image->handle, true);
            recordAcquire(image->reuseAcquire, image->handle, false);
            for (auto family : d->enabledFamilies) {
                std::array<VkCommandBuffer, 2> copies{command(family), command(family)};
                recordCopy(copies[0], *image, family, true);
                recordCopy(copies[1], *image, family, false);
                image->release.emplace(family, copies);
            }
            images.push_back(std::move(image));
        }
        presenter->start();
        note("Device-local rendering; one finished-frame copy; no display-GPU Vulkan device");
    }
    VkResult acquire(uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t *out) {
        auto until = deadline(timeout);
        uint32_t index = 0;
        auto result = presenter->acquire(timeout, &index);
        if (result != VK_SUCCESS)
            return result;
        auto &i = *images.at(index);
        result = d->GetFenceStatus(d->handle, i.retired);
        if (result == VK_NOT_READY) {
            auto left =
                until == Clock::time_point::max() ? UINT64_MAX
                : Clock::now() >= until
                    ? 0
                    : static_cast<uint64_t>(
                          std::chrono::duration_cast<std::chrono::nanoseconds>(until - Clock::now())
                              .count());
            result = d->WaitForFences(d->handle, 1, &i.retired, VK_TRUE, left);
        }
        if (result != VK_SUCCESS) {
            presenter->cancel(index);
            return result;
        }
        auto cb = i.used ? i.reuseAcquire : i.firstAcquire;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        submit.signalSemaphoreCount = semaphore ? 1 : 0;
        submit.pSignalSemaphores = semaphore ? &semaphore : nullptr;
        {
            std::lock_guard<std::mutex> g(d->acquisitionMutex);
            result = d->QueueSubmit(d->acquisitionQueue, 1, &submit, fence);
        }
        if (result != VK_SUCCESS) {
            presenter->cancel(index);
            return result;
        }
        *out = index;
        return VK_SUCCESS;
    }
    VkResult present(VkQueue queue, const VkPresentInfoKHR &info) {
        std::lock_guard<std::mutex> g(presentMutex);
        unsigned index = *info.pImageIndices;
        if (index >= images.size())
            return VK_ERROR_INITIALIZATION_FAILED;
        uint64_t id = serial + 1;
        for (auto *p = static_cast<const VkBaseInStructure *>(info.pNext); p; p = p->pNext) {
            if (p->sType != VK_STRUCTURE_TYPE_PRESENT_ID_KHR)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            id = static_cast<const VkPresentIdKHR *>(static_cast<const void *>(p))->pPresentIds[0];
        }
        if (!id || id <= serial || !presenter->acquired(index))
            return VK_ERROR_INITIALIZATION_FAILED;
        auto family = d->families.find(queue);
        if (family == d->families.end())
            return VK_ERROR_INITIALIZATION_FAILED;
        auto &i = *images[index];
        auto result = d->ResetFences(d->handle, 1, &i.retired);
        if (result != VK_SUCCESS)
            return result;
        waitStages.assign(info.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = info.waitSemaphoreCount;
        submit.pWaitSemaphores = info.pWaitSemaphores;
        submit.pWaitDstStageMask = waitStages.data();
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &i.release.at(family->second)[i.used ? 1 : 0];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &i.ready;
        result = d->QueueSubmit(queue, 1, &submit, i.retired);
        if (result != VK_SUCCESS)
            return result;
        i.used = true;
        VkSemaphoreGetFdInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
        get.semaphore = i.ready;
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        int fd = -1;
        result = d->GetSemaphoreFdKHR(d->handle, &get, &fd);
        if (result != VK_SUCCESS)
            return result;
        Fd ready(fd);
        result = presenter->present(index, fd, id);
        if (result == VK_SUCCESS)
            serial = id;
        return result;
    }
    ~Swapchain() {
        if (d && d->handle)
            d->DeviceWaitIdle(d->handle);
        presenter.reset();
        images.clear();
        for (auto &pool : pools)
            d->DestroyCommandPool(d->handle, pool.second, nullptr);
    }
};
inline std::unordered_map<VkSurfaceKHR, std::shared_ptr<Surface>> surfaces;
inline std::unordered_map<VkSwapchainKHR, std::shared_ptr<Swapchain>> chains;
inline std::unordered_set<VkImage> ownedImages;
inline std::shared_ptr<Surface> surfaceFor(VkSurfaceKHR h) {
    std::lock_guard<std::mutex> g(registryMutex);
    auto p = surfaces.find(h);
    return p == surfaces.end() ? nullptr : p->second;
}
inline std::shared_ptr<Swapchain> chainFor(VkSwapchainKHR h) {
    std::lock_guard<std::mutex> g(registryMutex);
    auto p = chains.find(h);
    return p == chains.end() ? nullptr : p->second;
}
} // namespace minimal
