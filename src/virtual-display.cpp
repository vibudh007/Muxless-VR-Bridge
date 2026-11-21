#ifdef MUXLESS_DESKTOP_FIXTURE
#define VK_USE_PLATFORM_XLIB_XRANDR_EXT
#endif
#include "virtual-display.hpp"
#include "headset.hpp"
#include "transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <vector>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace v3 {

PFN_vkVoidFunction nextPhysical(VkPhysicalDevice, const char *);
PFN_vkVoidFunction nextInstance(VkInstance, const char *);
VkInstance instanceForPhysicalDevice(VkPhysicalDevice);
bool bridgeRenderDevice(VkPhysicalDevice);
void releaseHeadsetOutput(VkDisplayKHR);

namespace {

// The desktop fixture has one fixed 90 Hz output. The headset uses its DRM mode.
char displayToken, modeToken, surfaceToken;
const auto display = reinterpret_cast<VkDisplayKHR>(&displayToken);
const auto mode = reinterpret_cast<VkDisplayModeKHR>(&modeToken);
const auto surface = reinterpret_cast<VkSurfaceKHR>(&surfaceToken);
constexpr VkExtent2D extent{2880, 1440};
constexpr uint32_t refreshMillihertz = 90000;
constexpr uint64_t refreshHz = refreshMillihertz / 1000;
constexpr uint64_t nsPerSecond = 1'000'000'000;
using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

uint64_t tickAt(Clock::time_point now) {
    const uint64_t ns = std::chrono::duration_cast<Nanoseconds>(now.time_since_epoch()).count();
    return ns / nsPerSecond * refreshHz + ns % nsPerSecond * refreshHz / nsPerSecond;
}

Clock::time_point startOfTick(uint64_t tick) {
    const auto seconds = tick / refreshHz * nsPerSecond;
    const auto fraction = (tick % refreshHz * nsPerSecond + refreshHz - 1) / refreshHz;
    return Clock::time_point(Nanoseconds(seconds + fraction));
}

constexpr VkSurfaceCapabilitiesKHR surfaceCaps{
    .minImageCount = 2,
    .maxImageCount = 3,
    .currentExtent = extent,
    .minImageExtent = extent,
    .maxImageExtent = extent,
    .maxImageArrayLayers = 1,
    .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    .currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    .supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    .supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,
};

constexpr std::array formats{
    VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};

const VkDisplayPropertiesKHR displayProps{
    .display = display,
    .displayName = "WMR bridge virtual display",
    .physicalDimensions = {120, 60},
    .physicalResolution = extent,
    .supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
    .planeReorderPossible = VK_FALSE,
    .persistentContent = VK_TRUE,
};
const VkDisplayModePropertiesKHR modeProps{mode, {extent, refreshMillihertz}};
const VkDisplayPlanePropertiesKHR planeProps{display, 0};
constexpr VkDisplayPlaneCapabilitiesKHR planeCaps{
    .supportedAlpha = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
    .minSrcPosition = {0, 0},
    .maxSrcPosition = {0, 0},
    .minSrcExtent = extent,
    .maxSrcExtent = extent,
    .minDstPosition = {0, 0},
    .maxDstPosition = {0, 0},
    .minDstExtent = extent,
    .maxDstExtent = extent,
};

struct HeadsetMode {
    drmModeModeInfo timing;
    VkDisplayModeParametersKHR parameters;
};

struct HeadsetDisplay {
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    dev_t device{};
    uint32_t connector{};
    int lease{-1};
    uint64_t generation{};
    VkExtent2D dimensions{};
    std::vector<HeadsetMode> modes;
    std::vector<uint32_t> scanoutFormats;
    std::vector<VkQueueFamilyProperties> queues;

