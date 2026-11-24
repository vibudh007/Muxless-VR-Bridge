#include "transport.hpp"
#include "desktop.hpp"
#include "display-event.hpp"
#include "output-audit.hpp"
#include "virtual-display.hpp"
#include "../runtime/profile_session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <sys/types.h>
#include <unistd.h>

namespace v3 {
namespace {

struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr next{};
    PFN_GetPhysicalDeviceProcAddr physicalProc{};
    uint32_t apiVersion{VK_API_VERSION_1_0};
};

struct Device {
    std::shared_ptr<Instance> instance;
    Vulkan vk{};
    bool bridge{};
    std::shared_ptr<void> profileLease;
    PFN_vkSetDeviceLoaderData loader{};
    uint32_t renderFamily{};
    uint32_t acquireFamily{};
    uint32_t copyFamily{};
    std::vector<VkQueue> renderQueues;
    std::vector<uint32_t> appFamilies;
    VkQueue acquireQueue{};
    VkQueue copyQueue{};
    Transport transport{};
    const char *transportName{};
    std::shared_ptr<std::mutex> privateQueueMutex;
    bool sharedCopy{};
    std::unique_ptr<DesktopVblank> desktopVblank;
    std::mutex displayFenceMutex;
    std::unordered_map<VkFence, std::shared_ptr<DisplayEvent>> displayFences;
    std::shared_ptr<DisplayEventNotifier> displayEventNotifier;
    std::atomic_bool hasDisplayFences{};
    PFN_vkCmdPipelineBarrier barrier{};
    PFN_vkCmdPipelineBarrier2 barrier2{};
    std::atomic_flag displayEventFault = ATOMIC_FLAG_INIT;
};

std::shared_mutex objects;
std::mutex headsetSwapchains;
struct ActiveOutput {
    VkDevice device;
    VkSurfaceKHR surface;
    VkDisplayKHR display;
    std::weak_ptr<FrameRing> ring;
};
std::map<std::pair<dev_t, uint32_t>, ActiveOutput> activeHeadsetRings;
std::unordered_map<void *, std::shared_ptr<Instance>> instances;
std::unordered_map<void *, std::shared_ptr<Device>> devices;
std::unordered_map<VkSwapchainKHR, std::shared_ptr<FrameRing>> rings;
std::unordered_map<VkImage, std::weak_ptr<FrameRing>> ringImages;

template<class T> void *dispatchKey(T object) {
    return object ? *reinterpret_cast<void **>(object) : nullptr;
}

template<class Map, class Key> auto find(const Map &map, Key key) {
    const std::shared_lock lock(objects);
    const auto entry = map.find(key);
    return entry == map.end() ? typename Map::mapped_type{} : entry->second;
}

template<class T> auto instanceFor(T object) { return find(instances, dispatchKey(object)); }
template<class T> auto deviceFor(T object) { return find(devices, dispatchKey(object)); }

struct Configuration {
    const char *name{};
    Transport mode{};
    bool invalid{};
    bool automatic{};
};

const Configuration &configuration() {
    static const auto config = []() -> Configuration {
        const auto *mode = std::getenv("V3_TRANSPORT");
        const auto *activate = std::getenv("V3_ENABLE_LAYER");
        if (!mode && (!activate || std::strcmp(activate, "1")))
            return {};
        std::array<char, 4096> path{};
        const auto size = readlink("/proc/self/exe", path.data(), path.size());
        if (size <= 0 || static_cast<size_t>(size) == path.size()) {
            std::fputs("wmr-v3: preflight configuration: cannot identify the executable from /proc/self/exe\n", stderr);
            return {};
        }
        const std::string_view full(path.data(), size);
        const auto name = full.substr(full.find_last_of('/') + 1);
        if (name != "vrcompositor" && name != "vrcompositor.real"
#ifdef MUXLESS_DESKTOP_FIXTURE
            && name != "v3-window-smoke"
#endif
        )
            return {};

        constexpr Configuration modes[] = {{"auto", Transport::copy, false, true},
                                           {"direct", Transport::direct}, {"copy", Transport::copy},
                                           {"peer", Transport::peer},
                                           {"ace", Transport::ace}};
        const auto selected = std::find_if(std::begin(modes), std::end(modes), [mode](const auto &entry) {
            return mode && !std::strcmp(mode, entry.name);
        });
        const auto invalid = [](const char *setting, const char *value, const char *choices) {
            std::fprintf(stderr, "wmr-v3: preflight configuration: %s=\"%.96s\"; expected %s\n",
                         setting, value ? value : "<unset>", choices);
            return Configuration{nullptr, {}, true};
        };
        const auto *output = std::getenv("V3_OUTPUT");
#ifndef MUXLESS_DESKTOP_FIXTURE
        if (output && *output && std::strcmp(output, "headset"))
            return invalid("V3_OUTPUT", output, "headset");
#else
        if (output && *output && std::strcmp(output, "headset") && std::strcmp(output, "desktop"))
            return invalid("V3_OUTPUT", output, "headset or desktop");
#endif
        if (headsetOutput())
            return {"sync-file", Transport::copy};
        if (selected == std::end(modes))
            return invalid("V3_TRANSPORT", mode, "auto, direct, copy, peer, or ace");
        const auto *clock = std::getenv("V3_VBLANK");
        if (clock && *clock && std::strcmp(clock, "virtual90") && std::strcmp(clock, "drm"))
            return invalid("V3_VBLANK", clock, "drm or virtual90");
        return *selected;
    }();
    return config;
}

bool enabled() { return configuration().name != nullptr; }

std::string_view selectedDeviceUuid() {
    const auto *value = std::getenv("MUXLESS_RENDER_DEVICE_UUID");
    return value ? std::string_view(value) : std::string_view{};
}

bool matchesRenderDevice(const Vulkan &vk) {
    VkPhysicalDeviceProperties properties{};
    const auto wanted = selectedDeviceUuid();
    if (wanted.empty()) {
        const auto query = vk.physicalFn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
        if (!query)
            return false;
        query(vk.physical, &properties);
        return properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    }

    auto query = vk.physicalFn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
    if (!query)
        query = vk.physicalFn<PFN_vkGetPhysicalDeviceProperties2KHR>("vkGetPhysicalDeviceProperties2KHR");
    if (!query || wanted.size() != 2 * VK_UUID_SIZE)
        return false;
    VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids};
    query(vk.physical, &props);
    if (props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < VK_UUID_SIZE; ++i)
        if (wanted[2 * i] != hex[ids.deviceUUID[i] >> 4] || wanted[2 * i + 1] != hex[ids.deviceUUID[i] & 15])
            return false;
    return true;
}

std::vector<VkExtensionProperties> deviceExtensions(const Vulkan &vk) {
    const auto query = vk.physicalFn<PFN_vkEnumerateDeviceExtensionProperties>(
        "vkEnumerateDeviceExtensionProperties");
    if (!query)
        throw std::runtime_error("Vulkan is missing vkEnumerateDeviceExtensionProperties");
    VkResult result = VK_INCOMPLETE;
    for (unsigned attempt = 0; attempt < 3 && result == VK_INCOMPLETE; ++attempt) {
        uint32_t count = 0;
        result = query(vk.physical, nullptr, &count, nullptr);
        if (result != VK_SUCCESS)
            break;
        if (!count)
            return {};
        std::vector<VkExtensionProperties> extensions(count);
        result = query(vk.physical, nullptr, &count, extensions.data());
        if (result == VK_SUCCESS) {
            extensions.resize(count);
            return extensions;
        }
    }
    throw std::runtime_error("Cannot enumerate render GPU extensions (VkResult " + std::to_string(result) + ")");
}

