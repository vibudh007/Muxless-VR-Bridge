// SPDX-License-Identifier: GPL-3.0-or-later
#include "swapchain.hpp"
#include "../runtime/profile_session.hpp"
#include <cmath>
#include <cstdlib>
#include <sys/stat.h>
#include <type_traits>
using namespace minimal;
#define VKENTRY static VKAPI_ATTR
#define FORWARD_I(name, object, ...)                                                               \
    auto context = instanceFor(object);                                                            \
    return context->fn<PFN_##name>(#name)(__VA_ARGS__)
#define FORWARD_D(name, object, ...)                                                               \
    auto context = deviceFor(object);                                                              \
    return context->fn<PFN_##name>(#name)(__VA_ARGS__)
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL minimalGIPA(VkInstance, const char *);
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL minimalGDPA(VkDevice, const char *);
VKENTRY VkResult VKAPI_CALL makeInstance(const VkInstanceCreateInfo *ci,
                                         const VkAllocationCallbacks *a, VkInstance *out) {
    auto *link = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                    link->function != VK_LAYER_LINK_INFO))
        link = (VkLayerInstanceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto next = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto result =
        reinterpret_cast<PFN_vkCreateInstance>(next(nullptr, "vkCreateInstance"))(ci, a, out);
    if (result != VK_SUCCESS)
        return result;
    try {
        auto i = std::make_shared<Instance>();
        i->handle = *out;
        i->next = next;
        for (uint32_t n = 0; n < ci->enabledExtensionCount; ++n)
            if (!strcmp(ci->ppEnabledExtensionNames[n], VK_EXT_ACQUIRE_DRM_DISPLAY_EXTENSION_NAME))
                i->enabled = true;
        std::lock_guard<std::mutex> g(registryMutex);
        instances.emplace(dispatchKey(*out), i);
        return result;
    } catch (const std::exception &e) {
        reinterpret_cast<PFN_vkDestroyInstance>(next(*out, "vkDestroyInstance"))(*out, a);
        return report(e);
    }
}
VKENTRY void VKAPI_CALL dropInstance(VkInstance instance, const VkAllocationCallbacks *a) {
    auto context = instanceFor(instance);
    {
        std::lock_guard<std::mutex> g(registryMutex);
        instances.erase(dispatchKey(instance));
    }
    context->fn<PFN_vkDestroyInstance>("vkDestroyInstance")(instance, a);
}
VKENTRY VkResult VKAPI_CALL makeDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *ci,
                                       const VkAllocationCallbacks *a, VkDevice *out) {
    auto i = instanceFor(physical);
    auto *link = (VkLayerDeviceCreateInfo *)ci->pNext;
    PFN_vkSetDeviceLoaderData setData = nullptr;
    for (auto *p = (VkLayerDeviceCreateInfo *)ci->pNext; p; p = (VkLayerDeviceCreateInfo *)p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            p->function == VK_LOADER_DATA_CALLBACK)
            setData = p->u.pfnSetDeviceLoaderData;
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                    link->function != VK_LAYER_LINK_INFO))
        link = (VkLayerDeviceCreateInfo *)link->pNext;
    if (!link)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(i->handle, "vkCreateDevice"));
    try {
        VkDeviceCreateInfo local = *ci;
        std::vector<const char *> extensions;
        std::vector<VkDeviceQueueCreateInfo> queues;
        std::vector<std::vector<float>> priorities;
        uint32_t privateFamily = UINT32_MAX, privateIndex = 0;
        VkPhysicalDeviceSynchronization2Features sync{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        if (i->enabled) {
            VkPhysicalDeviceProperties props{};
            i->fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(physical,
                                                                                      &props);
            if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                throw Error(
                    VK_ERROR_INITIALIZATION_FAILED,
                    "VR render device must be the discrete GPU; launch with Mesa GPU offload");
            uint32_t count = 0;
            auto enumerate = i->fn<PFN_vkEnumerateDeviceExtensionProperties>(
                "vkEnumerateDeviceExtensionProperties");
            check(enumerate(physical, nullptr, &count, nullptr), "Enumerate render extensions");
            std::vector<VkExtensionProperties> available(count);
            check(enumerate(physical, nullptr, &count, available.data()), "Read render extensions");
            if (ci->enabledExtensionCount)
                extensions.assign(ci->ppEnabledExtensionNames,
                                  ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
            for (auto required : {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                                  VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
                                  VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
                                  VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
                                  VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
                                  VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
                                  VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME}) {
                if (std::none_of(available.begin(), available.end(),
                                 [&](const auto &e) { return !strcmp(e.extensionName, required); }))
                    throw Error(VK_ERROR_EXTENSION_NOT_PRESENT, required);
                if (std::none_of(extensions.begin(), extensions.end(),
                                 [&](const char *n) { return !strcmp(n, required); }))
                    extensions.push_back(required);
            }
            auto query = i->fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                "vkGetPhysicalDeviceQueueFamilyProperties");
            query(physical, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            query(physical, &count, families.data());
            queues.assign(ci->pQueueCreateInfos, ci->pQueueCreateInfos + ci->queueCreateInfoCount);
            for (auto &q : queues)
                if (q.flags)
                    throw Error(VK_ERROR_FEATURE_NOT_PRESENT,
                                "Protected queues are outside the minimal prototype");
            // Reserve a private queue so image acquisition never waits behind the application's
            // graphics queue.
            for (int pass = 0; pass < 2 && privateFamily == UINT32_MAX; ++pass)
                for (uint32_t f = 0; f < count; ++f) {
                    auto flags = families[f].queueFlags;
                    if (!(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
                        continue;
                    if (!pass && (flags & VK_QUEUE_GRAPHICS_BIT))
                        continue;
                    auto q = std::find_if(queues.begin(), queues.end(),
                                          [&](const auto &x) { return x.queueFamilyIndex == f; });
                    uint32_t used = q == queues.end() ? 0 : q->queueCount;
                    if (used < families[f].queueCount) {
                        privateFamily = f;
                        privateIndex = used;
                        break;
                    }
                }
            if (privateFamily == UINT32_MAX)
                throw Error(VK_ERROR_FEATURE_NOT_PRESENT,
                            "No spare Vulkan queue for independent image acquisition");
            auto chosen = std::find_if(queues.begin(), queues.end(), [&](const auto &q) {
                return q.queueFamilyIndex == privateFamily;
            });
            if (chosen == queues.end()) {
                VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                q.queueFamilyIndex = privateFamily;
                q.queueCount = 1;
                queues.push_back(q);
            } else
                ++chosen->queueCount;
            priorities.reserve(queues.size());
            for (auto &q : queues) {
                std::vector<float> values(q.queueCount, 1.0f);
                auto original = std::find_if(
                    ci->pQueueCreateInfos, ci->pQueueCreateInfos + ci->queueCreateInfoCount,
                    [&](const auto &x) { return x.queueFamilyIndex == q.queueFamilyIndex; });
                if (original != ci->pQueueCreateInfos + ci->queueCreateInfoCount)
                    std::copy_n(original->pQueuePriorities, original->queueCount, values.data());
                priorities.push_back(std::move(values));
            }
            for (size_t q = 0; q < queues.size(); ++q)
                queues[q].pQueuePriorities = priorities[q].data();
            VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            supported.pNext = &sync;
            i->fn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2")(physical,
                                                                                    &supported);
            if (!sync.synchronization2)
                throw Error(VK_ERROR_FEATURE_NOT_PRESENT,
                            "Shared foreign ownership requires synchronization2");
            bool supplied = false;
            for (auto *p = static_cast<const VkBaseInStructure *>(ci->pNext); p; p = p->pNext) {
                if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) {
                    supplied = true;
                    if (!reinterpret_cast<const VkPhysicalDeviceSynchronization2Features *>(p)
                             ->synchronization2)
                        throw Error(VK_ERROR_FEATURE_NOT_PRESENT,
                                    "Application explicitly disabled synchronization2");
                }
                if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
                    supplied = true;
                    if (!reinterpret_cast<const VkPhysicalDeviceVulkan13Features *>(p)
                             ->synchronization2)
                        throw Error(VK_ERROR_FEATURE_NOT_PRESENT,
                                    "Application explicitly disabled synchronization2");
                }
            }
            if (!supplied) {
                sync.pNext = const_cast<void *>(ci->pNext);
                local.pNext = &sync;
            }
            local.queueCreateInfoCount = queues.size();
            local.pQueueCreateInfos = queues.data();
            local.enabledExtensionCount = extensions.size();
            local.ppEnabledExtensionNames = extensions.data();
        }
        auto result = create(physical, &local, a, out);
        if (result != VK_SUCCESS)
            return result;
        try {
            auto d = std::make_shared<Device>();
            d->handle = *out;
            d->physical = physical;
            d->instance = i;
            d->next = gdpa;
            d->setLoaderData = setData;
            d->load();
            if (i->enabled) {
                i->fn<PFN_vkGetPhysicalDeviceMemoryProperties>(
                    "vkGetPhysicalDeviceMemoryProperties")(physical, &d->memory);
                for (const auto &q : queues) {
                    d->enabledFamilies.push_back(q.queueFamilyIndex);
                    for (uint32_t n = 0; n < q.queueCount; ++n) {
                        VkQueue handle{};
                        d->GetDeviceQueue(*out, q.queueFamilyIndex, n, &handle);
                        if (setData)
                            check(setData(*out, handle), "Initialize queue dispatch");
                        d->families.emplace(handle, q.queueFamilyIndex);
                        if (q.queueFamilyIndex == privateFamily && n == privateIndex)
                            d->acquisitionQueue = handle;
                    }
                }
                d->acquisitionFamily = privateFamily;
                VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
                VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                properties.pNext=&drm;
                i->fn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(physical,&properties);
                if(drm.hasRender && drm.renderMajor<=UINT32_MAX && drm.renderMinor<=UINT32_MAX)
                    d->performanceLease=vrProfile::acquire(drm.renderMajor,drm.renderMinor);
            }
            std::lock_guard<std::mutex> g(registryMutex);
            devices.emplace(dispatchKey(*out), d);
            return result;
        } catch (...) {
            reinterpret_cast<PFN_vkDestroyDevice>(gdpa(*out, "vkDestroyDevice"))(*out, a);
            throw;
        }
    } catch (const std::exception &e) {
        return report(e);
    }
}
VKENTRY void VKAPI_CALL dropDevice(VkDevice device, const VkAllocationCallbacks *a) {
    auto d = deviceFor(device);
    std::vector<std::shared_ptr<Swapchain>> retired;
    {
        std::lock_guard<std::mutex> g(registryMutex);
        for (auto p = chains.begin(); p != chains.end();)
            if (p->second->d == d) {
                for (auto &image : p->second->images)
                    ownedImages.erase(image->handle);
                retired.push_back(std::move(p->second));
                p = chains.erase(p);
            } else
                ++p;
    }
    retired.clear();
    {
        std::lock_guard<std::mutex> g(registryMutex);
        devices.erase(dispatchKey(device));
    }
    d->DestroyDevice(device, a);
}
VKENTRY VkResult VKAPI_CALL getDisplay(VkPhysicalDevice physical, int fd, uint32_t connector,
                                       VkDisplayKHR *out) {
    auto i = instanceFor(physical);
    if (!i->enabled) {
        FORWARD_I(vkGetDrmDisplayEXT, physical, physical, fd, connector, out);
    }
    try {
        if (i->display) {
            if (i->display->connector != connector)
                return VK_ERROR_FEATURE_NOT_PRESENT;
            *out = opaque<VkDisplayKHR>(i->display.get());
            return VK_SUCCESS;
        }
        auto d = std::make_shared<Display>(fd, connector);
        i->renderPhysical = physical;
        i->display = d;
        *out = opaque<VkDisplayKHR>(d.get());
        note("Using the runtime-provided DRM lease");
        return VK_SUCCESS;
    } catch (const std::exception &e) {
        return report(e);
    }
}
VKENTRY VkResult VKAPI_CALL acquireDisplay(VkPhysicalDevice physical, int fd,
                                           VkDisplayKHR display) {
    auto i = instanceFor(physical);
    if (i->display && display == opaque<VkDisplayKHR>(i->display.get())) {
        struct stat a{}, b{};
        if (fstat(fd, &a) || fstat(i->display->fd.get(), &b) || a.st_rdev != b.st_rdev)
            return VK_ERROR_INITIALIZATION_FAILED;
        try {
            i->display->bindLease(fd);
        } catch (const std::exception &e) {
            return report(e);
        }
        return VK_SUCCESS;
    }
    FORWARD_I(vkAcquireDrmDisplayEXT, physical, physical, fd, display);
}
VKENTRY VkResult VKAPI_CALL releaseDisplay(VkPhysicalDevice physical, VkDisplayKHR display) {
    auto i = instanceFor(physical);
    if (i->display && display == opaque<VkDisplayKHR>(i->display.get()))
        return VK_SUCCESS;
    FORWARD_I(vkReleaseDisplayEXT, physical, physical, display);
}
VKENTRY VkResult VKAPI_CALL displayProperties(VkPhysicalDevice physical, uint32_t *count,
                                              VkDisplayPropertiesKHR *out) {
    auto i = instanceFor(physical);
    if (!i->display) {
        FORWARD_I(vkGetPhysicalDeviceDisplayPropertiesKHR, physical, physical, count, out);
    }
    auto &d = *i->display;
    auto &m = d.modes.front();
    VkDisplayPropertiesKHR p{};
    p.display = opaque<VkDisplayKHR>(&d);
    p.displayName = d.name.c_str();
    p.physicalDimensions = {d.widthMM, d.heightMM};
    p.physicalResolution = {m.drm.hdisplay, m.drm.vdisplay};
    p.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    return enumerate(std::vector<VkDisplayPropertiesKHR>{p}, count, out);
}
VKENTRY VkResult VKAPI_CALL planeProperties(VkPhysicalDevice physical, uint32_t *count,
                                            VkDisplayPlanePropertiesKHR *out) {
    auto i = instanceFor(physical);
    if (!i->display) {
        FORWARD_I(vkGetPhysicalDeviceDisplayPlanePropertiesKHR, physical, physical, count, out);
    }
    VkDisplayPlanePropertiesKHR p{opaque<VkDisplayKHR>(i->display.get()), 0};
    return enumerate(std::vector<VkDisplayPlanePropertiesKHR>{p}, count, out);
}
VKENTRY VkResult VKAPI_CALL supportedDisplays(VkPhysicalDevice physical, uint32_t plane,
                                              uint32_t *count, VkDisplayKHR *out) {
    auto i = instanceFor(physical);
    if (!i->display) {
        FORWARD_I(vkGetDisplayPlaneSupportedDisplaysKHR, physical, physical, plane, count, out);
    }
    if (plane)
        return VK_ERROR_INITIALIZATION_FAILED;
    return enumerate(std::vector<VkDisplayKHR>{opaque<VkDisplayKHR>(i->display.get())}, count, out);
}
VKENTRY VkResult VKAPI_CALL modeProperties(VkPhysicalDevice physical, VkDisplayKHR display,
                                           uint32_t *count, VkDisplayModePropertiesKHR *out) {
    auto i = instanceFor(physical);
    if (!i->display || display != opaque<VkDisplayKHR>(i->display.get())) {
        FORWARD_I(vkGetDisplayModePropertiesKHR, physical, physical, display, count, out);
    }
    std::vector<VkDisplayModePropertiesKHR> modes;
    for (auto &m : i->display->modes)
        modes.push_back({opaque<VkDisplayModeKHR>(&m),
                         {{m.drm.hdisplay, m.drm.vdisplay}, uint32_t(Display::hz(m) * 1000 + .5)}});
    return enumerate(modes, count, out);
}
VKENTRY VkResult VKAPI_CALL createMode(VkPhysicalDevice physical, VkDisplayKHR display,
                                       const VkDisplayModeCreateInfoKHR *ci,
                                       const VkAllocationCallbacks *a, VkDisplayModeKHR *out) {
    auto i = instanceFor(physical);
    if (!i->display || display != opaque<VkDisplayKHR>(i->display.get())) {
        FORWARD_I(vkCreateDisplayModeKHR, physical, physical, display, ci, a, out);
    }
    for (auto &m : i->display->modes)
        if (m.drm.hdisplay == ci->parameters.visibleRegion.width &&
            m.drm.vdisplay == ci->parameters.visibleRegion.height &&
            std::abs(Display::hz(m) * 1000 - ci->parameters.refreshRate) < 100) {
            *out = opaque<VkDisplayModeKHR>(&m);
            return VK_SUCCESS;
        }
    return VK_ERROR_INITIALIZATION_FAILED;
}
VKENTRY VkResult VKAPI_CALL planeCapabilities(VkPhysicalDevice physical, VkDisplayModeKHR mode,
                                              uint32_t plane, VkDisplayPlaneCapabilitiesKHR *out) {
    auto i = instanceFor(physical);
    if (!i->display) {
        FORWARD_I(vkGetDisplayPlaneCapabilitiesKHR, physical, physical, mode, plane, out);
    }
    for (auto &m : i->display->modes)
        if (mode == opaque<VkDisplayModeKHR>(&m) && plane == 0) {
            *out = {};
            out->supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
            out->minSrcExtent = out->maxSrcExtent = out->minDstExtent =
                out->maxDstExtent = {m.drm.hdisplay, m.drm.vdisplay};
            return VK_SUCCESS;
        }
    return VK_ERROR_INITIALIZATION_FAILED;
}
VKENTRY VkResult VKAPI_CALL createSurface(VkInstance instance,
                                          const VkDisplaySurfaceCreateInfoKHR *ci,
                                          const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    auto i = instanceFor(instance);
    if (i->display)
        for (auto &m : i->display->modes)
            if (ci->displayMode == opaque<VkDisplayModeKHR>(&m)) {
                if (ci->flags || ci->pNext || ci->planeIndex || ci->planeStackIndex ||
                    ci->transform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
                    ci->alphaMode != VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR ||
                    ci->imageExtent.width != m.drm.hdisplay ||
                    ci->imageExtent.height != m.drm.vdisplay)
                    return VK_ERROR_FEATURE_NOT_PRESENT;
                try {
                    auto s = std::make_shared<Surface>(Surface{i->display, &m, ci->imageExtent});
                    *out = opaque<VkSurfaceKHR>(s.get());
                    std::lock_guard<std::mutex> g(registryMutex);
                    surfaces.emplace(*out, s);
                    return VK_SUCCESS;
                } catch (const std::exception &e) {
                    return report(e);
                }
            }
    FORWARD_I(vkCreateDisplayPlaneSurfaceKHR, instance, instance, ci, a, out);
}
VKENTRY void VKAPI_CALL destroySurface(VkInstance instance, VkSurfaceKHR surface,
                                       const VkAllocationCallbacks *a) {
    {
        std::lock_guard<std::mutex> g(registryMutex);
        if (surfaces.erase(surface))
            return;
    }
    auto i = instanceFor(instance);
    i->fn<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR")(instance, surface, a);
}
VKENTRY VkResult VKAPI_CALL surfaceSupport(VkPhysicalDevice physical, uint32_t family,
                                           VkSurfaceKHR surface, VkBool32 *out) {
    if (surfaceFor(surface)) {
        auto i = instanceFor(physical);
        uint32_t count = 0;
        auto query = i->fn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            "vkGetPhysicalDeviceQueueFamilyProperties");
        query(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        query(physical, &count, families.data());
        *out = family < count &&
               (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT));
        return VK_SUCCESS;
    }
    FORWARD_I(vkGetPhysicalDeviceSurfaceSupportKHR, physical, physical, family, surface, out);
}
static bool sharedFormatSupported(VkPhysicalDevice physical, VkFormat format,
                                  VkImageUsageFlags usage) {
    auto i = instanceFor(physical);
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifier{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
    modifier.drmFormatModifier = DRM_FORMAT_MOD_LINEAR;
    modifier.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkPhysicalDeviceExternalImageFormatInfo external{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    external.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    external.pNext = &modifier;
    VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    query.pNext = &external;
    query.type = VK_IMAGE_TYPE_2D;
    query.format = format;
    query.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    query.usage = usage;
    VkExternalImageFormatProperties memory{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    properties.pNext = &memory;
    auto result = i->fn<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        "vkGetPhysicalDeviceImageFormatProperties2")(physical, &query, &properties);
    return result == VK_SUCCESS && (memory.externalMemoryProperties.externalMemoryFeatures &
                                    VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT);
}
static std::vector<VkSurfaceFormatKHR> sharedFormats(VkPhysicalDevice physical,
                                                     const Surface &surface) {
    std::vector<VkSurfaceFormatKHR> formats;
    for (auto pair : {std::pair{VK_FORMAT_B8G8R8A8_UNORM, DRM_FORMAT_ARGB8888},
                      std::pair{VK_FORMAT_R8G8B8A8_UNORM, DRM_FORMAT_ABGR8888}}) {
        const auto &planes = surface.display->formats;
        if (std::find(planes.begin(), planes.end(), pair.second) != planes.end() &&
            sharedFormatSupported(physical, pair.first, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
            formats.push_back({pair.first, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR});
    }
    return formats;
}
VKENTRY VkResult VKAPI_CALL surfaceCapabilities(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                                VkSurfaceCapabilitiesKHR *out) {
    if (auto s = surfaceFor(surface)) {
        *out = {};
        out->minImageCount = 2;
        out->maxImageCount = 3;
        out->currentExtent = out->minImageExtent = out->maxImageExtent = s->extent;
        out->maxImageArrayLayers = 1;
        out->supportedTransforms = out->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        out->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        out->supportedUsageFlags = 0;
        const auto formats = sharedFormats(physical, *s);
        if (formats.empty())
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        for (auto usage : {VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_USAGE_STORAGE_BIT,
                           VK_IMAGE_USAGE_SAMPLED_BIT}) {
            if (std::all_of(formats.begin(), formats.end(), [&](const auto &f) {
                    return sharedFormatSupported(physical, f.format,
                                                 usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
                }))
                out->supportedUsageFlags |= usage;
        }
        return VK_SUCCESS;
    }
    FORWARD_I(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, physical, physical, surface, out);
}
VKENTRY VkResult VKAPI_CALL surfaceCapabilitiesEXT(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                                   VkSurfaceCapabilities2EXT *out) {
    if (surfaceFor(surface)) {
        VkSurfaceCapabilitiesKHR caps{};
        auto result = surfaceCapabilities(physical, surface, &caps);
        out->minImageCount = caps.minImageCount;
        out->maxImageCount = caps.maxImageCount;
        out->currentExtent = caps.currentExtent;
        out->minImageExtent = caps.minImageExtent;
        out->maxImageExtent = caps.maxImageExtent;
        out->maxImageArrayLayers = caps.maxImageArrayLayers;
        out->supportedTransforms = caps.supportedTransforms;
        out->currentTransform = caps.currentTransform;
        out->supportedCompositeAlpha = caps.supportedCompositeAlpha;
        out->supportedUsageFlags = caps.supportedUsageFlags;
        out->supportedSurfaceCounters = VK_SURFACE_COUNTER_VBLANK_BIT_EXT;
        return result;
    }
    FORWARD_I(vkGetPhysicalDeviceSurfaceCapabilities2EXT, physical, physical, surface, out);
}
VKENTRY VkResult VKAPI_CALL surfaceFormats(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                           uint32_t *count, VkSurfaceFormatKHR *out) {
    if (auto s = surfaceFor(surface)) {
        auto formats = sharedFormats(physical, *s);
        if (formats.empty())
            return VK_ERROR_FORMAT_NOT_SUPPORTED;
        return enumerate(formats, count, out);
    }
    FORWARD_I(vkGetPhysicalDeviceSurfaceFormatsKHR, physical, physical, surface, count, out);
}
VKENTRY VkResult VKAPI_CALL presentModes(VkPhysicalDevice physical, VkSurfaceKHR surface,
                                         uint32_t *count, VkPresentModeKHR *out) {
    if (surfaceFor(surface))
        return enumerate(std::vector<VkPresentModeKHR>{VK_PRESENT_MODE_FIFO_KHR}, count, out);
    FORWARD_I(vkGetPhysicalDeviceSurfacePresentModesKHR, physical, physical, surface, count, out);
}
template <class Basic, class Extended, class Query>
VkResult extendedProperties(uint32_t *count, Extended *out, Query query) {
    if (!out)
        return query(count, nullptr);
    if (!*count) {
        uint32_t available = 0;
        auto result = query(&available, nullptr);
        return result != VK_SUCCESS ? result : available ? VK_INCOMPLETE : VK_SUCCESS;
    }
    std::vector<Basic> values(*count);
    auto result = query(count, values.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return result;
    for (uint32_t n = 0; n < *count; ++n) {
        if constexpr (std::is_same_v<Extended, VkDisplayProperties2KHR>)
            out[n].displayProperties = values[n];
        else if constexpr (std::is_same_v<Extended, VkDisplayPlaneProperties2KHR>)
            out[n].displayPlaneProperties = values[n];
        else if constexpr (std::is_same_v<Extended, VkSurfaceFormat2KHR>)
            out[n].surfaceFormat = values[n];
        else
            out[n].displayModeProperties = values[n];
    }
    return result;
}
VKENTRY VkResult VKAPI_CALL displayProperties2(VkPhysicalDevice p, uint32_t *n,
                                               VkDisplayProperties2KHR *out) {
    return extendedProperties<VkDisplayPropertiesKHR>(
        n, out, [&](uint32_t *c, VkDisplayPropertiesKHR *v) { return displayProperties(p, c, v); });
}
VKENTRY VkResult VKAPI_CALL planeProperties2(VkPhysicalDevice p, uint32_t *n,
                                             VkDisplayPlaneProperties2KHR *out) {
    return extendedProperties<VkDisplayPlanePropertiesKHR>(
        n, out,
        [&](uint32_t *c, VkDisplayPlanePropertiesKHR *v) { return planeProperties(p, c, v); });
}
VKENTRY VkResult VKAPI_CALL modeProperties2(VkPhysicalDevice p, VkDisplayKHR display, uint32_t *n,
                                            VkDisplayModeProperties2KHR *out) {
    return extendedProperties<VkDisplayModePropertiesKHR>(
        n, out, [&](uint32_t *c, VkDisplayModePropertiesKHR *v) {
            return modeProperties(p, display, c, v);
        });
}
VKENTRY VkResult VKAPI_CALL planeCapabilities2(VkPhysicalDevice p,
                                               const VkDisplayPlaneInfo2KHR *info,
                                               VkDisplayPlaneCapabilities2KHR *out) {
    return planeCapabilities(p, info->mode, info->planeIndex, &out->capabilities);
}
VKENTRY VkResult VKAPI_CALL surfaceCapabilities2(VkPhysicalDevice p,
                                                 const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                                 VkSurfaceCapabilities2KHR *out) {
    if (!surfaceFor(info->surface)) {
        FORWARD_I(vkGetPhysicalDeviceSurfaceCapabilities2KHR, p, p, info, out);
    }
    if (info->pNext || out->pNext)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    return surfaceCapabilities(p, info->surface, &out->surfaceCapabilities);
}
VKENTRY VkResult VKAPI_CALL surfaceFormats2(VkPhysicalDevice p,
                                            const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                            uint32_t *n, VkSurfaceFormat2KHR *out) {
    if (!surfaceFor(info->surface)) {
        FORWARD_I(vkGetPhysicalDeviceSurfaceFormats2KHR, p, p, info, n, out);
    }
    return extendedProperties<VkSurfaceFormatKHR>(
        n, out, [&](uint32_t *count, VkSurfaceFormatKHR *values) {
            return surfaceFormats(p, info->surface, count, values);
        });
}
VKENTRY VkResult VKAPI_CALL createSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                            const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    auto surface = surfaceFor(ci->surface);
    if (!surface) {
        FORWARD_D(vkCreateSwapchainKHR, device, device, ci, a, out);
    }
    try {
        auto d = deviceFor(device);
        auto previous = chainFor(ci->oldSwapchain);
        {
            std::lock_guard<std::mutex> g(registryMutex);
            for (auto &entry : chains)
                if (entry.second->surface->display == surface->display && entry.second->presenter &&
                    entry.second != previous)
                    throw Error(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR,
                                "Display already has an active swapchain");
        }
        if (previous) {
            if (previous->d != d || previous->surface->display != surface->display)
                throw Error(VK_ERROR_INITIALIZATION_FAILED,
                            "Old swapchain belongs to another display/device");
            check(d->DeviceWaitIdle(device), "Retire old swapchain");
            previous->presenter.reset();
        }
        auto chain = std::make_shared<Swapchain>();
        chain->d = d;
        chain->surface = surface;
        chain->initialize(*ci);
        *out = opaque<VkSwapchainKHR>(chain.get());
        std::lock_guard<std::mutex> g(registryMutex);
        for (auto &image : chain->images)
            ownedImages.insert(image->handle);
        chains.emplace(*out, chain);
        return VK_SUCCESS;
    } catch (const std::exception &e) {
        return report(e);
    }
}
VKENTRY void VKAPI_CALL destroySwapchain(VkDevice device, VkSwapchainKHR handle,
                                         const VkAllocationCallbacks *a) {
    std::shared_ptr<Swapchain> retired;
    {
        std::lock_guard<std::mutex> g(registryMutex);
        auto p = chains.find(handle);
        if (p != chains.end()) {
            retired = std::move(p->second);
            for (auto &image : retired->images)
                ownedImages.erase(image->handle);
            chains.erase(p);
        }
    }
    if (retired)
        return;
    auto d = deviceFor(device);
    d->fn<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR")(device, handle, a);
}
VKENTRY VkResult VKAPI_CALL swapchainImages(VkDevice device, VkSwapchainKHR handle, uint32_t *n,
                                            VkImage *out) {
    if (auto c = chainFor(handle)) {
        std::vector<VkImage> values;
        for (auto &i : c->images)
            values.push_back(i->handle);
        return enumerate(values, n, out);
    }
    FORWARD_D(vkGetSwapchainImagesKHR, device, device, handle, n, out);
}
VKENTRY VkResult VKAPI_CALL acquireImage(VkDevice device, VkSwapchainKHR handle, uint64_t timeout,
                                         VkSemaphore semaphore, VkFence fence, uint32_t *out) {
    if (auto c = chainFor(handle)) {
        if (!c->presenter)
            return VK_ERROR_OUT_OF_DATE_KHR;
        try {
            return c->acquire(timeout, semaphore, fence, out);
        } catch (const std::exception &e) {
            return report(e);
        }
    }
    FORWARD_D(vkAcquireNextImageKHR, device, device, handle, timeout, semaphore, fence, out);
}
VKENTRY VkResult VKAPI_CALL acquireImage2(VkDevice device, const VkAcquireNextImageInfoKHR *info,
                                          uint32_t *out) {
    if (chainFor(info->swapchain)) {
        if (info->deviceMask != 1 || info->pNext)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        return acquireImage(device, info->swapchain, info->timeout, info->semaphore, info->fence,
                            out);
    }
    FORWARD_D(vkAcquireNextImage2KHR, device, device, info, out);
}
VKENTRY VkResult VKAPI_CALL presentImages(VkQueue queue, const VkPresentInfoKHR *info) {
    std::shared_ptr<Swapchain> owned;
    for (uint32_t n = 0; n < info->swapchainCount; ++n)
        if (auto c = chainFor(info->pSwapchains[n]))
            owned = c;
    if (!owned) {
        FORWARD_D(vkQueuePresentKHR, queue, queue, info);
    }
    if (info->swapchainCount != 1)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!owned->presenter)
        return VK_ERROR_OUT_OF_DATE_KHR;
    VkResult result;
    try {
        result = owned->present(queue, *info);
    } catch (const std::exception &e) {
        result = report(e);
    }
    if (info->pResults)
        info->pResults[0] = result;
    return result;
}
VKENTRY VkResult VKAPI_CALL waitPresent(VkDevice device, VkSwapchainKHR handle, uint64_t id,
                                        uint64_t timeout) {
    if (auto c = chainFor(handle)) {
        return c->presenter ? c->presenter->wait(id, timeout) : VK_ERROR_OUT_OF_DATE_KHR;
    }
    FORWARD_D(vkWaitForPresentKHR, device, device, handle, id, timeout);
}
VKENTRY VkResult VKAPI_CALL swapchainCounter(VkDevice device, VkSwapchainKHR handle,
                                             VkSurfaceCounterFlagBitsEXT counter, uint64_t *out) {
    if (auto c = chainFor(handle)) {
        if (counter != VK_SURFACE_COUNTER_VBLANK_BIT_EXT)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        return c->presenter ? c->presenter->counter(out) : VK_ERROR_OUT_OF_DATE_KHR;
    }
    FORWARD_D(vkGetSwapchainCounterEXT, device, device, handle, counter, out);
}
VKENTRY VkResult VKAPI_CALL registerDisplayEvent(VkDevice device, VkDisplayKHR display,
                                                 const VkDisplayEventInfoEXT *info,
                                                 const VkAllocationCallbacks *a, VkFence *out) {
    auto d = deviceFor(device);
    if (d->instance->display && display == opaque<VkDisplayKHR>(d->instance->display.get()))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    FORWARD_D(vkRegisterDisplayEventEXT, device, device, display, info, a, out);
}
VKENTRY VkResult VKAPI_CALL refreshDuration(VkDevice device, VkSwapchainKHR handle,
                                            VkRefreshCycleDurationGOOGLE *out) {
    if (auto chain = chainFor(handle)) {
        out->refreshDuration =
            static_cast<uint64_t>(1000000000.0 / Display::hz(*chain->surface->mode) + .5);
        return VK_SUCCESS;
    }
    FORWARD_D(vkGetRefreshCycleDurationGOOGLE, device, device, handle, out);
}
VKENTRY VkResult VKAPI_CALL pastTimings(VkDevice device, VkSwapchainKHR handle, uint32_t *count,
                                        VkPastPresentationTimingGOOGLE *out) {
    if (chainFor(handle)) {
        *count = 0;
        return VK_SUCCESS;
    }
    FORWARD_D(vkGetPastPresentationTimingGOOGLE, device, device, handle, count, out);
}
VKENTRY VkResult VKAPI_CALL displayPower(VkDevice device, VkDisplayKHR display,
                                         const VkDisplayPowerInfoEXT *info) {
    auto d = deviceFor(device);
    if (d->instance->display && display == opaque<VkDisplayKHR>(d->instance->display.get()))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    FORWARD_D(vkDisplayPowerControlEXT, device, device, display, info);
}
static bool transportImage(VkImage image) {
    std::lock_guard<std::mutex> g(registryMutex);
    return ownedImages.count(image);
}
template <class Barrier> static void translate(Barrier &b) {
    if (b.oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    if (b.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    for (auto *family : {&b.srcQueueFamilyIndex, &b.dstQueueFamilyIndex})
        if (*family != VK_QUEUE_FAMILY_EXTERNAL && *family != VK_QUEUE_FAMILY_FOREIGN_EXT)
            *family = VK_QUEUE_FAMILY_IGNORED;
}
VKENTRY void VKAPI_CALL pipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags source,
                                        VkPipelineStageFlags target, VkDependencyFlags flags,
                                        uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                        const VkBufferMemoryBarrier *b, uint32_t ni,
                                        const VkImageMemoryBarrier *images) {
    auto d = deviceFor(cb);
    bool rewrite = false;
    for (uint32_t n = 0; n < ni; ++n)
        rewrite |= transportImage(images[n].image);
    if (!rewrite) {
        d->CmdPipelineBarrier(cb, source, target, flags, nm, m, nb, b, ni, images);
        return;
    }
    thread_local std::vector<VkImageMemoryBarrier> values;
    values.assign(images, images + ni);
    for (auto &barrier : values)
        if (transportImage(barrier.image))
            translate(barrier);
    d->CmdPipelineBarrier(cb, source, target, flags, nm, m, nb, b, ni, values.data());
}
VKENTRY void VKAPI_CALL pipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *info) {
    auto d = deviceFor(cb);
    auto call = d->fn<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2");
    if (!call)
        call = d->fn<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2KHR");
    bool rewrite = false;
    for (uint32_t n = 0; n < info->imageMemoryBarrierCount; ++n)
        rewrite |= transportImage(info->pImageMemoryBarriers[n].image);
    if (!rewrite) {
        call(cb, info);
        return;
    }
    thread_local std::vector<VkImageMemoryBarrier2> values;
    values.assign(info->pImageMemoryBarriers,
                  info->pImageMemoryBarriers + info->imageMemoryBarrierCount);
    for (auto &b : values)
        if (transportImage(b.image))
            translate(b);
    auto local = *info;
    local.pImageMemoryBarriers = values.data();
    call(cb, &local);
}
static PFN_vkVoidFunction hook(const char *name) {
#define H(api, function)                                                                           \
    if (!strcmp(name, #api))                                                                       \
    return reinterpret_cast<PFN_vkVoidFunction>(function)
    H(vkGetDrmDisplayEXT, getDisplay);
    H(vkAcquireDrmDisplayEXT, acquireDisplay);
    H(vkReleaseDisplayEXT, releaseDisplay);
    H(vkGetPhysicalDeviceDisplayPropertiesKHR, displayProperties);
    H(vkGetPhysicalDeviceDisplayProperties2KHR, displayProperties2);
    H(vkGetPhysicalDeviceDisplayPlanePropertiesKHR, planeProperties);
    H(vkGetPhysicalDeviceDisplayPlaneProperties2KHR, planeProperties2);
    H(vkGetDisplayPlaneSupportedDisplaysKHR, supportedDisplays);
    H(vkGetDisplayModePropertiesKHR, modeProperties);
    H(vkGetDisplayModeProperties2KHR, modeProperties2);
    H(vkCreateDisplayModeKHR, createMode);
    H(vkGetDisplayPlaneCapabilitiesKHR, planeCapabilities);
    H(vkGetDisplayPlaneCapabilities2KHR, planeCapabilities2);
    H(vkCreateDisplayPlaneSurfaceKHR, createSurface);
    H(vkDestroySurfaceKHR, destroySurface);
    H(vkGetPhysicalDeviceSurfaceSupportKHR, surfaceSupport);
    H(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, surfaceCapabilities);
    H(vkGetPhysicalDeviceSurfaceCapabilities2KHR, surfaceCapabilities2);
    H(vkGetPhysicalDeviceSurfaceCapabilities2EXT, surfaceCapabilitiesEXT);
    H(vkGetPhysicalDeviceSurfaceFormatsKHR, surfaceFormats);
    H(vkGetPhysicalDeviceSurfaceFormats2KHR, surfaceFormats2);
    H(vkGetPhysicalDeviceSurfacePresentModesKHR, presentModes);
    H(vkCreateSwapchainKHR, createSwapchain);
    H(vkDestroySwapchainKHR, destroySwapchain);
    H(vkGetSwapchainImagesKHR, swapchainImages);
    H(vkAcquireNextImageKHR, acquireImage);
    H(vkAcquireNextImage2KHR, acquireImage2);
    H(vkQueuePresentKHR, presentImages);
    H(vkWaitForPresentKHR, waitPresent);
    H(vkGetSwapchainCounterEXT, swapchainCounter);
    H(vkRegisterDisplayEventEXT, registerDisplayEvent);
    H(vkGetRefreshCycleDurationGOOGLE, refreshDuration);
    H(vkGetPastPresentationTimingGOOGLE, pastTimings);
    H(vkDisplayPowerControlEXT, displayPower);
    H(vkCmdPipelineBarrier, pipelineBarrier);
    H(vkCmdPipelineBarrier2, pipelineBarrier2);
    H(vkCmdPipelineBarrier2KHR, pipelineBarrier2);
#undef H
    return nullptr;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL minimalGIPA(VkInstance instance,
                                                                const char *name) {
    if (!strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(minimalGIPA);
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(minimalGDPA);
    if (!strcmp(name, "vkCreateInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(makeInstance);
    if (!strcmp(name, "vkDestroyInstance"))
        return reinterpret_cast<PFN_vkVoidFunction>(dropInstance);
    if (!strcmp(name, "vkCreateDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(makeDevice);
    if (!strcmp(name, "vkDestroyDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(dropDevice);
    auto i = instanceFor(instance);
    if (i && i->enabled)
        if (auto function = hook(name))
            return function;
    return i ? i->next(instance, name) : nullptr;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL minimalGDPA(VkDevice device, const char *name) {
    if (!strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(minimalGDPA);
    if (!strcmp(name, "vkDestroyDevice"))
        return reinterpret_cast<PFN_vkVoidFunction>(dropDevice);
    auto d = deviceFor(device);
    if (d && d->instance->enabled)
        if (auto function = hook(name))
            return function;
    return d ? d->next(device, name) : nullptr;
}
extern "C" __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (v->loaderLayerInterfaceVersion > 2)
        v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = minimalGIPA;
    v->pfnGetDeviceProcAddr = minimalGDPA;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}