    ~HeadsetDisplay() {
        if (lease >= 0)
            close(lease);
    }
};

struct HeadsetSurface {
    std::shared_ptr<HeadsetDisplay> display;
    HeadsetMode mode;
    uint64_t generation{};
    VkImageUsageFlags usage{};
    std::vector<VkSurfaceFormatKHR> formats;
};

std::mutex displayMutex;
std::vector<std::shared_ptr<HeadsetDisplay>> displays;
std::vector<std::unique_ptr<HeadsetSurface>> surfaces;

std::shared_ptr<HeadsetDisplay> findDisplay(VkDisplayKHR id) {
    const auto it = std::find_if(displays.begin(), displays.end(), [&](const auto &entry) {
        return reinterpret_cast<VkDisplayKHR>(entry.get()) == id;
    });
    return it == displays.end() ? nullptr : *it;
}

const HeadsetMode *findMode(VkDisplayModeKHR id, std::shared_ptr<HeadsetDisplay> &owner) {
    for (const auto &entry : displays) {
        const auto it = std::find_if(entry->modes.begin(), entry->modes.end(), [&](const auto &item) {
            return reinterpret_cast<VkDisplayModeKHR>(uintptr_t(&item)) == id;
        });
        if (it != entry->modes.end()) {
            owner = entry;
            return &*it;
        }
    }
    return nullptr;
}

auto findSurface(VkSurfaceKHR id) {
    return std::find_if(surfaces.begin(), surfaces.end(), [&](const auto &entry) {
        return reinterpret_cast<VkSurfaceKHR>(entry.get()) == id;
    });
}

bool sameMode(const VkDisplayModeParametersKHR &a, const VkDisplayModeParametersKHR &b) {
    return a.refreshRate == b.refreshRate && a.visibleRegion.width == b.visibleRegion.width &&
           a.visibleRegion.height == b.visibleRegion.height;
}

VkDisplayPropertiesKHR headsetProperties(const HeadsetDisplay &entry) {
    auto properties = displayProps;
    properties.display = reinterpret_cast<VkDisplayKHR>(uintptr_t(&entry));
    properties.displayName = "Muxless VR Bridge DRM display";
    properties.physicalDimensions = entry.dimensions;
    properties.physicalResolution = entry.modes.front().parameters.visibleRegion;
    return properties;
}

VkSurfaceCapabilitiesKHR headsetCapabilities(const HeadsetSurface &entry) {
    auto capabilities = surfaceCaps;
    capabilities.currentExtent = capabilities.minImageExtent = capabilities.maxImageExtent =
        entry.mode.parameters.visibleRegion;
    capabilities.supportedUsageFlags = entry.usage;
    return capabilities;
}

template <class Entry, class Write>
VkResult enumerate(size_t available, uint32_t *count, Entry *out, Write write) {
    if (!out) {
        *count = static_cast<uint32_t>(available);
        return VK_SUCCESS;
    }
    *count = std::min(*count, static_cast<uint32_t>(available));
    for (uint32_t i = 0; i < *count; ++i)
        write(out[i], i);
    return *count == available ? VK_SUCCESS : VK_INCOMPLETE;
}

uint32_t outputFourcc(VkFormat format) {
    return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB
         ? DRM_FORMAT_XBGR8888 : DRM_FORMAT_XRGB8888;
}

void negotiateFormats(HeadsetSurface &surface) {
    surface.formats.clear();
    surface.usage = 0;
    const auto gpu = surface.display->gpu;
    auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
        nextPhysical(gpu, "vkGetPhysicalDeviceImageFormatProperties2"));
    if (!query)
        query = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
            nextPhysical(gpu, "vkGetPhysicalDeviceImageFormatProperties2KHR"));
    if (!query)
        return;
    const auto extent = surface.mode.parameters.visibleRegion;
    const auto &scanout = surface.display->scanoutFormats;
    surface.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    for (const auto &candidate : formats) {
        if ((!scanout.empty() && !std::binary_search(scanout.begin(), scanout.end(), outputFourcc(candidate.format))) ||
            !supportsLinearFrameFeed(gpu, query, candidate.format, extent, surface.usage))
            continue;
        surface.formats.push_back(candidate);
    }
    for (const auto flag : {VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_USAGE_STORAGE_BIT}) {
        const auto usage = surface.usage | flag;
        if (std::all_of(surface.formats.begin(), surface.formats.end(), [&](const VkSurfaceFormatKHR &format) {
                return supportsLinearFrameFeed(gpu, query, format.format, extent, usage);
            }))
            surface.usage = usage;
    }
}

template <typename T>
VkResult one(uint32_t *count, T *entries, const T &value) {
    if (!entries) {
        *count = 1;
        return VK_SUCCESS;
    }
    if (*count == 0)
        return VK_INCOMPLETE;
    *count = 1;
    *entries = value;
    return VK_SUCCESS;
}

template <typename T, typename Property>
VkResult one(uint32_t *count, T *entries, Property T::*field, const Property &value) {
    if (!entries)
        return one<Property>(count, nullptr, value);
    if (*count == 0)
        return VK_INCOMPLETE;
    return one(count, &((*entries).*field), value);
}