bool hasExtension(const std::vector<VkExtensionProperties> &extensions, const char *name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto &entry) {
        return !std::strcmp(entry.extensionName, name);
    });
}

const char *copyClockExtension(const Vulkan &vk, const std::vector<VkExtensionProperties> &extensions) {
    const auto *trace = std::getenv("V3_COPY_TIMING");
    if (!trace || !*trace)
        return nullptr;
    const std::pair<const char *, const char *> clocks[] = {
        {VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR"},
        {VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"},
    };
    for (const auto &[name, entry] : clocks) {
        if (!hasExtension(extensions, name))
            continue;
        const auto domains = vk.physicalFn<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR>(entry);
        uint32_t domainCount = 0;
        if (!domains || domains(vk.physical, &domainCount, nullptr) != VK_SUCCESS)
            continue;
        std::vector<VkTimeDomainKHR> values(domainCount);
        if (domains(vk.physical, &domainCount, values.data()) == VK_SUCCESS &&
            std::find(values.begin(), values.end(), VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR) != values.end() &&
            std::find(values.begin(), values.end(), VK_TIME_DOMAIN_DEVICE_KHR) != values.end())
            return name;
    }
    return nullptr;
}

VkResult failure(const char *where, const std::exception &error) {
    std::fprintf(stderr, "wmr-v3: %s: %s\n", where, error.what());
    return dynamic_cast<const std::bad_alloc *>(&error) ? VK_ERROR_OUT_OF_HOST_MEMORY
                                                        : VK_ERROR_INITIALIZATION_FAILED;
}

void logQueueFamilies(const std::vector<VkQueueFamilyProperties> &families,
                      const std::vector<VkDeviceQueueCreateInfo> &requested) {
    for (size_t index = 0; index < families.size(); ++index) {
        uint64_t occupied = 0;
        for (const auto &queue : requested)
            if (queue.queueFamilyIndex == index)
                occupied += queue.queueCount;
        const auto &family = families[index];
        std::fprintf(stderr, "wmr-v3: preflight queue family %zu: flags=%#x queues=%u reserved=%llu\n",
                     index, family.queueFlags, family.queueCount, static_cast<unsigned long long>(occupied));
    }
}

#define ENTRY static VKAPI_ATTR
#define DEVICE(name) d->vk.deviceFn<PFN_vk##name>("vk" #name)

ENTRY VkResult VKAPI_CALL createInstance(const VkInstanceCreateInfo *ci,
                                         const VkAllocationCallbacks *allocator,
                                         VkInstance *out) {
    const auto &config = configuration();
    if (config.invalid)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto *link = const_cast<VkLayerInstanceCreateInfo *>(
        static_cast<const VkLayerInstanceCreateInfo *>(ci->pNext));
    while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                    link->function != VK_LAYER_LINK_INFO))
        link = const_cast<VkLayerInstanceCreateInfo *>(
            static_cast<const VkLayerInstanceCreateInfo *>(link->pNext));
    if (!link || !link->u.pLayerInfo) {
        if (config.name)
            std::fputs("wmr-v3: preflight instance: Vulkan loader did not provide the next layer link\n", stderr);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto pNext = link->u.pLayerInfo;
    const auto next = pNext->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = pNext->pNext;
    const auto call = next ? reinterpret_cast<PFN_vkCreateInstance>(next(nullptr, "vkCreateInstance")) : nullptr;
    if (!call) {
        if (config.name)
            std::fputs("wmr-v3: preflight instance: Vulkan loader is missing vkCreateInstance\n", stderr);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    try {
        auto instance = std::make_shared<Instance>();
        if (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion)
            instance->apiVersion = ci->pApplicationInfo->apiVersion;
        if (config.name)
            std::fprintf(stderr, "wmr-v3: preflight configuration: transport=%s output=%s timing=%s\n",
                         config.name, headsetOutput() ? "headset" : "desktop",
                         headsetOutput() ? "DRM lease" : (virtualRefreshClock() ? "virtual 90 Hz" : "DRM vblank"));
        auto result = call(ci, allocator, out);
        if (result != VK_SUCCESS) {
            if (config.name)
                std::fprintf(stderr, "wmr-v3: preflight instance: downstream vkCreateInstance returned %d\n", result);
            return result;
        }
        instance->handle = *out;
        instance->next = next;
        instance->physicalProc = pNext->pfnNextGetPhysicalDeviceProcAddr;
        try {
            const std::unique_lock lock(objects);
            instances.emplace(dispatchKey(*out), instance);
        } catch (...) {
            reinterpret_cast<PFN_vkDestroyInstance>(next(*out, "vkDestroyInstance"))(*out, allocator);
            throw;
        }
        return VK_SUCCESS;
    } catch (const std::exception &error) {
        return failure("CreateInstance", error);
    }
}

ENTRY void VKAPI_CALL destroyInstance(VkInstance handle, const VkAllocationCallbacks *allocator) {
    if (!handle)
        return;
    const auto instance = instanceFor(handle);
    releaseVirtualDisplays(handle);
    {
        const std::unique_lock lock(objects);
        instances.erase(dispatchKey(handle));
    }
    reinterpret_cast<PFN_vkDestroyInstance>(instance->next(handle, "vkDestroyInstance"))(handle, allocator);
}

ENTRY VkResult VKAPI_CALL enumeratePhysicalDevices(VkInstance instance, uint32_t *count, VkPhysicalDevice *out) {
    const auto i = instanceFor(instance);
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(i->next(instance, "vkEnumeratePhysicalDevices"));
    if (!out || selectedDeviceUuid().empty())
        return enumerate(instance, count, out);
    try {
        uint32_t total = 0;
        auto result = enumerate(instance, &total, nullptr);
        if (result != VK_SUCCESS || !total) {
            *count = total;
            return result;
        }
        std::vector<VkPhysicalDevice> available(total);
        result = enumerate(instance, &total, available.data());
        if (result != VK_SUCCESS && result != VK_INCOMPLETE)
            return result;
        available.resize(total);
        const auto selected = std::find_if(available.begin(), available.end(), [&](VkPhysicalDevice gpu) {
            return matchesRenderDevice({gpu, {}, i->next, instance, {}});
        });
        if (selected != available.end())
            std::rotate(available.begin(), selected, std::next(selected));
        *count = std::min(*count, total);
        std::copy_n(available.begin(), *count, out);
        return *count < total ? VK_INCOMPLETE : result;
    } catch (const std::exception &error) {
        return failure("EnumeratePhysicalDevices", error);
    }
}

ENTRY VkResult VKAPI_CALL createDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *ci,
                                       const VkAllocationCallbacks *allocator, VkDevice *out) {
    const auto instance = instanceFor(physical);
    if (!instance)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceCreateInfo *link = nullptr;
    PFN_vkSetDeviceLoaderData loader = nullptr;
    for (auto *p = static_cast<const VkLayerDeviceCreateInfo *>(ci->pNext); p;
         p = static_cast<const VkLayerDeviceCreateInfo *>(p->pNext)) {
        if (p->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            continue;
        if (p->function == VK_LAYER_LINK_INFO)
            link = const_cast<VkLayerDeviceCreateInfo *>(p);
        if (p->function == VK_LOADER_DATA_CALLBACK)
            loader = p->u.pfnSetDeviceLoaderData;
    }
    if (!link || !link->u.pLayerInfo) {
        if (enabled())
            std::fputs("wmr-v3: preflight device: Vulkan loader did not provide the next device link\n", stderr);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const auto next = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    const auto nextInstance = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    const auto create = reinterpret_cast<PFN_vkCreateDevice>(
        nextInstance ? nextInstance(instance->handle, "vkCreateDevice") : nullptr);
    if (!next || !create) {
        if (enabled())
            std::fputs("wmr-v3: preflight device: Vulkan loader is missing vkCreateDevice or device dispatch\n", stderr);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    try {
        auto d = std::make_shared<Device>();
        d->instance = instance;
        d->vk = {physical, {}, instance->next, instance->handle, next};
        d->bridge = enabled() && (!headsetOutput() || matchesRenderDevice(d->vk));
        d->loader = loader;
        VkDeviceCreateInfo request = *ci;
        std::vector<const char *> extensions;
        std::vector<VkDeviceQueueCreateInfo> queues;
        std::vector<float> acquirePriorities;
        std::vector<float> copyPriorities;
        uint32_t acquireIndex = 0;
        uint32_t copyIndex = 0;
        bool privateCopy = false;
        const char *calibration = nullptr;
        VkPhysicalDeviceDrmPropertiesEXT renderNode{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceSynchronization2Features sync2{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
        VkPhysicalDeviceTimelineSemaphoreFeatures timeline{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};

        if (d->bridge) {
            d->privateQueueMutex = std::make_shared<std::mutex>();
            if (headsetOutput())
                d->displayEventNotifier = std::make_shared<DisplayEventNotifier>();
            VkPhysicalDeviceProperties props{};
            const auto properties = d->vk.physicalFn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
            if (!properties)
                throw std::runtime_error("Vulkan is missing vkGetPhysicalDeviceProperties");
            properties(physical, &props);
            const uint32_t api = std::min(props.apiVersion, instance->apiVersion);
            std::fprintf(stderr, "wmr-v3: preflight render device: %s [%04x:%04x] type=%u Vulkan=%u.%u\n",
                         props.deviceName, props.vendorID, props.deviceID, unsigned(props.deviceType),
                         VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api));
            if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
                throw std::runtime_error("The selected render device is not a discrete GPU; check the compositor GPU selection");
            if (!loader)
                throw std::runtime_error("Vulkan loader lacks the callback for registering bridge command buffers");
            struct Extension { const char *name; uint32_t promoted{}; };
            constexpr Extension required[] = {
                {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME},
                {VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME},
                {VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME},
                {VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME},
                {VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME},
                {VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME},
                {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_API_VERSION_1_3},
                {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_API_VERSION_1_2},
            };
            const auto available = deviceExtensions(d->vk);
            const auto config = configuration();
            d->transport = config.mode;
            d->transportName = config.automatic ? "copy" : config.name;
            if (headsetOutput() && hasExtension(available, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
                VkPhysicalDeviceProperties2 extra{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &renderNode};
                auto query = d->vk.physicalFn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
                if (!query)
                    query = d->vk.physicalFn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2KHR");
                if (query)
                    query(physical, &extra);
            }
            if (ci->enabledExtensionCount)
                extensions.assign(ci->ppEnabledExtensionNames,
                                  ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
            bool missing = false;
            for (const auto &extension : required) {
                if (!hasExtension(available, extension.name)) {
                    if (!extension.promoted || api < extension.promoted) {
                        std::fprintf(stderr, "wmr-v3: preflight device: missing required extension %s\n", extension.name);
                        missing = true;
                    }
                } else if (std::none_of(extensions.begin(), extensions.end(), [&](const char *e) {
                        return !std::strcmp(e, extension.name);
                    }))
                    extensions.push_back(extension.name);
            }
            if (missing)
                return VK_ERROR_EXTENSION_NOT_PRESENT;
            calibration = copyClockExtension(d->vk, available);
            if (calibration && std::none_of(extensions.begin(), extensions.end(),
                                            [&](auto e) { return !std::strcmp(e, calibration); }))
                extensions.push_back(calibration);
            request.enabledExtensionCount = extensions.size();
            request.ppEnabledExtensionNames = extensions.data();

            const auto query = d->vk.physicalFn<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                "vkGetPhysicalDeviceQueueFamilyProperties");
            if (!query)
                throw std::runtime_error("Vulkan is missing vkGetPhysicalDeviceQueueFamilyProperties");
            uint32_t count = 0;
            query(physical, &count, nullptr);
            if (!count || !ci->queueCreateInfoCount || !ci->pQueueCreateInfos)
                throw std::runtime_error("No queue families or no queues requested for the render device");
            std::vector<VkQueueFamilyProperties> families(count);
            query(physical, &count, families.data());
            families.resize(count);
            queues.assign(ci->pQueueCreateInfos, ci->pQueueCreateInfos + ci->queueCreateInfoCount);
            for (const auto &queue : queues)
                if (queue.queueFamilyIndex >= count || !queue.queueCount ||
                    queue.queueCount > families[queue.queueFamilyIndex].queueCount) {
                    std::fprintf(stderr, "wmr-v3: preflight device: invalid queue request family=%u count=%u\n",
                                 queue.queueFamilyIndex, queue.queueCount);
                    logQueueFamilies(families, queues);
                    return VK_ERROR_INITIALIZATION_FAILED;
                }
            auto graphics = std::find_if(queues.begin(), queues.end(), [&](const auto &q) {
                return !q.flags && (families[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT);
            });
            if (graphics == queues.end()) {
                logQueueFamilies(families, queues);
                throw std::runtime_error("The compositor did not request an unprotected graphics queue");
            }
            d->renderFamily = graphics->queueFamilyIndex;
            d->renderQueues.resize(graphics->queueCount);
            for (const auto &q : queues) {
                constexpr auto executable = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
                if (!q.flags && (families[q.queueFamilyIndex].queueFlags & executable))
                    d->appFamilies.push_back(q.queueFamilyIndex);
            }
            const auto reserve = [&](VkQueueFlags required, VkQueueFlags excluded,
                                     std::vector<float> &priorities, uint32_t &familyOut,
                                     bool preferSecond = false, bool allowRenderFamily = false) {
                for (uint32_t index = 0; index < count; ++index) {
                    const auto &family = families[index];
                    if (!(family.queueFlags & required) || (family.queueFlags & excluded) ||
                        (!allowRenderFamily && index == d->renderFamily))
                        continue;
                    auto q = queues.end();
                    uint64_t occupied = 0;
                    for (auto entry = queues.begin(); entry != queues.end(); ++entry) {
                        if (entry->queueFamilyIndex != index)
                            continue;
                        occupied += entry->queueCount;
                        if (!entry->flags)
                            q = entry;
                    }
                    if (occupied >= family.queueCount || (occupied && q == queues.end()))
                        continue;
                    const uint32_t existing = q == queues.end() ? 0 : q->queueCount;
                    const uint32_t queueIndex = preferSecond && !existing && family.queueCount - occupied > 1
                        ? 1 : existing;
                    if (q == queues.end()) {
                        VkDeviceQueueCreateInfo extra{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                        extra.queueFamilyIndex = index;
                        queues.push_back(extra);
                        q = std::prev(queues.end());
                    }
                    if (!existing || q->pQueuePriorities) {
                        if (existing)
                            priorities.assign(q->pQueuePriorities, q->pQueuePriorities + existing);
                        priorities.resize(queueIndex + 1, 1.0f);
                        q->pQueuePriorities = priorities.data();
                    }
                    q->queueCount = queueIndex + 1;
                    familyOut = index;
                    return queueIndex;
                }
                return UINT32_MAX;
            };
            d->copyFamily = d->renderFamily;
            if (headsetOutput()) {
                d->acquireFamily = d->renderFamily;
                // Vulkan compute queues can copy alongside later rendering.
                copyIndex = reserve(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT,
                                    copyPriorities, d->copyFamily);
                privateCopy = copyIndex != UINT32_MAX;
                d->sharedCopy = !privateCopy;
                std::fprintf(stderr, "wmr-v3: headset copy queue: mode=%s family=%u index=%u flags=%#x\n",
                             privateCopy ? "compute" : "graphics",
                             d->copyFamily, privateCopy ? copyIndex : 0u,
                             unsigned(families[d->copyFamily].queueFlags));
            } else {
                acquireIndex = reserve(VK_QUEUE_COMPUTE_BIT, 0, acquirePriorities, d->acquireFamily);
                if (acquireIndex == UINT32_MAX)
                    acquireIndex = reserve(VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, 0,
                                           acquirePriorities, d->acquireFamily, false, true);
                if (acquireIndex == UINT32_MAX) {
                    logQueueFamilies(families, queues);
                    throw std::runtime_error("No spare Vulkan queue for independent buffer acquisition");
                }
                if (config.mode == Transport::ace) {
                    privateCopy = true;
                    copyIndex = reserve(VK_QUEUE_COMPUTE_BIT, VK_QUEUE_GRAPHICS_BIT,
                                        copyPriorities, d->copyFamily);
                    if (copyIndex == UINT32_MAX) {
                        logQueueFamilies(families, queues);
                        throw std::runtime_error("ACE mode needs a spare compute-only queue on the render GPU");
                    }
                    std::fprintf(stderr, "wmr-v3: compute-only copy queue family %u index %u\n",
                                 d->copyFamily, copyIndex);
                }
                std::fprintf(stderr, "wmr-v3: preflight transport: requested=%s resolved=%s\n",
                             config.name, d->transportName);
            }
            request.queueCreateInfoCount = queues.size();
            request.pQueueCreateInfos = queues.data();

            VkPhysicalDeviceFeatures2 support{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            support.pNext = &timeline;
            timeline.pNext = &sync2;
            auto features = d->vk.physicalFn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
            if (!features)
                features = d->vk.physicalFn<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2KHR");
            if (!features)
                throw std::runtime_error("Vulkan is missing the core and KHR feature-query entry points");
            features(physical, &support);
            if (!sync2.synchronization2 || !timeline.timelineSemaphore) {
                std::fprintf(stderr, "wmr-v3: preflight device: required Vulkan features synchronization2=%u timelineSemaphore=%u\n",
                             sync2.synchronization2, timeline.timelineSemaphore);
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            auto fenceSupport = d->vk.physicalFn<PFN_vkGetPhysicalDeviceExternalFenceProperties>(
                "vkGetPhysicalDeviceExternalFenceProperties");
            if (!fenceSupport)
                fenceSupport = d->vk.physicalFn<PFN_vkGetPhysicalDeviceExternalFenceProperties>(
                    "vkGetPhysicalDeviceExternalFencePropertiesKHR");
            if (!fenceSupport)
                throw std::runtime_error("Vulkan is missing the core and KHR external-fence capability queries");
            const VkPhysicalDeviceExternalFenceInfo fenceInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
                nullptr, headsetOutput() ? VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT :
                                           VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT};
            VkExternalFenceProperties fenceProps{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
            fenceSupport(physical, &fenceInfo, &fenceProps);
            const auto requiredFence = headsetOutput() ? VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT :
                                                         VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT;
            if (!(fenceProps.externalFenceFeatures & requiredFence) ||
                !(fenceProps.compatibleHandleTypes & fenceInfo.handleType)) {
                std::fprintf(stderr, "wmr-v3: preflight device: fence type %#x lacks capability %#x (features=%#x handles=%#x)\n",
                             unsigned(fenceInfo.handleType), unsigned(requiredFence),
                             fenceProps.externalFenceFeatures, fenceProps.compatibleHandleTypes);
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            bool hasTimeline = false, hasSync2 = false;
            for (auto *p = static_cast<const VkBaseInStructure *>(ci->pNext); p; p = p->pNext) {
                if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES ||
                    p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
                    hasTimeline = p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
                        ? reinterpret_cast<const VkPhysicalDeviceVulkan12Features *>(p)->timelineSemaphore
                        : reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures *>(p)->timelineSemaphore;
                    if (!hasTimeline)
                        throw std::runtime_error("The compositor explicitly disabled timeline semaphores");
                }
                if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES ||
                    p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) {
                    hasSync2 = p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
                        ? reinterpret_cast<const VkPhysicalDeviceVulkan13Features *>(p)->synchronization2
                        : reinterpret_cast<const VkPhysicalDeviceSynchronization2Features *>(p)->synchronization2;
                    if (!hasSync2)
                        throw std::runtime_error("The compositor explicitly disabled synchronization2");
                }
            }
            if (!hasSync2) {
                sync2.pNext = const_cast<void *>(request.pNext);
                request.pNext = &sync2;
            }
            if (!hasTimeline) {
                timeline.pNext = const_cast<void *>(request.pNext);
                request.pNext = &timeline;
            }
        }
        const auto result = create(physical, &request, allocator, out);
        if (result != VK_SUCCESS) {
            if (enabled())
                std::fprintf(stderr, "wmr-v3: preflight device: downstream vkCreateDevice returned %d\n", result);
            return result;
        }
        d->vk.device = *out;
        if (calibration)
            d->vk.calibrate = d->vk.deviceFn<PFN_vkGetCalibratedTimestampsKHR>(
                !std::strcmp(calibration, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)
                    ? "vkGetCalibratedTimestampsKHR" : "vkGetCalibratedTimestampsEXT");
        try {
            if (d->bridge) {
                const auto getQueue = DEVICE(GetDeviceQueue);
                if (!getQueue)
                    throw std::runtime_error("Vulkan device is missing vkGetDeviceQueue");
                for (uint32_t index = 0; index < d->renderQueues.size(); ++index)
                    getQueue(*out, d->renderFamily, index, &d->renderQueues[index]);
                if (std::any_of(d->renderQueues.begin(), d->renderQueues.end(),
                                [](VkQueue queue) { return !queue; }))
                    throw std::runtime_error("Vulkan did not return every compositor graphics queue");
                if (!headsetOutput()) {
                    getQueue(*out, d->acquireFamily, acquireIndex, &d->acquireQueue);
                    if (!d->acquireQueue || loader(*out, d->acquireQueue) != VK_SUCCESS)
                        throw std::runtime_error("Cannot initialize the desktop acquisition queue");
                }
                if (privateCopy) {
                    getQueue(*out, d->copyFamily, copyIndex, &d->copyQueue);
                    const auto copyLoader = d->copyQueue ? loader(*out, d->copyQueue) : VK_ERROR_INITIALIZATION_FAILED;
                    if (copyLoader != VK_SUCCESS) {
                        std::fprintf(stderr, "wmr-v3: preflight device: private copy queue family=%u index=%u loader result=%d\n",
                                     d->copyFamily, copyIndex, copyLoader);
                        throw std::runtime_error("Cannot initialize the private copy queue");
                    }
                }
                if (d->sharedCopy)
                    d->copyQueue = d->renderQueues.front();
                if (!headsetOutput()) {
                    try {
                        d->desktopVblank = std::make_unique<DesktopVblank>(desktopRenderNode());
                    } catch (const std::exception &error) {
                        if (virtualRefreshClock())
                            throw;
                        std::fprintf(stderr, "wmr-v3: preflight desktop timing: no DRM vblank or first-pixel fences; swapchain counter uses a synthetic 90 Hz clock: %s\n", error.what());
                    }
                }
            }
            d->barrier = DEVICE(CmdPipelineBarrier);
            d->barrier2 = DEVICE(CmdPipelineBarrier2);
            if (!d->barrier2)
                d->barrier2 = DEVICE(CmdPipelineBarrier2KHR);
            if (d->bridge) {
                if (!d->barrier || !d->barrier2)
                    throw std::runtime_error("Vulkan device is missing required image-barrier dispatch");
                if (!headsetOutput() && (!DEVICE(CreateFence) || !DEVICE(DestroyFence) || !DEVICE(GetFenceFdKHR)))
                    throw std::runtime_error("Vulkan device is missing desktop fixture fence dispatch");
                if (headsetOutput())
                    std::fprintf(stderr, "wmr-v3: preflight device ready: host acquire, copy family=%u queue=%u (%s)\n",
                                 d->copyFamily, d->sharedCopy ? 0 : copyIndex,
                                 d->sharedCopy ? "compositor" : "independent Vulkan queue");
            }
            if (renderNode.hasRender && renderNode.renderMajor >= 0 && renderNode.renderMinor >= 0)
                d->profileLease = vrProfile::acquire(renderNode.renderMajor, renderNode.renderMinor);
            const std::unique_lock lock(objects);
            devices.emplace(dispatchKey(*out), d);
        } catch (...) {
            if (const auto destroy = DEVICE(DestroyDevice))
                destroy(*out, allocator);
            throw;
        }
        return VK_SUCCESS;
    } catch (const std::exception &error) {
        return failure("CreateDevice", error);
    }
}

ENTRY void VKAPI_CALL destroyDevice(VkDevice handle, const VkAllocationCallbacks *allocator) {
    if (!handle)
        return;
    auto d = deviceFor(handle);
    {
        const std::unique_lock lock(objects);
        devices.erase(dispatchKey(handle));
    }
    FrameRing::drainDeferred(handle);
    DEVICE(DestroyDevice)(handle, allocator);
}

ENTRY VkResult VKAPI_CALL createSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR *ci,
                                          const VkAllocationCallbacks *allocator, VkSwapchainKHR *out) {
    const auto d = deviceFor(device);
    if (!virtualSurface(ci->surface))
        return DEVICE(CreateSwapchainKHR)(device, ci, allocator, out);
    if (!d->bridge) {
        std::fputs("wmr-v3: preflight swapchain: the Vulkan device does not match the selected discrete renderer\n", stderr);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::unique_lock headsetLock(headsetSwapchains, std::defer_lock);
    std::shared_ptr<FrameRing> retiredHeadset;
    const auto reject = [&](VkResult result) {
        if (retiredHeadset)
            retiredHeadset->retireHeadset(false);
        return result;
    };
    try {
        if (headsetOutput()) {
            headsetLock.lock();
            if (ci->oldSwapchain) {
                const auto old = find(rings, ci->oldSwapchain);
                const auto active = std::find_if(activeHeadsetRings.begin(), activeHeadsetRings.end(),
                                                 [&](const auto &entry) {
                    const auto &output = entry.second;
                    return old && output.device == device && output.surface == ci->surface &&
                           output.ring.lock() == old;
                });
                if (active == activeHeadsetRings.end())
                    return VK_ERROR_INITIALIZATION_FAILED;
                retiredHeadset = old;
                activeHeadsetRings.erase(active);
                old->retireHeadset(true);
            }
        }
        if (allocator || ci->imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            std::fprintf(stderr, "wmr-v3: preflight swapchain: color space=%u custom allocator=%s; requires sRGB nonlinear and the default allocator\n",
                         unsigned(ci->imageColorSpace), allocator ? "yes" : "no");
            return reject(VK_ERROR_FEATURE_NOT_PRESENT);
        }
        for (auto *p = static_cast<const VkBaseInStructure *>(ci->pNext); p; p = p->pNext) {
            if (p->sType != VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT) {
                std::fprintf(stderr, "wmr-v3: preflight swapchain: unsupported Vulkan extension structure sType=%u\n",
                             unsigned(p->sType));
                return reject(VK_ERROR_FEATURE_NOT_PRESENT);
            }
            const auto counters = reinterpret_cast<const VkSwapchainCounterCreateInfoEXT *>(p)->surfaceCounters;
            if (counters & ~VK_SURFACE_COUNTER_VBLANK_BIT_EXT)
                return reject(VK_ERROR_FEATURE_NOT_PRESENT);
        }
        const auto lease = copyHeadsetLease(d->vk.physical, ci->surface);
        if (headsetOutput() && lease.fd < 0) {
            std::fputs("wmr-v3: preflight swapchain: no usable DRM lease for the requested headset surface\n", stderr);
            return reject(VK_ERROR_SURFACE_LOST_KHR);
        }
        const auto key = std::make_pair(lease.device, lease.connector);
        if (lease.fd >= 0 && activeHeadsetRings.count(key)) {
            std::fputs("wmr-v3: preflight swapchain: this leased output already has an active swapchain\n", stderr);
            return reject(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR);
        }
        const auto ring = std::make_shared<FrameRing>(d->vk, *ci, d->transport, d->acquireQueue,
                                                      d->acquireFamily, d->renderFamily, d->loader,
                                                      d->copyQueue, d->copyFamily, lease,
                                                      d->privateQueueMutex, d->appFamilies);
        const auto handle = reinterpret_cast<VkSwapchainKHR>(ring.get());
        const auto display = lease.fd >= 0 ? headsetDisplayForSurface(ci->surface) : VK_NULL_HANDLE;
        {
            const std::unique_lock lock(objects);
            rings.emplace(handle, ring);
            try {
                for (auto image : ring->images())
                    ringImages.emplace(image, ring);
                if (lease.fd >= 0)
                    activeHeadsetRings.emplace(key, ActiveOutput{device, ci->surface, display, ring});
            } catch (...) {
                for (auto image : ring->images())
                    ringImages.erase(image);
                rings.erase(handle);
                throw;
            }
        }
        *out = handle;
        std::fprintf(stderr,
                     "wmr-v3: preflight swapchain ready: transport=%s extent=%ux%u vk_format=%u fourcc-bpp=32 modifier=%#llx shared_slots=%zu\n",
                     d->transportName,
                     ci->imageExtent.width, ci->imageExtent.height, unsigned(ci->imageFormat),
                     static_cast<unsigned long long>(ring->modifier()), ring->images().size());
        return VK_SUCCESS;
    } catch (const std::exception &error) {
        if (retiredHeadset)
            retiredHeadset->retireHeadset(false);
        return failure("preflight swapchain", error);
    }
}

ENTRY void VKAPI_CALL destroySwapchain(VkDevice device, VkSwapchainKHR handle,
                                       const VkAllocationCallbacks *allocator) {
    if (!handle)
        return;
    std::unique_lock headsetLock(headsetSwapchains, std::defer_lock);
    if (headsetOutput())
        headsetLock.lock();
    std::shared_ptr<FrameRing> ring;
    {
        const std::unique_lock lock(objects);
        auto entry = rings.find(handle);
        if (entry != rings.end()) {
            ring = std::move(entry->second);
            for (auto image : ring->images())
                ringImages.erase(image);
            rings.erase(entry);
        }
    }
    if (ring && headsetLock.owns_lock()) {
        const auto output = std::find_if(activeHeadsetRings.begin(), activeHeadsetRings.end(),
                                         [&](const auto &entry) { return entry.second.ring.lock() == ring; });
        if (output != activeHeadsetRings.end()) {
            ring->retireHeadset(false);
            activeHeadsetRings.erase(output);
        }
    }
    if (!ring) {
        const auto d = deviceFor(device);
        DEVICE(DestroySwapchainKHR)(device, handle, allocator);
    }
}

ENTRY VkResult VKAPI_CALL getImages(VkDevice device, VkSwapchainKHR handle, uint32_t *count,
                                    VkImage *images) {
    if (const auto ring = find(rings, handle)) {
        const auto &source = ring->images();
        if (!images) {
            *count = source.size();
            return VK_SUCCESS;
        }
        *count = std::min(*count, static_cast<uint32_t>(source.size()));
        std::copy_n(source.begin(), *count, images);
        return *count < source.size() ? VK_INCOMPLETE : VK_SUCCESS;
    }
    const auto d = deviceFor(device);
    return DEVICE(GetSwapchainImagesKHR)(device, handle, count, images);
}

VkResult acquireFromRing(FrameRing &ring, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index) {
    try {
        return ring.acquire(timeout, sem, fence, index);
    } catch (const std::exception &error) {
        return failure("AcquireNextImage", error);
    }
}

ENTRY VkResult VKAPI_CALL acquire(VkDevice device, VkSwapchainKHR handle, uint64_t timeout,
                                  VkSemaphore sem, VkFence fence, uint32_t *index) {
    if (const auto ring = find(rings, handle))
        return acquireFromRing(*ring, timeout, sem, fence, index);
    const auto d = deviceFor(device);
    return DEVICE(AcquireNextImageKHR)(device, handle, timeout, sem, fence, index);
}

ENTRY VkResult VKAPI_CALL acquire2(VkDevice device, const VkAcquireNextImageInfoKHR *info,
                                   uint32_t *index) {
    if (const auto ring = find(rings, info->swapchain)) {
        if (info->deviceMask != 1 || info->pNext) {
            std::fprintf(stderr, "wmr-v3: AcquireNextImage2: unsupported device mask %#x or extension chain\n",
                         info->deviceMask);
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        return acquireFromRing(*ring, info->timeout, info->semaphore, info->fence, index);
    }
    const auto d = deviceFor(device);
    return DEVICE(AcquireNextImage2KHR)(device, info, index);
}

std::unique_lock<std::mutex> lockSharedCopyQueue(const Device &device, VkQueue queue) {
    if (device.sharedCopy && device.copyQueue == queue)
        return std::unique_lock(*device.privateQueueMutex);
    return {};
}

ENTRY VkResult VKAPI_CALL queueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo *info, VkFence fence) {
    const auto d = deviceFor(queue);
    const auto lock = lockSharedCopyQueue(*d, queue);
    return DEVICE(QueueSubmit)(queue, count, info, fence);
}

template<bool khr>
VkResult VKAPI_CALL queueSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2 *info, VkFence fence) {
    const auto d = deviceFor(queue);
    const auto lock = lockSharedCopyQueue(*d, queue);
    if constexpr (khr)
        return DEVICE(QueueSubmit2KHR)(queue, count, info, fence);
    return DEVICE(QueueSubmit2)(queue, count, info, fence);
}

ENTRY VkResult VKAPI_CALL queueBindSparse(VkQueue queue, uint32_t count, const VkBindSparseInfo *info, VkFence fence) {
    const auto d = deviceFor(queue);
    const auto lock = lockSharedCopyQueue(*d, queue);
    return DEVICE(QueueBindSparse)(queue, count, info, fence);
}

ENTRY VkResult VKAPI_CALL queueWaitIdle(VkQueue queue) {
    const auto d = deviceFor(queue);
    const auto lock = lockSharedCopyQueue(*d, queue);
    return DEVICE(QueueWaitIdle)(queue);
}

ENTRY VkResult VKAPI_CALL deviceWaitIdle(VkDevice device) {
    const auto d = deviceFor(device);
    if (d->privateQueueMutex) {
        const std::lock_guard lock(*d->privateQueueMutex);
        return DEVICE(DeviceWaitIdle)(device);
    }
    return DEVICE(DeviceWaitIdle)(device);
}

ENTRY VkResult VKAPI_CALL present(VkQueue queue, const VkPresentInfoKHR *info) {
    const auto d = deviceFor(queue);
    for (uint32_t i = 0; i < info->swapchainCount; ++i) {
        if (const auto ring = find(rings, info->pSwapchains[i])) {
            if (info->swapchainCount != 1) {
                std::fprintf(stderr, "wmr-v3: QueuePresent: bridge swapchain cannot share a present with %u swapchains\n",
                             info->swapchainCount);
                if (info->pResults)
                    std::fill_n(info->pResults, info->swapchainCount, VK_ERROR_FEATURE_NOT_PRESENT);
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
            VkResult result = VK_ERROR_FEATURE_NOT_PRESENT;
            try {
                // The async compositor can present from its compute queue.
                // Private copies wait on that producer and use their own command pool.
                if (d->copyQueue || std::find(d->renderQueues.begin(), d->renderQueues.end(), queue) !=
                                      d->renderQueues.end())
                    result = ring->present(queue, *info);
                else
                    std::fprintf(stderr, "wmr-v3: QueuePresent: requires a graphics queue from family %u for this transport\n",
                                 d->renderFamily);
            } catch (const std::exception &error) {
                result = failure("QueuePresent", error);
            }
            if (info->pResults)
                info->pResults[i] = result;
            return result;
        }
    }
    const auto lock = lockSharedCopyQueue(*d, queue);
    return DEVICE(QueuePresentKHR)(queue, info);
}

void logDisplayEventFailure(Device &device, const char *stage, VkResult result) {
    if (!device.displayEventFault.test_and_set(std::memory_order_relaxed))
        std::fprintf(stderr, "wmr-v3: RegisterDisplayEvent: %s (VkResult %d); first-pixel timing fence unavailable\n",
                     stage, result);
}

VkResult registerHeadsetVblank(VkDevice device, Device &state, VkDisplayKHR display,
                               const VkAllocationCallbacks *allocator, VkFence *out) {
    std::shared_ptr<FrameRing> ring;
    {
        const std::lock_guard lock(headsetSwapchains);
        const auto output = std::find_if(activeHeadsetRings.begin(), activeHeadsetRings.end(),
                                         [&](const auto &entry) {
            return entry.second.device == device && entry.second.display == display;
        });
        if (output != activeHeadsetRings.end())
            ring = output->second.ring.lock();
    }
    if (!ring)
        return VK_ERROR_FEATURE_NOT_PRESENT;

    VkFence fence{};
    const auto create = state.vk.deviceFn<PFN_vkCreateFence>("vkCreateFence");
    const auto destroy = state.vk.deviceFn<PFN_vkDestroyFence>("vkDestroyFence");
    try {
        auto event = std::make_shared<DisplayEvent>(state.displayEventNotifier);
        const VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        auto result = create(device, &info, allocator, &fence);
        if (result != VK_SUCCESS)
            return result;
        result = ring->queueVblank(event);
        if (result != VK_SUCCESS) {
            destroy(device, fence, allocator);
            return result;
        }
        const std::lock_guard lock(state.displayFenceMutex);
        state.displayFences.emplace(fence, std::move(event));
        state.hasDisplayFences.store(true, std::memory_order_release);
        *out = fence;
        return VK_SUCCESS;
    } catch (const std::exception &error) {
        if (fence)
            destroy(device, fence, allocator);
        return failure("RegisterDisplayEvent", error);
    }
}

std::shared_ptr<DisplayEvent> displayFence(Device &device, VkFence fence) {
    if (!device.hasDisplayFences.load(std::memory_order_acquire))
        return {};
    const std::lock_guard lock(device.displayFenceMutex);
    const auto entry = device.displayFences.find(fence);
    return entry == device.displayFences.end() ? nullptr : entry->second;
}

void forgetDisplayFences(Device &device, uint32_t count, const VkFence *fences) {
    if (!device.hasDisplayFences.load(std::memory_order_acquire))
        return;
    const std::lock_guard lock(device.displayFenceMutex);
    for (uint32_t i = 0; i < count; ++i)
        device.displayFences.erase(fences[i]);
    device.hasDisplayFences.store(!device.displayFences.empty(), std::memory_order_release);
}

ENTRY VkResult VKAPI_CALL fenceStatus(VkDevice device, VkFence fence) {
    const auto d = deviceFor(device);
    if (const auto event = displayFence(*d, fence))
        return event->status();
    return DEVICE(GetFenceStatus)(device, fence);
}

ENTRY VkResult VKAPI_CALL waitForFences(VkDevice device, uint32_t count, const VkFence *fences,
                                       VkBool32 all, uint64_t timeout) {
    const auto d = deviceFor(device);
    const auto wait = DEVICE(WaitForFences);
    if (!d->hasDisplayFences.load(std::memory_order_acquire))
        return wait(device, count, fences, all, timeout);
    if (count == 1) {
        if (const auto event = displayFence(*d, *fences))
            return event->waitNs(timeout);
        return wait(device, count, fences, all, timeout);
    }
    try {
        std::vector<VkFence> native;
        std::vector<std::shared_ptr<DisplayEvent>> events;
        native.reserve(count);
        events.reserve(count);
        {
            const std::lock_guard lock(d->displayFenceMutex);
            for (uint32_t i = 0; i < count; ++i) {
                const auto entry = d->displayFences.find(fences[i]);
                if (entry == d->displayFences.end())
                    native.push_back(fences[i]);
                else
                    events.push_back(entry->second);
            }
        }
        if (events.empty())
            return wait(device, count, fences, all, timeout);
        const auto start = std::chrono::steady_clock::now();
        const auto remaining = [&] {
            if (timeout == UINT64_MAX)
                return timeout;
            const auto elapsed = std::chrono::steady_clock::now() - start;
            const auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
            return timeout - std::min(timeout, ns);
        };
        for (;;) {
            const auto generation = d->displayEventNotifier->generation();
            bool pending = false;
            for (const auto &event : events) {
                const auto result = event->status();
                if (result < 0 || (result == VK_SUCCESS && !all))
                    return result;
                pending |= result == VK_NOT_READY;
            }
            if (all && !pending)
                return native.empty() ? VK_SUCCESS : wait(device, native.size(), native.data(), VK_TRUE, remaining());

            const auto time = remaining();
            if (!native.empty()) {
                const auto result = wait(device, native.size(), native.data(), all, std::min<uint64_t>(time, 250000));
                if (result < 0 || (result == VK_SUCCESS && !all))
                    return result;
                if (result == VK_SUCCESS)
                    native.clear();
            } else if (pending && time)
                d->displayEventNotifier->waitAfter(generation, time);
            if (!time)
                return VK_TIMEOUT;
        }
    } catch (const std::exception &error) {
        return failure("WaitForFences", error);
    }
}

ENTRY VkResult VKAPI_CALL resetFences(VkDevice device, uint32_t count, const VkFence *fences) {
    const auto d = deviceFor(device);
    const auto result = DEVICE(ResetFences)(device, count, fences);
    if (result == VK_SUCCESS)
        forgetDisplayFences(*d, count, fences);
    return result;
}

ENTRY void VKAPI_CALL destroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    const auto d = deviceFor(device);
    forgetDisplayFences(*d, 1, &fence);
    DEVICE(DestroyFence)(device, fence, allocator);
}

ENTRY VkResult VKAPI_CALL displayEvent(VkDevice device, VkDisplayKHR display,
                                       const VkDisplayEventInfoEXT *info,
                                       const VkAllocationCallbacks *allocator, VkFence *fence) {
    const auto d = deviceFor(device);
    if (!virtualDisplay(display))
        return DEVICE(RegisterDisplayEventEXT)(device, display, info, allocator, fence);
    if (headsetOutput())
        return info->displayEvent == VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT
            ? registerHeadsetVblank(device, *d, display, allocator, fence) : VK_ERROR_FEATURE_NOT_PRESENT;
    if (!d->desktopVblank || info->displayEvent != VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT) {
        logDisplayEventFailure(*d, info->displayEvent != VK_DISPLAY_EVENT_TYPE_FIRST_PIXEL_OUT_EXT
                               ? "unsupported display event" : "no active DRM timing source",
                               VK_ERROR_FEATURE_NOT_PRESENT);
        return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    VkExportFenceCreateInfo share{VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO};
    share.handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    create.pNext = &share;
    VkFence event{};
    auto result = DEVICE(CreateFence)(device, &create, allocator, &event);
    if (result != VK_SUCCESS) {
        logDisplayEventFailure(*d, "could not allocate an exportable Vulkan fence", result);
        return result;
    }
    VkFenceGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR};
    exportInfo.fence = event;
    exportInfo.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    result = DEVICE(GetFenceFdKHR)(device, &exportInfo, &fd);
    if (result == VK_SUCCESS) {
        try {
            result = d->desktopVblank->queue(fd);
        } catch (const std::exception &error) {
            result = failure("RegisterDisplayEvent", error);
        }
        if (fd >= 0)
            close(fd);
        if (result == VK_SUCCESS) {
            *fence = event;
            return VK_SUCCESS;
        }
        logDisplayEventFailure(*d, "DRM could not register the next vblank", result);
    } else
        logDisplayEventFailure(*d, "Vulkan could not export the fence as OPAQUE_FD", result);
    DEVICE(DestroyFence)(device, event, allocator);
    return result;
}

ENTRY VkResult VKAPI_CALL displayPower(VkDevice device, VkDisplayKHR display,
                                       const VkDisplayPowerInfoEXT *info) {
    if (virtualDisplay(display))
        return VK_SUCCESS;
    const auto d = deviceFor(device);
    return DEVICE(DisplayPowerControlEXT)(device, display, info);
}

ENTRY VkResult VKAPI_CALL swapchainCounter(VkDevice device, VkSwapchainKHR ring,
                                           VkSurfaceCounterFlagBitsEXT counter, uint64_t *value) {
    const auto d = deviceFor(device);
    if (const auto owned = find(rings, ring)) {
        if (counter != VK_SURFACE_COUNTER_VBLANK_BIT_EXT)
            return VK_ERROR_FEATURE_NOT_PRESENT;
        if (headsetOutput())
            return owned->displayCounter(value);
        if (d->desktopVblank)
            return d->desktopVblank->counter(value);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        *value = uint64_t(now.tv_sec) * 90 + uint64_t(now.tv_nsec) * 90 / 1000000000;
        return VK_SUCCESS;
    }
    return DEVICE(GetSwapchainCounterEXT)(device, ring, counter, value);
}

ENTRY VkResult VKAPI_CALL waitForPresent(VkDevice device, VkSwapchainKHR handle,
                                         uint64_t id, uint64_t timeout) {
    if (const auto ring = find(rings, handle)) {
        try {
            return ring->waitPresent(id, timeout, false);
        } catch (const std::exception &error) {
            return failure("WaitForPresent", error);
        }
    }
    const auto d = deviceFor(device);
    return DEVICE(WaitForPresentKHR)(device, handle, id, timeout);
}

ENTRY VkResult VKAPI_CALL waitForPresent2(VkDevice device, VkSwapchainKHR handle,
                                          const VkPresentWait2InfoKHR *info) {
    if (const auto ring = find(rings, handle)) {
        try {
            return ring->waitPresent(info->presentId, info->timeout, true);
        } catch (const std::exception &error) {
            return failure("WaitForPresent2", error);
        }
    }
    const auto d = deviceFor(device);
    return DEVICE(WaitForPresent2KHR)(device, handle, info);
}

template<class Barrier> const Barrier *translate(const Barrier *barriers, uint32_t count,
                                                 std::vector<Barrier> &translated) {
    if (!count)
        return barriers;
    const auto presentLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const auto needsTranslation = [presentLayout](const Barrier &barrier) {
        return barrier.oldLayout == presentLayout || barrier.newLayout == presentLayout;
    };
    const auto end = barriers + count;
    auto entry = std::find_if(barriers, end, needsTranslation);
    if (entry == end)
        return barriers;
    const std::shared_lock lock(objects);
    for (; entry != end; ++entry) {
        const auto &b = *entry;
        if (!needsTranslation(b) || ringImages.find(b.image) == ringImages.end())
            continue;
        if (translated.empty())
            translated.assign(barriers, end);
        auto &next = translated[entry - barriers];
        if (b.oldLayout == presentLayout)
            next.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (b.newLayout == presentLayout)
            next.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    return translated.empty() ? barriers : translated.data();
}

ENTRY void VKAPI_CALL barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                               VkDependencyFlags flags, uint32_t mc, const VkMemoryBarrier *mb,
                               uint32_t bc, const VkBufferMemoryBarrier *bb,
                               uint32_t ic, const VkImageMemoryBarrier *ib) {
    auto d = deviceFor(cb);
    thread_local std::vector<VkImageMemoryBarrier> translated;
    translated.clear();
    d->barrier(cb, src, dst, flags, mc, mb, bc, bb, ic, translate(ib, ic, translated));
}

ENTRY void VKAPI_CALL barrier2(VkCommandBuffer cb, const VkDependencyInfo *info) {
    auto d = deviceFor(cb);
    thread_local std::vector<VkImageMemoryBarrier2> translated;
    translated.clear();
    auto request = *info;
    request.pImageMemoryBarriers = translate(info->pImageMemoryBarriers, info->imageMemoryBarrierCount,
                                             translated);
    d->barrier2(cb, &request);
}

PFN_vkVoidFunction layerHook(const char *name) {
#define HOOK(function, local) if (!std::strcmp("vk" #function, name)) \
    return reinterpret_cast<PFN_vkVoidFunction>(local)
    HOOK(CreateInstance, createInstance);
    HOOK(DestroyInstance, destroyInstance);
    HOOK(CreateDevice, createDevice);
    HOOK(DestroyDevice, destroyDevice);
    if (!enabled())
        return nullptr;
    HOOK(EnumeratePhysicalDevices, enumeratePhysicalDevices);
    HOOK(CreateSwapchainKHR, createSwapchain);
    HOOK(DestroySwapchainKHR, destroySwapchain);
    HOOK(GetSwapchainImagesKHR, getImages);
    HOOK(AcquireNextImageKHR, acquire);
    HOOK(AcquireNextImage2KHR, acquire2);
    HOOK(QueueSubmit, queueSubmit);
    HOOK(QueueSubmit2, queueSubmit2<false>);
    HOOK(QueueSubmit2KHR, queueSubmit2<true>);
    HOOK(QueueBindSparse, queueBindSparse);
    HOOK(QueueWaitIdle, queueWaitIdle);
    HOOK(DeviceWaitIdle, deviceWaitIdle);
    HOOK(QueuePresentKHR, present);
    HOOK(RegisterDisplayEventEXT, displayEvent);
    if (headsetOutput()) {
        HOOK(GetFenceStatus, fenceStatus);
        HOOK(WaitForFences, waitForFences);
        HOOK(ResetFences, resetFences);
        HOOK(DestroyFence, destroyFence);
    }
    HOOK(DisplayPowerControlEXT, displayPower);
    HOOK(GetSwapchainCounterEXT, swapchainCounter);
    HOOK(WaitForPresentKHR, waitForPresent);
    HOOK(WaitForPresent2KHR, waitForPresent2);
    HOOK(CmdPipelineBarrier, barrier);
    HOOK(CmdPipelineBarrier2, barrier2);
    HOOK(CmdPipelineBarrier2KHR, barrier2);
#undef HOOK
    if (const auto audit = outputAuditHook(name))
        return audit;
    return virtualDisplayHook(name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGDPA(VkDevice device, const char *name) {
    const auto d = deviceFor(device);
    if (!d || !name)
        return nullptr;
    const auto next = d->vk.deviceProc(device, name);
    if (next)
        if (const auto hook = layerHook(name))
            return hook;
    return next;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerGIPA(VkInstance instance, const char *name) {
    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(layerGIPA);
    if (!std::strcmp(name, "vkGetDeviceProcAddr"))
        return reinterpret_cast<PFN_vkVoidFunction>(layerGDPA);
    if (const auto hook = layerHook(name))
        return hook;
    const auto i = instanceFor(instance);
    return i ? i->next(instance, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layerPhysical(VkInstance instance, const char *name) {
    if (enabled())
        if (const auto hook = virtualDisplayHook(name))
            return hook;
    const auto i = instanceFor(instance);
    return i && i->physicalProc ? i->physicalProc(instance, name) : nullptr;
}

#undef ENTRY
#undef DEVICE

} // namespace

PFN_vkVoidFunction auditNextDevice(const void *object, const char *name) {
    const auto device = deviceFor(const_cast<void *>(object));
    return device ? device->vk.deviceProc(device->vk.device, name) : nullptr;
}

bool auditOwnsImage(VkImage image) {
    const std::shared_lock lock(objects);
    return ringImages.count(image) != 0;
}

void auditBeginWrite(VkCommandBuffer command, VkImage image) {
    if (const auto ring = find(ringImages, image).lock())
        ring->auditWrite(command, image, true);
}

void auditEndWrite(VkCommandBuffer command, VkImage image) {
    if (const auto ring = find(ringImages, image).lock())
        ring->auditWrite(command, image, false);
}

VkInstance instanceForPhysicalDevice(VkPhysicalDevice physical) {
    const auto i = instanceFor(physical);
    return i ? i->handle : VK_NULL_HANDLE;
}

bool bridgeRenderDevice(VkPhysicalDevice physical) {
    const auto i = instanceFor(physical);
    return i && matchesRenderDevice({physical, {}, i->next, i->handle, {}});
}

void releaseHeadsetOutput(VkDisplayKHR display) {
    const std::lock_guard lock(headsetSwapchains);
    const auto output = std::find_if(activeHeadsetRings.begin(), activeHeadsetRings.end(),
                                     [&](const auto &entry) { return entry.second.display == display; });
    if (output == activeHeadsetRings.end())
        return;
    if (const auto ring = output->second.ring.lock())
        ring->retireHeadset(false);
    activeHeadsetRings.erase(output);
}

PFN_vkVoidFunction nextPhysical(VkPhysicalDevice physical, const char *name) {
    const auto instance = instanceFor(physical);
    return instance ? instance->next(instance->handle, name) : nullptr;
}

PFN_vkVoidFunction nextInstance(VkInstance handle, const char *name) {
    const auto instance = instanceFor(handle);
    return instance ? instance->next(handle, name) : nullptr;
}

} // namespace v3

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char *name) {
    return v3::layerGIPA(instance, name);
}

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char *name) {
    return v3::layerGDPA(device, name);
}

extern "C" __attribute__((visibility("default"))) VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *request) {
    if (!request || request->loaderLayerInterfaceVersion == 0)
        return VK_ERROR_INITIALIZATION_FAILED;
    request->loaderLayerInterfaceVersion = std::min(request->loaderLayerInterfaceVersion, 2u);
    request->pfnGetInstanceProcAddr = v3::layerGIPA;
    request->pfnGetDeviceProcAddr = v3::layerGDPA;
    request->pfnGetPhysicalDeviceProcAddr = v3::layerPhysical;
    return VK_SUCCESS;
}