template <typename Function, typename... Args>
VkResult forwardPhysical(const char *name, VkPhysicalDevice gpu, Args... args) {
    const auto next = reinterpret_cast<Function>(nextPhysical(gpu, name));
    return next ? next(gpu, args...) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

#ifdef MUXLESS_DESKTOP_FIXTURE
VKAPI_ATTR VkResult VKAPI_CALL randrDisplay(VkPhysicalDevice, Display *, RROutput,
                                           VkDisplayKHR *out) {
    *out = display;
    return VK_SUCCESS;
}
#endif

VKAPI_ATTR VkResult VKAPI_CALL getDrmDisplay(VkPhysicalDevice gpu, int fd, uint32_t connector,
                                            VkDisplayKHR *out) {
    const auto forward = [&] {
        return forwardPhysical<PFN_vkGetDrmDisplayEXT>("vkGetDrmDisplayEXT", gpu, fd, connector, out);
    };
    if (!bridgeRenderDevice(gpu))
        return forward();
    struct stat stat{};
    using Connector = std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>;
    const Connector output(drmModeGetConnectorCurrent(fd, connector), drmModeFreeConnector);
    if (!output) {
        std::fprintf(stderr, "wmr-v3: preflight cannot query DRM connector %u (errno %d)\n", connector, errno);
        return forward();
    }
    if (output->connection != DRM_MODE_CONNECTED) {
        std::fprintf(stderr, "wmr-v3: preflight DRM connector %u is not connected (status %u)\n",
                     connector, static_cast<unsigned>(output->connection));
        return forward();
    }
    if (fstat(fd, &stat)) {
        std::fprintf(stderr, "wmr-v3: preflight DRM connector %u fstat failed (errno %d)\n", connector, errno);
        return forward();
    }

    try {
        auto entry = std::make_shared<HeadsetDisplay>();
        entry->instance = instanceForPhysicalDevice(gpu);
        if (!entry->instance)
            return VK_ERROR_INITIALIZATION_FAILED;
        entry->gpu = gpu;
        entry->device = stat.st_rdev;
        entry->connector = connector;
        entry->dimensions = {output->mmWidth, output->mmHeight};
        for (int i = 0; i < output->count_modes; ++i) {
            const auto &timing = output->modes[i];
            const auto rate = headsetRefresh(timing);
            if (rate)
                entry->modes.push_back({timing, {{timing.hdisplay, timing.vdisplay}, rate}});
        }
        if (entry->modes.empty()) {
            std::fprintf(stderr, "wmr-v3: preflight DRM connector %u has no usable progressive mode\n", connector);
            return forward();
        }
        std::stable_partition(entry->modes.begin(), entry->modes.end(), [](const HeadsetMode &item) {
            return item.timing.type & DRM_MODE_TYPE_PREFERRED;
        });
        const auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            nextPhysical(gpu, "vkGetPhysicalDeviceQueueFamilyProperties"));
        if (!queues)
            return VK_ERROR_INITIALIZATION_FAILED;
        uint32_t count{};
        queues(gpu, &count, nullptr);
        entry->queues.resize(count);
        queues(gpu, &count, entry->queues.data());
        entry->queues.resize(count);
        const std::lock_guard lock(displayMutex);
        for (const auto &other : displays) {
            if (other->instance == entry->instance && other->gpu == gpu && other->device == entry->device &&
                other->connector == connector && other->modes.size() == entry->modes.size() &&
                std::equal(other->modes.begin(), other->modes.end(), entry->modes.begin(),
                           [](const HeadsetMode &a, const HeadsetMode &b) {
                               return sameHeadsetTiming(a.timing, b.timing);
                           })) {
                *out = reinterpret_cast<VkDisplayKHR>(other.get());
                return VK_SUCCESS;
            }
        }
        displays.push_back(entry);
        *out = reinterpret_cast<VkDisplayKHR>(entry.get());
        return VK_SUCCESS;
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL acquireDrmDisplay(VkPhysicalDevice gpu, int fd, VkDisplayKHR id) {
    std::unique_lock lock(displayMutex);
    const auto entry = findDisplay(id);
    if (!entry) {
        lock.unlock();
        return forwardPhysical<PFN_vkAcquireDrmDisplayEXT>("vkAcquireDrmDisplayEXT", gpu, fd, id);
    }
    struct stat stat{};
    if (entry->gpu != gpu || entry->lease >= 0) {
        std::fprintf(stderr, "wmr-v3: preflight physical-device conflict or duplicate lease for DRM connector %u\n",
                     entry->connector);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (fstat(fd, &stat)) {
        std::fprintf(stderr, "wmr-v3: preflight lease fstat failed for connector %u (errno %d)\n",
                     entry->connector, errno);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (stat.st_rdev != entry->device) {
        std::fprintf(stderr, "wmr-v3: preflight lease device %u:%u differs from device %u:%u for connector %u\n",
                     major(stat.st_rdev), minor(stat.st_rdev), major(entry->device), minor(entry->device),
                     entry->connector);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    using Lease = std::unique_ptr<drmModeObjectList, decltype(&drmFree)>;
    const Lease granted(drmModeGetLease(fd), drmFree);
    if (!granted || std::find(granted->objects, granted->objects + granted->count,
                              entry->connector) == granted->objects + granted->count) {
        std::fprintf(stderr, "wmr-v3: preflight DRM lease does not contain headset connector %u\n",
                     entry->connector);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::vector<uint32_t> scanoutFormats;
    try {
        scanoutFormats = headsetLinearFormats(fd, entry->connector);
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "wmr-v3: preflight cannot inspect DRM connector %u scanout: %s\n",
                     entry->connector, error.what());
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (duplicate < 0) {
        std::fprintf(stderr, "wmr-v3: preflight cannot duplicate DRM lease for headset connector %u "
                     "(errno %d)\n", entry->connector, errno);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    entry->scanoutFormats = std::move(scanoutFormats);
    entry->lease = duplicate;
    ++entry->generation;
    std::fprintf(stderr, "wmr-v3: preflight DRM lease device=%u:%u connector=%u modes=%zu\n",
                 major(entry->device), minor(entry->device), entry->connector, entry->modes.size());
    for (auto &native : surfaces) {
        if (native->display != entry || native->generation)
            continue;
        native->generation = entry->generation;
        native->formats.erase(std::remove_if(native->formats.begin(), native->formats.end(), [&](const auto &format) {
            return !std::binary_search(entry->scanoutFormats.begin(), entry->scanoutFormats.end(),
                                       outputFourcc(format.format));
        }), native->formats.end());
    }
    return VK_SUCCESS;
}

#ifdef MUXLESS_DESKTOP_FIXTURE
VKAPI_ATTR VkResult VKAPI_CALL acquireXlibDisplay(VkPhysicalDevice gpu, Display *xlib,
                                                  VkDisplayKHR id) {
    if (id == display)
        return VK_SUCCESS;
    return forwardPhysical<PFN_vkAcquireXlibDisplayEXT>("vkAcquireXlibDisplayEXT", gpu, xlib, id);
}
#endif

VKAPI_ATTR VkResult VKAPI_CALL releaseDisplay(VkPhysicalDevice gpu, VkDisplayKHR id) {
    {
        std::unique_lock lock(displayMutex);
        const auto entry = findDisplay(id);
        if (entry) {
            if (entry->gpu != gpu)
                return VK_ERROR_INITIALIZATION_FAILED;
            if (entry->lease >= 0)
                close(std::exchange(entry->lease, -1));
            entry->scanoutFormats.clear();
            lock.unlock();
            releaseHeadsetOutput(id);
            return VK_SUCCESS;
        }
    }
    if (id == display)
        return VK_SUCCESS;
    return forwardPhysical<PFN_vkReleaseDisplayEXT>("vkReleaseDisplayEXT", gpu, id);
}

template <class Entry, class Write>
VkResult displayList(VkPhysicalDevice gpu, uint32_t *count, Entry *out, Write write) {
    const std::lock_guard lock(displayMutex);
    size_t available{};
    size_t written{};
    const auto capacity = out ? *count : 0;
    for (const auto &entry : displays) {
        if (entry->gpu != gpu)
            continue;
        ++available;
        if (written < capacity)
            write(out[written++], headsetProperties(*entry));
    }
    if (!available)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *count = static_cast<uint32_t>(out ? written : available);
    return out && written < available ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL displayProperties(VkPhysicalDevice gpu, uint32_t *count,
                                                 VkDisplayPropertiesKHR *out) {
    if (headsetOutput() && bridgeRenderDevice(gpu)) {
        const auto result = displayList(gpu, count, out, [](auto &entry, auto value) { entry = value; });
        if (result != VK_ERROR_FEATURE_NOT_PRESENT)
            return result;
    }
    if (!headsetOutput())
        return one(count, out, displayProps);
    return forwardPhysical<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(
        "vkGetPhysicalDeviceDisplayPropertiesKHR", gpu, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL displayProperties2(VkPhysicalDevice gpu, uint32_t *count,
                                                  VkDisplayProperties2KHR *out) {
    if (headsetOutput() && bridgeRenderDevice(gpu)) {
        const auto result = displayList(gpu, count, out, [](auto &entry, auto value) { entry.displayProperties = value; });
        if (result != VK_ERROR_FEATURE_NOT_PRESENT)
            return result;
    }
    if (!headsetOutput())
        return one(count, out, &VkDisplayProperties2KHR::displayProperties, displayProps);
    return forwardPhysical<PFN_vkGetPhysicalDeviceDisplayProperties2KHR>(
        "vkGetPhysicalDeviceDisplayProperties2KHR", gpu, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL displayPlaneProperties(VkPhysicalDevice gpu, uint32_t *count,
                                                      VkDisplayPlanePropertiesKHR *out) {
    if (headsetOutput() && bridgeRenderDevice(gpu)) {
        const std::lock_guard lock(displayMutex);
        const auto entry = std::find_if(displays.begin(), displays.end(), [&](const auto &candidate) {
            return candidate->gpu == gpu;
        });
        if (entry != displays.end())
            return one(count, out, VkDisplayPlanePropertiesKHR{reinterpret_cast<VkDisplayKHR>(entry->get()), 0});
    }
    if (!headsetOutput())
        return one(count, out, planeProps);
    return forwardPhysical<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(
        "vkGetPhysicalDeviceDisplayPlanePropertiesKHR", gpu, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL displayPlaneProperties2(VkPhysicalDevice gpu, uint32_t *count,
                                                       VkDisplayPlaneProperties2KHR *out) {
    if (headsetOutput() && bridgeRenderDevice(gpu)) {
        const std::lock_guard lock(displayMutex);
        const auto entry = std::find_if(displays.begin(), displays.end(), [&](const auto &candidate) {
            return candidate->gpu == gpu;
        });
        if (entry != displays.end())
            return one(count, out, &VkDisplayPlaneProperties2KHR::displayPlaneProperties,
                       VkDisplayPlanePropertiesKHR{reinterpret_cast<VkDisplayKHR>(entry->get()), 0});
    }
    if (!headsetOutput())
        return one(count, out, &VkDisplayPlaneProperties2KHR::displayPlaneProperties, planeProps);
    return forwardPhysical<PFN_vkGetPhysicalDeviceDisplayPlaneProperties2KHR>(
        "vkGetPhysicalDeviceDisplayPlaneProperties2KHR", gpu, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL supportedDisplays(VkPhysicalDevice gpu, uint32_t plane,
                                                  uint32_t *count, VkDisplayKHR *out) {
    if (headsetOutput() && bridgeRenderDevice(gpu)) {
        if (plane != 0)
            return VK_ERROR_INITIALIZATION_FAILED;
        const auto result = displayList(gpu, count, out, [](auto &entry, auto properties) { entry = properties.display; });
        if (result != VK_ERROR_FEATURE_NOT_PRESENT)
            return result;
    }
    if (!headsetOutput())
        return plane == 0 ? one(count, out, display) : VK_ERROR_INITIALIZATION_FAILED;
    return forwardPhysical<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(
        "vkGetDisplayPlaneSupportedDisplaysKHR", gpu, plane, count, out);
}

template <class Entry, class Write>
VkResult listModes(const HeadsetDisplay &display, uint32_t *count, Entry *out, Write write) {
    return enumerate(display.modes.size(), count, out, [&](auto &entry, size_t i) {
        const auto &selected = display.modes[i];
        write(entry, VkDisplayModePropertiesKHR{reinterpret_cast<VkDisplayModeKHR>(uintptr_t(&selected)),
                                               selected.parameters});
    });
}

VKAPI_ATTR VkResult VKAPI_CALL displayModeProperties(VkPhysicalDevice gpu, VkDisplayKHR id,
                                                     uint32_t *count, VkDisplayModePropertiesKHR *out) {
    {
        const std::lock_guard lock(displayMutex);
        const auto entry = findDisplay(id);
        if (entry && entry->gpu == gpu)
            return listModes(*entry, count, out, [](auto &item, auto value) { item = value; });
    }
    if (id == display)
        return one(count, out, modeProps);
    return forwardPhysical<PFN_vkGetDisplayModePropertiesKHR>("vkGetDisplayModePropertiesKHR",
                                                              gpu, id, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL displayModeProperties2(VkPhysicalDevice gpu, VkDisplayKHR id,
                                                      uint32_t *count, VkDisplayModeProperties2KHR *out) {
    {
        const std::lock_guard lock(displayMutex);
        const auto entry = findDisplay(id);
        if (entry && entry->gpu == gpu)
            return listModes(*entry, count, out, [](auto &item, auto value) { item.displayModeProperties = value; });
    }
    if (id == display)
        return one(count, out, &VkDisplayModeProperties2KHR::displayModeProperties, modeProps);
    return forwardPhysical<PFN_vkGetDisplayModeProperties2KHR>("vkGetDisplayModeProperties2KHR",
                                                               gpu, id, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL createMode(VkPhysicalDevice gpu, VkDisplayKHR id,
                                          const VkDisplayModeCreateInfoKHR *info,
                                          const VkAllocationCallbacks *allocator, VkDisplayModeKHR *out) {
    {
        const std::lock_guard lock(displayMutex);
        const auto entry = findDisplay(id);
        if (entry && entry->gpu == gpu) {
            const auto it = std::find_if(entry->modes.begin(), entry->modes.end(), [&](const auto &candidate) {
                return sameMode(candidate.parameters, info->parameters);
            });
            if (it == entry->modes.end())
                return VK_ERROR_INITIALIZATION_FAILED;
            *out = reinterpret_cast<VkDisplayModeKHR>(&*it);
            return VK_SUCCESS;
        }
    }
    if (id == display) {
        if (!sameMode(info->parameters, modeProps.parameters))
            return VK_ERROR_INITIALIZATION_FAILED;
        *out = mode;
        return VK_SUCCESS;
    }
    return forwardPhysical<PFN_vkCreateDisplayModeKHR>("vkCreateDisplayModeKHR",
                                                        gpu, id, info, allocator, out);
}

bool setPlaneCapabilities(VkPhysicalDevice gpu, VkDisplayModeKHR id, VkDisplayPlaneCapabilitiesKHR *out) {
    if (id == mode) {
        *out = planeCaps;
        return true;
    }
    const std::lock_guard lock(displayMutex);
    std::shared_ptr<HeadsetDisplay> entry;
    const auto selected = findMode(id, entry);
    if (!selected || entry->gpu != gpu)
        return false;
    *out = planeCaps;
    out->minSrcExtent = out->maxSrcExtent = out->minDstExtent = out->maxDstExtent =
        selected->parameters.visibleRegion;
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL planeCapabilities(VkPhysicalDevice gpu, VkDisplayModeKHR id,
                                                 uint32_t plane, VkDisplayPlaneCapabilitiesKHR *out) {
    if (setPlaneCapabilities(gpu, id, out))
        return plane == 0 ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    return forwardPhysical<PFN_vkGetDisplayPlaneCapabilitiesKHR>("vkGetDisplayPlaneCapabilitiesKHR",
                                                                   gpu, id, plane, out);
}

VKAPI_ATTR VkResult VKAPI_CALL planeCapabilities2(VkPhysicalDevice gpu,
                                                  const VkDisplayPlaneInfo2KHR *info,
                                                  VkDisplayPlaneCapabilities2KHR *out) {
    if (setPlaneCapabilities(gpu, info->mode, &out->capabilities))
        return info->planeIndex == 0 ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    return forwardPhysical<PFN_vkGetDisplayPlaneCapabilities2KHR>("vkGetDisplayPlaneCapabilities2KHR",
                                                                  gpu, info, out);
}

bool supportedSurface(const VkDisplaySurfaceCreateInfoKHR &info, VkExtent2D extent) {
    return !info.flags && !info.pNext && !info.planeIndex && !info.planeStackIndex &&
           info.transform == VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR &&
           info.alphaMode == VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR &&
           info.imageExtent.width == extent.width && info.imageExtent.height == extent.height;
}

VKAPI_ATTR VkResult VKAPI_CALL createSurface(VkInstance instance,
                                             const VkDisplaySurfaceCreateInfoKHR *info,
                                             const VkAllocationCallbacks *allocator, VkSurfaceKHR *out) {
    if (info->displayMode == mode) {
        if (info->planeIndex != 0 || info->imageExtent.width != extent.width ||
            info->imageExtent.height != extent.height)
            return VK_ERROR_INITIALIZATION_FAILED;
        *out = surface;
        return VK_SUCCESS;
    }
    try {
        const std::lock_guard lock(displayMutex);
        std::shared_ptr<HeadsetDisplay> owner;
        if (const auto selected = findMode(info->displayMode, owner)) {
            const auto requested = selected->parameters.visibleRegion;
            if (owner->instance != instance || allocator || !supportedSurface(*info, requested)) {
                std::fprintf(stderr, "wmr-v3: unsupported DRM display surface %ux%u transform=%#x "
                             "for %ux%u native mode\n", info->imageExtent.width, info->imageExtent.height,
                             unsigned(info->transform), requested.width, requested.height);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            auto native = std::make_unique<HeadsetSurface>();
            native->display = owner;
            native->mode = *selected;
            native->generation = owner->lease >= 0 ? owner->generation : 0;
            negotiateFormats(*native);
            if (native->formats.empty()) {
                std::fprintf(stderr, "wmr-v3: no common Vulkan and linear DRM scanout format for connector %u\n",
                             owner->connector);
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            const auto handle = reinterpret_cast<VkSurfaceKHR>(native.get());
            surfaces.push_back(std::move(native));
            *out = handle;
            return VK_SUCCESS;
        }
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    const auto next = reinterpret_cast<PFN_vkCreateDisplayPlaneSurfaceKHR>(
        nextInstance(instance, "vkCreateDisplayPlaneSurfaceKHR"));
    return next ? next(instance, info, allocator, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL destroySurface(VkInstance instance, VkSurfaceKHR id,
                                          const VkAllocationCallbacks *allocator) {
    {
        const std::lock_guard lock(displayMutex);
        const auto entry = findSurface(id);
        if (entry != surfaces.end()) {
            surfaces.erase(entry);
            return;
        }
    }
    if (id == surface)
        return;
    const auto next = reinterpret_cast<PFN_vkDestroySurfaceKHR>(nextInstance(instance,
                                                                              "vkDestroySurfaceKHR"));
    if (next)
        next(instance, id, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL surfaceSupport(VkPhysicalDevice gpu, uint32_t queueFamily,
                                              VkSurfaceKHR id, VkBool32 *out) {
    {
        const std::lock_guard lock(displayMutex);
        const auto entry = findSurface(id);
        if (entry != surfaces.end()) {
            const auto &display = *(*entry)->display;
            *out = display.gpu == gpu && queueFamily < display.queues.size() &&
                   (display.queues[queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT);
            return VK_SUCCESS;
        }
    }
    if (id == surface) {
        *out = VK_TRUE;
        return VK_SUCCESS;
    }
    return forwardPhysical<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
        "vkGetPhysicalDeviceSurfaceSupportKHR", gpu, queueFamily, id, out);
}

bool setSurfaceCapabilities(VkSurfaceKHR id, VkSurfaceCapabilitiesKHR *out) {
    if (id == surface) {
        *out = surfaceCaps;
        return true;
    }
    const std::lock_guard lock(displayMutex);
    const auto entry = findSurface(id);
    if (entry == surfaces.end())
        return false;
    *out = headsetCapabilities(**entry);
    return true;
}

VKAPI_ATTR VkResult VKAPI_CALL surfaceCapabilities(VkPhysicalDevice gpu, VkSurfaceKHR id,
                                                   VkSurfaceCapabilitiesKHR *out) {
    if (setSurfaceCapabilities(id, out))
        return VK_SUCCESS;
    return forwardPhysical<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", gpu, id, out);
}

VKAPI_ATTR VkResult VKAPI_CALL surfaceCapabilities2(VkPhysicalDevice gpu,
                                                    const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                                    VkSurfaceCapabilities2KHR *out) {
    if (setSurfaceCapabilities(info->surface, &out->surfaceCapabilities))
        return VK_SUCCESS;
    return forwardPhysical<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(
        "vkGetPhysicalDeviceSurfaceCapabilities2KHR", gpu, info, out);
}

VKAPI_ATTR VkResult VKAPI_CALL surfaceCapabilitiesEXT(VkPhysicalDevice gpu, VkSurfaceKHR id,
                                                      VkSurfaceCapabilities2EXT *out) {
    VkSurfaceCapabilitiesKHR caps{};
    if (!setSurfaceCapabilities(id, &caps))
        return forwardPhysical<PFN_vkGetPhysicalDeviceSurfaceCapabilities2EXT>(
            "vkGetPhysicalDeviceSurfaceCapabilities2EXT", gpu, id, out);
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
    return VK_SUCCESS;
}

template <class Entry, class Write>
VkResult surfaceFormatList(VkSurfaceKHR id, uint32_t *count, Entry *out, Write write) {
    if (id == surface)
        return enumerate(formats.size(), count, out, [&](auto &entry, size_t i) { write(entry, formats[i]); });
    const std::lock_guard lock(displayMutex);
    const auto entry = findSurface(id);
    if (entry == surfaces.end())
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const auto &native = (*entry)->formats;
    return enumerate(native.size(), count, out, [&](auto &item, size_t i) { write(item, native[i]); });
}

VKAPI_ATTR VkResult VKAPI_CALL surfaceFormats(VkPhysicalDevice gpu, VkSurfaceKHR id,
                                              uint32_t *count, VkSurfaceFormatKHR *out) {
    const auto result = surfaceFormatList(id, count, out, [](auto &entry, auto format) { entry = format; });
    if (result != VK_ERROR_FEATURE_NOT_PRESENT)
        return result;
    return forwardPhysical<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
        "vkGetPhysicalDeviceSurfaceFormatsKHR", gpu, id, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL surfaceFormats2(VkPhysicalDevice gpu,
                                               const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                               uint32_t *count, VkSurfaceFormat2KHR *out) {
    const auto result = surfaceFormatList(info->surface, count, out,
                                          [](auto &entry, auto format) { entry.surfaceFormat = format; });
    if (result != VK_ERROR_FEATURE_NOT_PRESENT)
        return result;
    return forwardPhysical<PFN_vkGetPhysicalDeviceSurfaceFormats2KHR>(
        "vkGetPhysicalDeviceSurfaceFormats2KHR", gpu, info, count, out);
}

VKAPI_ATTR VkResult VKAPI_CALL presentModes(VkPhysicalDevice gpu, VkSurfaceKHR id,
                                            uint32_t *count, VkPresentModeKHR *out) {
    if (virtualSurface(id))
        return one(count, out, VK_PRESENT_MODE_FIFO_KHR);
    return forwardPhysical<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
        "vkGetPhysicalDeviceSurfacePresentModesKHR", gpu, id, count, out);
}

} // namespace

struct DesktopVblank::Impl {
    struct Fence {
        Clock::time_point deadline;
        uint32_t handle;
    };

    int drm{-1};
    int stop{-1};
    uint32_t crtc{};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Fence> pending;
    bool stopping{};
    std::atomic<bool> lost{};
    std::thread worker;

    ~Impl() {
        if (worker.joinable()) {
            if (stop >= 0) {
                while (eventfd_write(stop, 1) < 0 && errno == EINTR) {}
            } else {
                {
                    const std::lock_guard lock(mutex);
                    stopping = true;
                }
                changed.notify_one();
            }
            worker.join();
        }
        if (stop >= 0)
            close(stop);
        if (drm >= 0)
            close(drm);
    }

    void signal(uint32_t handle) {
        if (drmSyncobjSignal(drm, &handle, 1))
            lost.store(true, std::memory_order_relaxed);
        drmSyncobjDestroy(drm, handle);
    }

    void dispatchVirtual() {
        std::unique_lock lock(mutex);
        while (!stopping) {
            changed.wait(lock, [&] { return stopping || !pending.empty(); });
            if (stopping)
                return;
            const auto event = pending.front();
            if (changed.wait_until(lock, event.deadline, [&] { return stopping; }))
                return;
            pending.pop_front();
            lock.unlock();
            signal(event.handle);
            lock.lock();
        }
    }

    void dispatch() {
        drmEventContext context{};
        context.version = 4;
        context.sequence_handler = [](int fd, uint64_t, uint64_t, uint64_t cookie) {
            auto handle = uint32_t(cookie);
            drmSyncobjSignal(fd, &handle, 1);
            drmSyncobjDestroy(fd, handle);
        };
        pollfd files[]{{drm, POLLIN, 0}, {stop, POLLIN, 0}};
        for (;;) {
            if (poll(files, 2, -1) < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (files[1].revents)
                return;
            if (files[0].revents & (POLLERR | POLLHUP | POLLNVAL))
                break;
            if (files[0].revents & POLLIN) {
                errno = 0;
                if (drmHandleEvent(drm, &context) && errno != EINTR && errno != EAGAIN)
                    break;
            }
        }
        lost.store(true, std::memory_order_relaxed);
    }
};

DesktopVblank::DesktopVblank(const char *renderNode) : impl(std::make_unique<Impl>()) {
    auto &s = *impl;
    const int render = open(renderNode, O_RDONLY | O_CLOEXEC);
    if (render < 0)
        throw std::system_error(errno, std::generic_category(), renderNode);
    if (virtualRefreshClock()) {
        s.drm = render;
        s.worker = std::thread([&s] { s.dispatchVirtual(); });
        return;
    }
    drmDevicePtr device{};
    const auto result = drmGetDevice2(render, 0, &device);
    close(render);
    if (result)
        throw std::system_error(-result, std::generic_category(), "find desktop DRM device");
    if (device->available_nodes & (1 << DRM_NODE_PRIMARY))
        s.drm = open(device->nodes[DRM_NODE_PRIMARY], O_RDONLY | O_CLOEXEC);
    drmFreeDevice(&device);
    if (s.drm < 0)
        throw std::runtime_error("Cannot open a desktop DRM event client");

    using Resources = std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)>;
    using Connector = std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>;
    using Encoder = std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)>;
    Resources resources(drmModeGetResources(s.drm), drmModeFreeResources);
    if (!resources)
        throw std::runtime_error("Cannot query desktop display resources");
    for (int i = 0; i < resources->count_connectors; ++i) {
        Connector connector(drmModeGetConnectorCurrent(s.drm, resources->connectors[i]),
                            drmModeFreeConnector);
        if (!connector || connector->connector_type != DRM_MODE_CONNECTOR_eDP ||
            connector->connector_type_id != 1 || !connector->encoder_id)
            continue;
        Encoder encoder(drmModeGetEncoder(s.drm, connector->encoder_id), drmModeFreeEncoder);
        if (encoder)
            s.crtc = encoder->crtc_id;
        break;
    }
    uint64_t sequence{}, time{};
    if (!s.crtc || drmCrtcGetSequence(s.drm, s.crtc, &sequence, &time))
        throw std::runtime_error("Cannot read vblank for eDP-1");
    s.stop = eventfd(0, EFD_CLOEXEC);
    if (s.stop < 0)
        throw std::system_error(errno, std::generic_category(), "create vblank shutdown event");
    s.worker = std::thread([&s] { s.dispatch(); });
}

DesktopVblank::~DesktopVblank() = default;

VkResult DesktopVblank::queue(int fenceFd) {
    auto &s = *impl;
    if (s.lost.load(std::memory_order_relaxed))
        return VK_ERROR_OUT_OF_DATE_KHR;
    uint32_t handle{};
    if (drmSyncobjFDToHandle(s.drm, fenceFd, &handle))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (virtualRefreshClock()) {
        try {
            const std::lock_guard lock(s.mutex);
            s.pending.push_back({startOfTick(tickAt(Clock::now()) + 1), handle});
        } catch (const std::bad_alloc &) {
            drmSyncobjDestroy(s.drm, handle);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        s.changed.notify_one();
        return VK_SUCCESS;
    }
    uint64_t sequence{};
    if (!drmCrtcQueueSequence(s.drm, s.crtc, DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS,
                              1, &sequence, handle))
        return VK_SUCCESS;
    drmSyncobjDestroy(s.drm, handle);
    return VK_ERROR_OUT_OF_DATE_KHR;
}

VkResult DesktopVblank::counter(uint64_t *value) const {
    if (impl->lost.load(std::memory_order_relaxed))
        return VK_ERROR_OUT_OF_DATE_KHR;
    if (virtualRefreshClock()) {
        *value = tickAt(Clock::now());
        return VK_SUCCESS;
    }
    uint64_t time{};
    return drmCrtcGetSequence(impl->drm, impl->crtc, value, &time) ? VK_ERROR_OUT_OF_DATE_KHR : VK_SUCCESS;
}

bool virtualDisplay(VkDisplayKHR id) {
    if (id == display)
        return true;
    const std::lock_guard lock(displayMutex);
    return bool(findDisplay(id));
}

bool virtualSurface(VkSurfaceKHR id) {
    if (id == surface)
        return true;
    const std::lock_guard lock(displayMutex);
    return findSurface(id) != surfaces.end();
}

VkDisplayKHR headsetDisplayForSurface(VkSurfaceKHR id) {
    const std::lock_guard lock(displayMutex);
    const auto it = findSurface(id);
    if (it == surfaces.end())
        return VK_NULL_HANDLE;
    const auto &entry = **it;
    if (entry.display->lease < 0 || entry.generation != entry.display->generation)
        return VK_NULL_HANDLE;
    return reinterpret_cast<VkDisplayKHR>(entry.display.get());
}

void releaseVirtualDisplays(VkInstance instance) {
    const std::lock_guard lock(displayMutex);
    surfaces.erase(std::remove_if(surfaces.begin(), surfaces.end(), [&](const auto &entry) {
        return entry->display->instance == instance;
    }), surfaces.end());
    displays.erase(std::remove_if(displays.begin(), displays.end(), [&](const auto &entry) {
        return entry->instance == instance;
    }), displays.end());
}

#ifdef MUXLESS_DESKTOP_FIXTURE
bool headsetOutput() {
    static const auto enabled = [] {
        const auto *output = std::getenv("V3_OUTPUT");
        return output && !std::strcmp(output, "headset");
    }();
    return enabled;
}
#endif

HeadsetLease::~HeadsetLease() {
    if (fd >= 0)
        close(fd);
}

HeadsetLease copyHeadsetLease(VkPhysicalDevice physical, VkSurfaceKHR id) {
    if (!headsetOutput())
        return {-1, 0};
    const std::lock_guard lock(displayMutex);
    const auto it = findSurface(id);
    if (it == surfaces.end())
        return {-1, 0};
    auto &surface = **it;
    const auto &entry = *surface.display;
    if (entry.gpu != physical || entry.lease < 0 ||
        surface.generation != entry.generation || surface.formats.empty())
        return {-1, entry.connector};
    const int fd = fcntl(entry.lease, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        std::fprintf(stderr, "wmr-v3: preflight cannot duplicate stored DRM lease for headset connector %u "
                     "(errno %d)\n", entry.connector, errno);
        return {-1, entry.connector};
    }
    return {fd, entry.connector, surface.mode.parameters, surface.mode.timing, entry.device};
}

bool virtualRefreshClock() {
    static const bool enabled = [] {
        const auto *clock = std::getenv("V3_VBLANK");
        return clock && !std::strcmp(clock, "virtual90");
    }();
    return enabled;
}

PFN_vkVoidFunction virtualDisplayHook(const char *name) {
#define HOOK(api, fn) if (std::strcmp(name, #api) == 0) return reinterpret_cast<PFN_vkVoidFunction>(fn)
    if (headsetOutput()) {
        HOOK(vkGetDrmDisplayEXT, getDrmDisplay);
        HOOK(vkAcquireDrmDisplayEXT, acquireDrmDisplay);
    } else {
#ifdef MUXLESS_DESKTOP_FIXTURE
        HOOK(vkGetRandROutputDisplayEXT, randrDisplay);
        HOOK(vkAcquireXlibDisplayEXT, acquireXlibDisplay);
#endif
    }
    HOOK(vkReleaseDisplayEXT, releaseDisplay);
    HOOK(vkGetPhysicalDeviceDisplayPropertiesKHR, displayProperties);
    HOOK(vkGetPhysicalDeviceDisplayProperties2KHR, displayProperties2);
    HOOK(vkGetPhysicalDeviceDisplayPlanePropertiesKHR, displayPlaneProperties);
    HOOK(vkGetPhysicalDeviceDisplayPlaneProperties2KHR, displayPlaneProperties2);
    HOOK(vkGetDisplayPlaneSupportedDisplaysKHR, supportedDisplays);
    HOOK(vkGetDisplayModePropertiesKHR, displayModeProperties);
    HOOK(vkGetDisplayModeProperties2KHR, displayModeProperties2);
    HOOK(vkCreateDisplayModeKHR, createMode);
    HOOK(vkGetDisplayPlaneCapabilitiesKHR, planeCapabilities);
    HOOK(vkGetDisplayPlaneCapabilities2KHR, planeCapabilities2);
    HOOK(vkCreateDisplayPlaneSurfaceKHR, createSurface);
    HOOK(vkDestroySurfaceKHR, destroySurface);
    HOOK(vkGetPhysicalDeviceSurfaceSupportKHR, surfaceSupport);
    HOOK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR, surfaceCapabilities);
    HOOK(vkGetPhysicalDeviceSurfaceCapabilities2KHR, surfaceCapabilities2);
    HOOK(vkGetPhysicalDeviceSurfaceCapabilities2EXT, surfaceCapabilitiesEXT);
    HOOK(vkGetPhysicalDeviceSurfaceFormatsKHR, surfaceFormats);
    HOOK(vkGetPhysicalDeviceSurfaceFormats2KHR, surfaceFormats2);
    HOOK(vkGetPhysicalDeviceSurfacePresentModesKHR, presentModes);
#undef HOOK
    return nullptr;
}

}
