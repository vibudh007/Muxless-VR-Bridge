#include "transport.hpp"
#include "virtual-display.hpp"
#include "wsi-trace.hpp"
#include "desktop.hpp"
#include "headset.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <time.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>

namespace v3 {
namespace {

void check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " (" + std::to_string(result) + ")");
}

class Fd {
    int fd_;
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { if (fd_ >= 0) close(fd_); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
};

struct Image {
    VkImage handle{};
    VkDeviceMemory memory{};
};

struct ExportedImage {
    Fd fd;
    uint32_t pitch, offset;
};

// A new DRM client on the exporting GPU holds handles to precisely these BOs.
// Same-device PRIME import references the original GEM object without attaching
// or pinning it. fdinfo tracks the actual TTM location of those objects.
class ResidencyObserver {
    Fd device;
    uint64_t bytes{};
    uint64_t initialVram{};
    uint64_t initialGtt{};

    static int openNode(const Vulkan &vk) {
        VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &drm;
        const auto query = vk.physicalFn<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
        if (query)
            query(vk.physical, &props);
        if (!drm.hasRender)
            throw std::runtime_error("Cannot identify the exporting Vulkan render node");
        const auto path = "/dev/dri/renderD" + std::to_string(drm.renderMinor);
        const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        struct stat info{};
        if (fd < 0 || fstat(fd, &info) || !S_ISCHR(info.st_mode) ||
            major(info.st_rdev) != drm.renderMajor || minor(info.st_rdev) != drm.renderMinor) {
            if (fd >= 0)
                close(fd);
            throw std::runtime_error("Cannot open a separate DRM client for the exporting GPU");
        }
        return fd;
    }

public:
    explicit ResidencyObserver(const Vulkan &vk) : device(openNode(vk)) {
        std::ifstream info("/proc/self/fdinfo/" + std::to_string(device.get()));
        if (!info)
            throw std::runtime_error("Cannot read the residency observer's fdinfo");
        for (std::string line; std::getline(info, line); ) {
            std::istringstream fields(line);
            std::string key, unit;
            uint64_t value = 0;
            if (!(fields >> key >> value) || (key != "drm-resident-vram:" && key != "drm-resident-gtt:"))
                continue;
            fields >> unit;
            constexpr const char *units[] = {"KiB", "MiB", "GiB", "TiB"};
            uint64_t scale = 1;
            for (const auto *candidate : units) {
                scale *= 1024;
                if (unit == candidate) {
                    value *= scale;
                    break;
                }
            }
            (key == "drm-resident-vram:" ? initialVram : initialGtt) = value;
        }
    }

    void observe(int dmaBuf) {
        struct stat info{};
        uint32_t handle = 0;
        if (fstat(dmaBuf, &info) || info.st_size <= 0 ||
            drmPrimeFDToHandle(device.get(), dmaBuf, &handle))
            throw std::runtime_error("Cannot observe the exported DMA-BUF on its original GPU");
        bytes += info.st_size;
    }

    void publish(const char *path) const {
        std::ostringstream receipt;
        receipt << "{\"pid\":" << getpid() << ",\"fd\":" << device.get()
                << ",\"bytes\":" << bytes << ",\"initial_vram_bytes\":" << initialVram
                << ",\"initial_gtt_bytes\":" << initialGtt << "}\n";
        // Give the test sampler the observer file once, so it can read live
        // fdinfo outside the compositor, without touching frame submissions.
        Fd socketFd(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (std::strlen(path) >= sizeof(address.sun_path))
            throw std::runtime_error("Residency sampler socket path is too long");
        std::strcpy(address.sun_path, path);
        if (socketFd.get() < 0 || connect(socketFd.get(), reinterpret_cast<sockaddr *>(&address), sizeof(address)))
            throw std::runtime_error("Cannot connect to the benchmark residency sampler");
        auto text = receipt.str();
        iovec data{text.data(), text.size()};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
        msghdr message{};
        message.msg_iov = &data;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        auto *cmsg = CMSG_FIRSTHDR(&message);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int));
        const int fd = device.get();
        std::memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
        if (sendmsg(socketFd.get(), &message, MSG_NOSIGNAL) != static_cast<ssize_t>(text.size()))
            throw std::runtime_error("Cannot pass the observer descriptor to the sampler");
    }
};

struct Slot {
    Image shared;
    Image local;
    VkSemaphore ready{};
    VkSemaphore released{};
    VkSemaphore scanoutReady{};
    VkCommandBuffer acquireFirst{};
    VkCommandBuffer acquireAgain{};
    VkCommandBuffer presentFirst{};
    VkCommandBuffer presentAgain{};
    uint64_t point{};
    uint64_t relayPoint{};
    uint64_t frame{};
    bool acquired{};
    uint32_t writeRanges{};
    bool writing{};
};

uint64_t monotonic() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
}

struct Sample {
    uint64_t frame{}, before{}, after{}, gpu{};
    uint64_t legacyId{}, id{};
    uint32_t slot{};
    uint32_t writeRanges{};
    uint64_t outputSpan{}, outputSum{};
    uint32_t producerWaits{};
    std::array<uint64_t, 4> copyTicks{};
};

constexpr uint32_t maxWriteRanges = 8;
constexpr uint32_t maxSlots = 4;

uint32_t fourcc(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return DRM_FORMAT_XBGR8888;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return DRM_FORMAT_XRGB8888;
    default:
        throw std::runtime_error("Frame feed needs uncompressed 8-bit RGBA or BGRA (VkFormat " +
                                 std::to_string(format) + ")");
    }
}

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageLayout before, VkImageLayout after,
                                   VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                                   VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                                   uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
                                   uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = before;
    b.newLayout = after;
    b.srcQueueFamilyIndex = srcFamily;
    b.dstQueueFamilyIndex = dstFamily;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

VkSemaphoreSubmitInfo semaphoreInfo(VkSemaphore semaphore, uint64_t point) {
    VkSemaphoreSubmitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    info.semaphore = semaphore;
    info.value = point;
    info.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    return info;
}

constexpr auto all = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
constexpr auto transfer = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
constexpr auto write = VK_ACCESS_2_MEMORY_WRITE_BIT;
constexpr auto readWrite = VK_ACCESS_2_MEMORY_READ_BIT | write;

struct ImageSupport {
    VkResult result{};
    VkImageFormatProperties limits{};
    VkExternalMemoryProperties external{};
};

ImageSupport queryImage(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceImageFormatProperties2 query,
                        const VkImageCreateInfo &info, uint64_t modifier) {
    const bool shared = info.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    const VkPhysicalDeviceImageDrmFormatModifierInfoEXT layout{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, nullptr, modifier,
        info.sharingMode, info.queueFamilyIndexCount, info.pQueueFamilyIndices};
    const VkPhysicalDeviceExternalImageFormatInfo external{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, &layout,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const VkPhysicalDeviceImageFormatInfo2 request{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        shared ? &external : nullptr, info.format, info.imageType, info.tiling, info.usage, info.flags};
    VkExternalImageFormatProperties handle{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 limits{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    limits.pNext = shared ? &handle : nullptr;
    const auto result = query(physical, &request, &limits);
    return {result, limits.imageFormatProperties, handle.externalMemoryProperties};
}

bool fitsImage(const VkImageFormatProperties &limits, const VkImageCreateInfo &info) {
    return limits.maxExtent.width >= info.extent.width && limits.maxExtent.height >= info.extent.height &&
           limits.maxExtent.depth >= info.extent.depth && limits.maxMipLevels >= info.mipLevels &&
           limits.maxArrayLayers >= info.arrayLayers && (limits.sampleCounts & info.samples);
}

bool canExportDmaBuf(const VkExternalMemoryProperties &properties) {
    return (properties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) &&
           (properties.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
}

} // namespace

bool supportsLinearFrameFeed(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceImageFormatProperties2 query,
                             VkFormat format, VkExtent2D extent, VkImageUsageFlags renderUsage) {
    if (!query || !extent.width || !extent.height)
        return false;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const auto shared = queryImage(physical, query, info, DRM_FORMAT_MOD_LINEAR);
    if (shared.result != VK_SUCCESS || !fitsImage(shared.limits, info) || !canExportDmaBuf(shared.external))
        return false;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = renderUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const auto local = queryImage(physical, query, info, DRM_FORMAT_MOD_LINEAR);
    return local.result == VK_SUCCESS && fitsImage(local.limits, info);
}

#define VK_D(name) vk.deviceFn<PFN_vk##name>("vk" #name)
#define VK_P(name) vk.physicalFn<PFN_vk##name>("vk" #name)

struct FrameRing::Impl {
    Impl(Vulkan v, std::shared_ptr<std::mutex> queueMutex)
        : vk(v), privateQueueMutex(std::move(queueMutex)) {}
    ~Impl();

    Vulkan vk;
    Transport mode{};
    VkExtent2D extent{};
    VkFormat format{};
    uint64_t sharedModifier{DRM_FORMAT_MOD_LINEAR};
    VkQueue acquireQueue{};
    VkQueue copyQueue{};
    std::shared_ptr<std::mutex> privateQueueMutex;
    uint32_t acquireFamily{};
    uint32_t copyFamily{};
    std::vector<uint32_t> families;
    bool sourceBarrier{};
    PFN_vkSetDeviceLoaderData setLoaderData{};
    PFN_vkQueueSubmit2 queueSubmit{};
    PFN_vkWaitSemaphores waitSemaphores{};
    PFN_vkGetSemaphoreFdKHR semaphoreFd{};
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd{};
    PFN_vkImportFenceFdKHR importFenceFd{};
    PFN_vkResetFences resetFences{};
    PFN_vkCmdPipelineBarrier2 pipelineBarrier{};
    PFN_vkGetSemaphoreCounterValue semaphoreCounter{};
    PFN_vkGetQueryPoolResults queryResults{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::unique_ptr<FramePresenter> presenter;
    HeadsetPresenter *headset{};
    std::unique_ptr<ResidencyObserver> residency;
    VkCommandPool acquirePool{};
    VkCommandPool presentPool{};
    VkQueryPool queryPool{};
    VkQueryPool outputQueries{};
    float timestampPeriod{};
    uint64_t timestampMask{};
    uint64_t outputTimestampMask{};
    std::FILE *trace{};
    std::FILE *copyTrace{};
    std::array<char, 65536> traceBuffer{};
    std::array<Sample, 128> samples{};
    unsigned pendingTrace{};
    uint32_t queriesPerSlot{2};
    uint64_t lastCalibration{};
    std::array<uint64_t, 2> calibration{};
    uint64_t clockDeviation{};
    std::vector<Slot> slots;
    std::vector<VkImage> images;
    std::vector<VkSemaphoreSubmitInfo> waits;
    std::mutex state;
    std::condition_variable desktopPresentQueued;
    uint32_t cursor{};
    uint64_t frame{};
    uint64_t completedLegacyId{}, completedId{};
    uint64_t queuedDesktopId{};
    bool submitted{};
    VkResult fault{VK_SUCCESS};
    bool releaseGpuResources{true};
    Impl *nextDeferred{};
    static std::mutex deferredMutex;
    static Impl *deferred;

    void initialize(const VkSwapchainCreateInfoKHR &, Transport, VkQueue, uint32_t, uint32_t,
                    PFN_vkSetDeviceLoaderData, VkQueue, uint32_t, const HeadsetLease &,
                    const std::vector<uint32_t> &);
    bool copies() const { return mode == Transport::copy || copyQueue; }
    VkImageCreateInfo imageInfo(VkImageUsageFlags, bool shared) const;
    void preflightImage(const VkImageCreateInfo &) const;
    VkDeviceSize createImage(Image &, VkImageCreateInfo, uint32_t rejected = 0, uint32_t *typeUsed = nullptr);
    void destroyImage(Image &) const;
    ExportedImage exportImage(const Image &, VkDeviceSize) const;
    uint32_t importHeadsetBuffer(Slot &, const VkImageCreateInfo &, uint32_t fourcc, uint32_t &rejected);
    VkSemaphore makeSemaphore(VkSemaphoreType, VkExternalSemaphoreHandleTypeFlagBits = {}) const;
    int timelineFd(VkSemaphore) const;
    VkResult signalAcquire(VkSemaphore, VkFence) const;
    VkCommandPool pool(uint32_t) const;
    VkCommandBuffer command(VkCommandPool) const;
    void encodeAcquire(VkCommandBuffer, const Slot &, bool first) const;
    void encodePresent(VkCommandBuffer, const Slot &, bool first, uint32_t index) const;
    void barrier(VkCommandBuffer, const VkImageMemoryBarrier2 &) const;
    void begin(VkCommandBuffer) const;
    void finishSample(const Slot &, Sample &) const;
    void writeCopySample(const Sample &, const DesktopFeedback &);
    void feedback();
    void stopOutput();
    bool drain() const;
    VkResult fail(VkResult result);
    VkResult submit(VkQueue, VkCommandBuffer, uint32_t, const VkSemaphoreSubmitInfo *,
                    uint32_t, const VkSemaphoreSubmitInfo *, VkFence) const;
};

std::mutex FrameRing::Impl::deferredMutex;
FrameRing::Impl *FrameRing::Impl::deferred{};

void FrameRing::Impl::initialize(const VkSwapchainCreateInfoKHR &ci, Transport kind,
                                VkQueue q, uint32_t acquireFamily, uint32_t renderFamily,
                                PFN_vkSetDeviceLoaderData loader, VkQueue dmaQueue, uint32_t copyFamily,
                                const HeadsetLease &lease, const std::vector<uint32_t> &appFamilies) {
    const bool forHeadset = lease.fd >= 0;
    const VkExtent2D requiredExtent = forHeadset ? lease.mode.visibleRegion : VkExtent2D{2880, 1440};
    if (!requiredExtent.width || !requiredExtent.height ||
        ci.imageExtent.width != requiredExtent.width || ci.imageExtent.height != requiredExtent.height)
        throw std::runtime_error("Frame feed requires " + std::to_string(requiredExtent.width) + "x" +
                                 std::to_string(requiredExtent.height) + "; received " +
                                 std::to_string(ci.imageExtent.width) + "x" +
                                 std::to_string(ci.imageExtent.height));
    constexpr auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!ci.imageUsage || (ci.imageUsage & ~usage))
        throw std::runtime_error("Unsupported frame feed image usage mask " + std::to_string(ci.imageUsage));
    if (ci.imageArrayLayers != 1 || ci.flags || ci.minImageCount > maxSlots ||
        ci.preTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ||
        ci.imageSharingMode != VK_SHARING_MODE_EXCLUSIVE) {
        char description[160];
        std::snprintf(description, sizeof(description),
                      "Unsupported frame feed shape (layers %u, swapchain flags %#x, minimum images %u, "
                      "transform %#x, sharing mode %u)", ci.imageArrayLayers, unsigned(ci.flags),
                      ci.minImageCount, unsigned(ci.preTransform), unsigned(ci.imageSharingMode));
        throw std::runtime_error(description);
    }
    if (ci.compositeAlpha != VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR || ci.presentMode != VK_PRESENT_MODE_FIFO_KHR)
        throw std::runtime_error("Frame feed requires opaque alpha and FIFO presentation (alpha " +
                                 std::to_string(ci.compositeAlpha) + ", mode " +
                                 std::to_string(ci.presentMode) + ")");
    const auto drmFormat = fourcc(ci.imageFormat);
    const bool privateCopy = kind == Transport::sdma || kind == Transport::ace;
    if (!privateQueueMutex || (forHeadset ? (q || !dmaQueue) : !q) ||
        (privateCopy && !dmaQueue) || (!dmaQueue && copyFamily != renderFamily) ||
        (forHeadset && kind != Transport::copy && !privateCopy))
        throw std::runtime_error("Frame feed queue configuration is invalid (acquire family " +
                                 std::to_string(acquireFamily) + ", render family " +
                                 std::to_string(renderFamily) + ", copy family " +
                                 std::to_string(copyFamily) + ")");
    queueSubmit = VK_D(QueueSubmit2);
    if (!queueSubmit)
        queueSubmit = VK_D(QueueSubmit2KHR);
    waitSemaphores = VK_D(WaitSemaphores);
    if (!waitSemaphores)
        waitSemaphores = VK_D(WaitSemaphoresKHR);
    pipelineBarrier = VK_D(CmdPipelineBarrier2);
    if (!pipelineBarrier)
        pipelineBarrier = VK_D(CmdPipelineBarrier2KHR);
    if (!queueSubmit || !pipelineBarrier)
        throw std::runtime_error("Frame feed needs vkQueueSubmit2 and vkCmdPipelineBarrier2 (synchronization2)");
    semaphoreFd = VK_D(GetSemaphoreFdKHR);
    if (!waitSemaphores || !semaphoreFd)
        throw std::runtime_error("Frame feed needs vkWaitSemaphores and vkGetSemaphoreFdKHR");
    if (forHeadset) {
        importSemaphoreFd = VK_D(ImportSemaphoreFdKHR);
        importFenceFd = VK_D(ImportFenceFdKHR);
        resetFences = VK_D(ResetFences);
        if (!importSemaphoreFd || !importFenceFd || !resetFences)
            throw std::runtime_error("Headset frame acquisition needs binary SYNC_FD semaphore and fence import");
    }
    if (!VK_D(GetMemoryFdKHR))
        throw std::runtime_error("Frame feed needs vkGetMemoryFdKHR (DMA-BUF export)");
    if (!loader)
        throw std::runtime_error("Frame feed needs Vulkan loader dispatch for private command buffers");
    mode = kind;
#ifdef MUXLESS_DESKTOP_FIXTURE
    if (const auto *requested = std::getenv("V3_MODIFIER")) {
        if (forHeadset || std::strcmp(requested, "tile64") || mode != Transport::direct)
            throw std::runtime_error("V3_MODIFIER=tile64 is available only for the direct desktop test");
        sharedModifier = fourcc_mod_code(AMD, AMD_FMT_MOD_SET(TILE_VERSION, AMD_FMT_MOD_TILE_VER_GFX9) |
                                              AMD_FMT_MOD_SET(TILE, AMD_FMT_MOD_TILE_GFX9_64K_S));
    }
#endif
    extent = ci.imageExtent;
    format = ci.imageFormat;
    this->acquireFamily = acquireFamily;
    this->copyFamily = copyFamily;
    families.reserve(appFamilies.size() + 3);
    const auto addFamily = [this](uint32_t family) {
        if (std::find(families.begin(), families.end(), family) == families.end())
            families.push_back(family);
    };
    if (q)
        addFamily(acquireFamily);
    addFamily(renderFamily);
    addFamily(copyFamily);
    for (const auto family : appFamilies)
        addFamily(family);
    acquireQueue = q;
    copyQueue = dmaQueue;
    sourceBarrier = !copyQueue || copyFamily == renderFamily;
    setLoaderData = loader;

    auto semaphoreProperties = VK_P(GetPhysicalDeviceExternalSemaphoreProperties);
    if (!semaphoreProperties)
        semaphoreProperties = VK_P(GetPhysicalDeviceExternalSemaphorePropertiesKHR);
    if (!semaphoreProperties)
        throw std::runtime_error("Frame feed cannot query Vulkan external semaphore capabilities");
    VkSemaphoreTypeCreateInfo timelineType{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkPhysicalDeviceExternalSemaphoreInfo semInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
    semInfo.pNext = forHeadset ? nullptr : &timelineType;
    semInfo.handleType = forHeadset ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                                    : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalSemaphoreProperties semProps{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
    semaphoreProperties(vk.physical, &semInfo, &semProps);
    constexpr auto syncFlags = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
                               VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    if ((semProps.externalSemaphoreFeatures & syncFlags) != syncFlags ||
        !(semProps.compatibleHandleTypes & semInfo.handleType))
        throw std::runtime_error(forHeadset
            ? "Vulkan producer does not support importing and exporting binary SYNC_FD semaphores"
            : "Vulkan producer does not support importable and exportable OPAQUE_FD timelines");

    const auto sharedInfo = imageInfo(copies() ? VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                                               : ci.imageUsage, true);
    preflightImage(sharedInfo);
    const auto localInfo = imageInfo(ci.imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, false);
    if (copies())
        preflightImage(localInfo);
    VK_P(GetPhysicalDeviceMemoryProperties)(vk.physical, &memory);
    if (q)
        acquirePool = pool(acquireFamily);
    presentPool = pool(copyFamily);
    if (forHeadset) {
        auto output = std::make_unique<HeadsetPresenter>(lease.fd, lease.connector, lease.timing);
        if (!output->supportsBuffer(drmFormat, sharedModifier))
            throw std::runtime_error("Leased headset plane does not report a linear framebuffer (fourcc " +
                                     std::to_string(drmFormat) + ", VkFormat " +
                                     std::to_string(ci.imageFormat) + ")");
        headset = output.get();
        presenter = std::move(output);
    } else {
#ifdef MUXLESS_DESKTOP_FIXTURE
        presenter = std::make_unique<DesktopPresenter>(desktopRenderNode());
#else
        throw std::runtime_error("A DRM headset lease is required");
#endif
    }

    slots.resize(std::max(3u, ci.minImageCount));
    images.reserve(slots.size());
    waits.reserve(8);
    const auto *residencyPath = std::getenv("V3_RESIDENCY_SOCKET");
    if (residencyPath && *residencyPath)
        residency = std::make_unique<ResidencyObserver>(vk);
    if (const auto *path = std::getenv("V3_COPY_TIMING"); path && *path) {
        copyTrace = std::fopen(path, "a");
        if (!copyTrace)
            throw std::runtime_error("Cannot open the GPU copy trace");
        if (std::fseek(copyTrace, 0, SEEK_END) == 0 && std::ftell(copyTrace) == 0)
            std::fputs("sequence,presented_ns,frame_id,slot,producer_waits,submit_enter_ns,submit_exit_ns,commit_ns,fence_ns,batch_begin_tick,copy_begin_tick,copy_end_tick,batch_end_tick,clock_ticks,clock_ns,clock_deviation_ns,tick_period_ns,timestamp_mask,sdma_engine\n", copyTrace);
        queriesPerSlot = 4;
    }
    if (const auto *path = std::getenv("V3_TRACE"); path && *path) {
        trace = std::fopen(path, "a");
        if (!trace)
            throw std::runtime_error("Cannot open the benchmark trace");
        std::setvbuf(trace, traceBuffer.data(), _IOFBF, traceBuffer.size());
        if (std::fseek(trace, 0, SEEK_END) == 0 && std::ftell(trace) == 0) {
            std::fputs("frame_id,slot,submit_enter_ns,submit_exit_ns,commit_ns,presented_ns,sequence,refresh_ns,flags,discarded,gpu_ns,target_edp,output_gpu_ns,output_sum_ns,output_writes,source_ready_ns\n", trace);
            std::fflush(trace);
        }
    }

    if (trace || copyTrace) {
        VkPhysicalDeviceProperties properties{};
        VK_P(GetPhysicalDeviceProperties)(vk.physical, &properties);
        timestampPeriod = properties.limits.timestampPeriod;
        uint32_t familyCount = 0;
        VK_P(GetPhysicalDeviceQueueFamilyProperties)(vk.physical, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueProperties(familyCount);
        VK_P(GetPhysicalDeviceQueueFamilyProperties)(vk.physical, &familyCount, queueProperties.data());
        const auto bits = queueProperties.at(copyFamily).timestampValidBits;
        if (bits) {
            timestampMask = bits == 64 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
            VkQueryPoolCreateInfo create{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            create.queryType = VK_QUERY_TYPE_TIMESTAMP;
            create.queryCount = slots.size() * queriesPerSlot;
            check(VK_D(CreateQueryPool)(vk.device, &create, nullptr, &queryPool), "Create timing queries");
            queryResults = VK_D(GetQueryPoolResults);
            semaphoreCounter = VK_D(GetSemaphoreCounterValue);
            if (!semaphoreCounter)
                semaphoreCounter = VK_D(GetSemaphoreCounterValueKHR);
        }
        const auto *audit = std::getenv("V3_OUTPUT_AUDIT");
        const auto outputBits = queueProperties.at(renderFamily).timestampValidBits;
        if (audit && !std::strcmp(audit, "1") && outputBits && queryPool) {
            outputTimestampMask = outputBits == 64 ? UINT64_MAX : (uint64_t{1} << outputBits) - 1;
            VkQueryPoolCreateInfo create{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            create.queryType = VK_QUERY_TYPE_TIMESTAMP;
            create.queryCount = slots.size() * maxWriteRanges * 2;
            check(VK_D(CreateQueryPool)(vk.device, &create, nullptr, &outputQueries), "Create output timing queries");
        }
    }
    uint32_t rejectedMemory = 0;
    for (uint32_t index = 0; index < slots.size(); ++index) {
        auto &slot = slots[index];
        uint32_t imported = UINT32_MAX;
#ifdef MUXLESS_DESKTOP_FIXTURE
        VkDeviceSize sharedSize = 0;
#endif
        if (headset)
            imported = importHeadsetBuffer(slot, sharedInfo, drmFormat, rejectedMemory);
#ifdef MUXLESS_DESKTOP_FIXTURE
        else
            sharedSize = createImage(slot.shared, sharedInfo);
#endif
        if (copies())
            createImage(slot.local, localInfo);
        images.push_back(copies() ? slot.local.handle : slot.shared.handle);
        slot.ready = makeSemaphore(VK_SEMAPHORE_TYPE_TIMELINE, forHeadset
            ? VkExternalSemaphoreHandleTypeFlagBits{} : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
        if (forHeadset)
            slot.scanoutReady = makeSemaphore(VK_SEMAPHORE_TYPE_BINARY, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
        else
            slot.released = makeSemaphore(VK_SEMAPHORE_TYPE_TIMELINE, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);

#ifdef MUXLESS_DESKTOP_FIXTURE
        if (!headset) {
            const auto image = exportImage(slot.shared, sharedSize);
            if (residency)
                residency->observe(image.fd.get());
            Fd ready(timelineFd(slot.ready));
            Fd released(timelineFd(slot.released));
            imported = static_cast<DesktopPresenter *>(presenter.get())->add(
                image.fd.get(), extent.width, extent.height, drmFormat, image.pitch,
                image.offset, sharedModifier, ready.get(), released.get());
        }
#endif
        if (imported != index)
            throw std::runtime_error("Frame consumer registered slot " + std::to_string(imported) +
                                     " while importing slot " + std::to_string(index));

        if (acquireQueue) {
            slot.acquireFirst = command(acquirePool);
            encodeAcquire(slot.acquireFirst, slot, true);
            if (!copies()) {
                slot.acquireAgain = command(acquirePool);
                encodeAcquire(slot.acquireAgain, slot, false);
            }
        }
        slot.presentFirst = command(presentPool);
        encodePresent(slot.presentFirst, slot, true, index);
        slot.presentAgain = slot.presentFirst;
        if (copies()) {
            slot.presentAgain = command(presentPool);
            encodePresent(slot.presentAgain, slot, false, index);
        }
    }
    if (residency)
        residency->publish(residencyPath);
}

VkImageCreateInfo FrameRing::Impl::imageInfo(VkImageUsageFlags usage, bool shared) const {
    const bool concurrent = families.size() > 1 && !(shared && copyQueue);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {extent.width, extent.height, 1};
    info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = shared ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = concurrent ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
    info.queueFamilyIndexCount = concurrent ? families.size() : 0;
    info.pQueueFamilyIndices = concurrent ? families.data() : nullptr;
    return info;
}

void FrameRing::Impl::preflightImage(const VkImageCreateInfo &info) const {
    auto imageProperties = VK_P(GetPhysicalDeviceImageFormatProperties2);
    if (!imageProperties)
        imageProperties = VK_P(GetPhysicalDeviceImageFormatProperties2KHR);
    if (!imageProperties)
        throw std::runtime_error("Frame feed cannot query Vulkan image-format capabilities");

    const bool shared = info.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    const auto support = queryImage(vk.physical, imageProperties, info, sharedModifier);
    const auto details = [&] {
        char text[160];
        std::snprintf(text, sizeof(text), "%s (VkFormat %u, usage %#x, extent %ux%u, modifier %#llx)",
                      shared ? "scanout DMA-BUF" : "local render image",
                      unsigned(info.format), unsigned(info.usage), info.extent.width, info.extent.height,
                      static_cast<unsigned long long>(shared ? sharedModifier : 0));
        return std::string(text);
    };
    if (support.result != VK_SUCCESS)
        throw std::runtime_error("Vulkan image-format query failed for " + details() +
                                 " (VkResult " + std::to_string(support.result) + ")");
    if (!fitsImage(support.limits, info))
        throw std::runtime_error("Vulkan image limits cannot accommodate " + details());
    if (shared && !canExportDmaBuf(support.external))
        throw std::runtime_error("Vulkan producer cannot export " + details());
}

VkDeviceSize FrameRing::Impl::createImage(Image &out, VkImageCreateInfo info, uint32_t rejected,
                                          uint32_t *typeUsed) {
    const bool shared = info.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;

    VkImageDrmFormatModifierListCreateInfoEXT modifiers{
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
    modifiers.drmFormatModifierCount = 1;
    modifiers.pDrmFormatModifiers = &sharedModifier;
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.pNext = &modifiers;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    info.pNext = shared ? &external : nullptr;
    check(VK_D(CreateImage)(vk.device, &info, nullptr, &out.handle),
          shared ? "Create scanout DMA-BUF image" : "Create local render image");
    VkMemoryRequirements requirements{};
    VK_D(GetImageMemoryRequirements)(vk.device, out.handle, &requirements);
    uint32_t chosen = UINT32_MAX;
    uint32_t bestRank = UINT32_MAX;
    constexpr auto mappable = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
        if (!(requirements.memoryTypeBits & ~rejected & (1u << type)))
            continue;
        const auto flags = memory.memoryTypes[type].propertyFlags;
        const bool local = flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        const bool visible = flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        const bool cached = flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        uint32_t rank;
        if (shared) {
            const auto heap = memory.memoryTypes[type].heapIndex;
            const bool localHeap = memory.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
            if (headset) {
                // Prefer the existing write-combined system memory placement.
                // PRIME import and the leased plane have final say.
                rank = (local || localHeap ? 4u : 0u) + ((flags & mappable) == mappable ? 0u : 2u) +
                       (cached ? 1u : 0u);
            } else {
                if ((flags & mappable) != mappable || cached ||
                    local != (mode == Transport::peer) || localHeap != local)
                    continue;
                rank = 0;
            }
        } else {
            if (!local || (!headset && visible))
                continue;
            rank = visible ? 1 : 0;
        }
        if (rank < bestRank) {
            bestRank = rank;
            chosen = type;
            if (rank == 0)
                break;
        }
    }
    if (chosen == UINT32_MAX)
        throw std::runtime_error(!shared ? "No device-local render memory"
            : headset ? "No remaining compatible Vulkan memory types for the leased scanout DMA-BUF"
            : mode == Transport::peer ? "No exportable BAR-visible device-local image memory"
                                       : "No uncached coherent system-memory heap for scanout");

    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = out.handle;
    VkExportMemoryAllocateInfo exported{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exported.pNext = &dedicated;
    exported.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.pNext = shared ? &exported : nullptr;
    alloc.allocationSize = requirements.size;
    alloc.memoryTypeIndex = chosen;
    check(VK_D(AllocateMemory)(vk.device, &alloc, nullptr, &out.memory),
          shared ? "Allocate scanout DMA-BUF memory" : "Allocate local render memory");
    check(VK_D(BindImageMemory)(vk.device, out.handle, out.memory, 0),
          shared ? "Bind scanout DMA-BUF memory" : "Bind local render memory");
    if (typeUsed)
        *typeUsed = chosen;
    return requirements.size;
}

void FrameRing::Impl::destroyImage(Image &image) const {
    if (image.handle)
        VK_D(DestroyImage)(vk.device, image.handle, nullptr);
    if (image.memory)
        VK_D(FreeMemory)(vk.device, image.memory, nullptr);
    image = {};
}

ExportedImage FrameRing::Impl::exportImage(const Image &image, VkDeviceSize size) const {
    const VkMemoryGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, nullptr,
        image.memory, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    check(VK_D(GetMemoryFdKHR)(vk.device, &exportInfo, &fd), "Export frame DMA-BUF");
    Fd exported(fd);
    if (exported.get() < 0)
        throw std::runtime_error("Vulkan returned an invalid frame DMA-BUF descriptor");
    const VkImageSubresource plane{VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
    VkSubresourceLayout layout{};
    VK_D(GetImageSubresourceLayout)(vk.device, image.handle, &plane, &layout);
    const auto rowBytes = extent.width * 4ull;
    const bool validLayout = layout.rowPitch >= rowBytes && layout.rowPitch <= UINT32_MAX &&
                             layout.offset <= UINT32_MAX && layout.offset < size;
    if (!validLayout || (sharedModifier == DRM_FORMAT_MOD_LINEAR &&
                         layout.rowPitch * (extent.height - 1) + rowBytes > size - layout.offset))
        throw std::runtime_error("Frame DMA-BUF layout is invalid (pitch " +
                                 std::to_string(layout.rowPitch) + ", offset " +
                                 std::to_string(layout.offset) + ", allocation " +
                                 std::to_string(size) + " bytes)");
    return {Fd(exported.release()), static_cast<uint32_t>(layout.rowPitch),
            static_cast<uint32_t>(layout.offset)};
}

uint32_t FrameRing::Impl::importHeadsetBuffer(Slot &slot, const VkImageCreateInfo &info,
                                              uint32_t fourcc, uint32_t &rejected) {
    for (;;) {
        uint32_t type;
        const auto size = createImage(slot.shared, info, rejected, &type);
        const auto image = exportImage(slot.shared, size);
        uint32_t imported;
        try {
            imported = headset->addBuffer(image.fd.get(), extent.width, extent.height, fourcc,
                                          image.pitch, image.offset, sharedModifier);
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::exception &error) {
            std::fprintf(stderr, "wmr-v3: leased scanout rejected Vulkan memory type %u: %s\n",
                         type, error.what());
            destroyImage(slot.shared);
            rejected |= 1u << type;
            continue;
        }
        if (residency)
            residency->observe(image.fd.get());
        const auto flags = memory.memoryTypes[type].propertyFlags;
        std::fprintf(stderr, "wmr-v3: preflight scanout slot %u uses Vulkan memory type %u (flags %#x)\n",
                     imported, type, flags);
        return imported;
    }
}

VkSemaphore FrameRing::Impl::makeSemaphore(VkSemaphoreType kind,
                                           VkExternalSemaphoreHandleTypeFlagBits handle) const {
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = kind;
    VkExportSemaphoreCreateInfo exported{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    exported.handleTypes = handle;
    type.pNext = handle ? &exported : nullptr;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    info.pNext = &type;
    VkSemaphore sem{};
    check(VK_D(CreateSemaphore)(vk.device, &info, nullptr, &sem),
          kind == VK_SEMAPHORE_TYPE_TIMELINE ? "Create frame timeline" : "Create scanout SYNC_FD semaphore");
    return sem;
}

int FrameRing::Impl::timelineFd(VkSemaphore sem) const {
    VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    info.semaphore = sem;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    check(semaphoreFd(vk.device, &info, &fd), "Export timeline FD");
    if (fd < 0)
        throw std::runtime_error("Vulkan returned an invalid frame timeline descriptor");
    return fd;
}

VkResult FrameRing::Impl::signalAcquire(VkSemaphore semaphore, VkFence fence) const {
    // KMS already displaced this slot. Import a signaled payload without
    // submitting to an application queue from an acquire call.
    if (fence) {
        const VkImportFenceFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, nullptr,
            fence, VK_FENCE_IMPORT_TEMPORARY_BIT, VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, -1};
        const auto result = importFenceFd(vk.device, &info);
        if (result != VK_SUCCESS)
            return result;
    }
    if (semaphore) {
        const VkImportSemaphoreFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR, nullptr,
            semaphore, VK_SEMAPHORE_IMPORT_TEMPORARY_BIT, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, -1};
        const auto result = importSemaphoreFd(vk.device, &info);
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST && fence) {
            const auto reset = resetFences(vk.device, 1, &fence);
            if (reset != VK_SUCCESS)
                return reset;
        }
        return result;
    }
    return VK_SUCCESS;
}

VkCommandPool FrameRing::Impl::pool(uint32_t family) const {
    VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.queueFamilyIndex = family;
    VkCommandPool result{};
    check(VK_D(CreateCommandPool)(vk.device, &info, nullptr, &result), "Create frame command pool");
    return result;
}

VkCommandBuffer FrameRing::Impl::command(VkCommandPool commandPool) const {
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = commandPool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    VkCommandBuffer result{};
    check(VK_D(AllocateCommandBuffers)(vk.device, &info, &result), "Allocate frame commands");
    check(setLoaderData(vk.device, result), "Initialize command dispatch");
    return result;
}

void FrameRing::Impl::begin(VkCommandBuffer cb) const {
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    // Re-submission may queue before the previous instance retires on the GPU.
    info.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    check(VK_D(BeginCommandBuffer)(cb, &info), "Begin frame commands");
}

void FrameRing::Impl::barrier(VkCommandBuffer cb, const VkImageMemoryBarrier2 &image) const {
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers = &image;
    pipelineBarrier(cb, &info);
}

void FrameRing::Impl::encodeAcquire(VkCommandBuffer cb, const Slot &slot, bool first) const {
    begin(cb);
    const auto image = copies() ? slot.local.handle : slot.shared.handle;
    if (first) {
        barrier(cb, imageBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 0, 0, all, readWrite));
    } else {
        barrier(cb, imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                 0, 0, all, readWrite, VK_QUEUE_FAMILY_FOREIGN_EXT, acquireFamily));
    }
    check(VK_D(EndCommandBuffer)(cb), "Record acquisition");
}

void FrameRing::Impl::encodePresent(VkCommandBuffer cb, const Slot &slot, bool first,
                                     uint32_t index) const {
    begin(cb);
    const auto query = index * queriesPerSlot;
    if (queryPool) {
        VK_D(CmdResetQueryPool)(cb, queryPool, query, queriesPerSlot);
        VK_D(CmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queryPool, query);
    }
    const bool copy = copies();
    if (copy) {
        const auto sourceLayout = sourceBarrier ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        if (sourceBarrier)
            barrier(cb, imageBarrier(slot.local.handle, VK_IMAGE_LAYOUT_GENERAL, sourceLayout,
                                     all, write, transfer, VK_ACCESS_2_TRANSFER_READ_BIT));
        barrier(cb, imageBarrier(slot.shared.handle,
                                 first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                                 transfer, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                 first ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT,
                                 first ? VK_QUEUE_FAMILY_IGNORED : copyFamily));
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {extent.width, extent.height, 1};
        if (queryPool && copyTrace)
            VK_D(CmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, queryPool, query + 1);
        VK_D(CmdCopyImage)(cb, slot.local.handle, sourceLayout,
                           slot.shared.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        if (queryPool && copyTrace)
            VK_D(CmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, queryPool, query + 2);
        if (sourceBarrier)
            barrier(cb, imageBarrier(slot.local.handle, sourceLayout,
                                     VK_IMAGE_LAYOUT_GENERAL, transfer, VK_ACCESS_2_TRANSFER_READ_BIT,
                                     all, readWrite));
    }
    barrier(cb, imageBarrier(slot.shared.handle,
                             copy ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL,
                             VK_IMAGE_LAYOUT_GENERAL, copy ? transfer : all,
                             copy ? VK_ACCESS_2_TRANSFER_WRITE_BIT : write, 0, 0,
                             copyFamily, VK_QUEUE_FAMILY_FOREIGN_EXT));
    if (queryPool) {
        if (copyTrace && !copy) {
            VK_D(CmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, query + 1);
            VK_D(CmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, query + 2);
        }
        VK_D(CmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queryPool, query + queriesPerSlot - 1);
    }
    check(VK_D(EndCommandBuffer)(cb), "Record frame release");
}

VkResult FrameRing::Impl::submit(VkQueue queue, VkCommandBuffer cb, uint32_t waitCount,
                                 const VkSemaphoreSubmitInfo *waits,
                                 uint32_t signalCount, const VkSemaphoreSubmitInfo *signals, VkFence fence) const {
    VkCommandBufferSubmitInfo cmd{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmd.commandBuffer = cb;
    VkSubmitInfo2 batches[2]{{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}, {VK_STRUCTURE_TYPE_SUBMIT_INFO_2}};
    auto &info = batches[0];
    info.waitSemaphoreInfoCount = waitCount;
    info.pWaitSemaphoreInfos = waits;
    info.commandBufferInfoCount = cb ? 1 : 0;
    info.pCommandBufferInfos = cb ? &cmd : nullptr;
    info.signalSemaphoreInfoCount = signalCount ? 1 : 0;
    info.pSignalSemaphoreInfos = signals;
    if (signalCount == 2) {
        // Retirement must follow the binary signal. Signals in one batch are unordered.
        batches[1].signalSemaphoreInfoCount = 1;
        batches[1].pSignalSemaphoreInfos = signals + 1;
    }
    const auto batchCount = signalCount == 2 ? 2u : 1u;
    // A device's private queues can still be used by its previous swapchain.
    if (queue == acquireQueue || queue == copyQueue) {
        const std::lock_guard lock(*privateQueueMutex);
        return queueSubmit(queue, batchCount, batches, fence);
    }
    return queueSubmit(queue, batchCount, batches, fence);
}

VkResult FrameRing::Impl::fail(VkResult result) {
    if (fault == VK_SUCCESS) {
        fault = result;
        if (headset)
            headset->wakeFeedbackWaiters();
        else
            desktopPresentQueued.notify_all();
    }
    return fault;
}

bool FrameRing::Impl::drain() const {
    std::array<VkSemaphore, maxSlots> timelines{};
    std::array<uint64_t, maxSlots> points{};
    uint32_t count = 0;
    for (const auto &slot : slots) {
        const auto point = std::max(slot.point, slot.relayPoint);
        if (!point)
            continue;
        timelines[count] = slot.ready;
        points[count++] = point;
    }
    if (count) {
        VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        info.semaphoreCount = count;
        info.pSemaphores = timelines.data();
        info.pValues = points.data();
        const auto result = waitSemaphores(vk.device, &info, UINT64_MAX);
        if (result == VK_ERROR_DEVICE_LOST)
            return true;
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "wmr-v3: wait for submitted frames failed (%d)\n", result);
            return false;
        }
    }
    if (!acquireQueue)
        return true;
    // An acquired image may have no present point. Drain the private queue for
    // its acquisition command buffer without synchronizing application queues.
    const std::lock_guard lock(*privateQueueMutex);
    const auto result = VK_D(QueueWaitIdle)(acquireQueue);
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST)
        return true;
    std::fprintf(stderr, "wmr-v3: drain acquisition queue failed (%d)\n", result);
    return false;
}

void FrameRing::Impl::stopOutput() {
    if (presenter) {
        try {
            presenter->dispatch();
            feedback();
        } catch (const std::exception &) {
            // The device and all local resources still need to be destroyed.
        }
        // Reuse waits are submitted only after the compositor attaches a fence.
        presenter.reset();
    }
    if (trace)
        std::fclose(std::exchange(trace, nullptr));
    if (copyTrace)
        std::fclose(std::exchange(copyTrace, nullptr));
    residency.reset();
}

FrameRing::Impl::~Impl() {
    stopOutput();
    if (!releaseGpuResources)
        return;
    if (acquirePool)
        VK_D(DestroyCommandPool)(vk.device, acquirePool, nullptr);
    if (presentPool)
        VK_D(DestroyCommandPool)(vk.device, presentPool, nullptr);
    if (queryPool)
        VK_D(DestroyQueryPool)(vk.device, queryPool, nullptr);
    if (outputQueries)
        VK_D(DestroyQueryPool)(vk.device, outputQueries, nullptr);
    for (auto &slot : slots) {
        destroyImage(slot.local);
        destroyImage(slot.shared);
        if (slot.ready)
            VK_D(DestroySemaphore)(vk.device, slot.ready, nullptr);
        if (slot.released)
            VK_D(DestroySemaphore)(vk.device, slot.released, nullptr);
        if (slot.scanoutReady)
            VK_D(DestroySemaphore)(vk.device, slot.scanoutReady, nullptr);
    }
}

void FrameRing::Impl::finishSample(const Slot &slot, Sample &sample) const {
    if (!queryPool || !semaphoreCounter || sample.frame != slot.frame ||
        (sample.gpu && (!sample.writeRanges || sample.outputSpan)))
        return;
    uint64_t point = 0;
    if (semaphoreCounter(vk.device, slot.ready, &point) != VK_SUCCESS || point < slot.point)
        return;
    if (!sample.gpu &&
        queryResults(vk.device, queryPool, sample.slot * queriesPerSlot, queriesPerSlot,
                     sizeof(sample.copyTicks), sample.copyTicks.data(), sizeof(uint64_t),
                     VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        sample.gpu = uint64_t(double((sample.copyTicks[queriesPerSlot - 1] -
                                     sample.copyTicks[0]) & timestampMask) * timestampPeriod);
    if (outputQueries && sample.writeRanges && !sample.outputSpan) {
        std::array<uint64_t, maxWriteRanges * 2> values{};
        const auto count = sample.writeRanges * 2;
        const auto start = sample.slot * maxWriteRanges * 2;
        if (queryResults(vk.device, outputQueries, start, count, count * sizeof(uint64_t),
                         values.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            const auto elapsed = [&](uint64_t startTicks, uint64_t endTicks) {
                return uint64_t(double((endTicks - startTicks) & outputTimestampMask) * timestampPeriod);
            };
            sample.outputSpan = elapsed(values[0], values[count - 1]);
            for (uint32_t i = 0; i < count; i += 2)
                sample.outputSum += elapsed(values[i], values[i + 1]);
        }
    }
}

void FrameRing::Impl::writeCopySample(const Sample &sample, const DesktopFeedback &event) {
    if (vk.calibrate && event.presentedNs > lastCalibration + 1'000'000'000) {
        const VkCalibratedTimestampInfoKHR domains[] = {
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_DEVICE_KHR},
            {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR, nullptr, VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR},
        };
        if (vk.calibrate(vk.device, 2, domains, calibration.data(), &clockDeviation) != VK_SUCCESS)
            calibration = {};
        lastCalibration = event.presentedNs;
    }
    using U = unsigned long long;
    std::fprintf(copyTrace, "%llu,%llu,%llu,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%.9g,%llu,%d\n",
                 U(event.sequence), U(event.presentedNs), U(sample.frame), sample.slot, sample.producerWaits,
                 U(sample.before), U(sample.after), U(event.commitNs), U(event.sourceReadyNs),
                 U(sample.copyTicks[0]), U(sample.copyTicks[1]), U(sample.copyTicks[2]), U(sample.copyTicks[3]),
                 U(calibration[0]), U(calibration[1]), U(clockDeviation), double(timestampPeriod), U(timestampMask),
                 event.dmaEngine);
}

void FrameRing::Impl::feedback() {
    for (const auto &event : presenter->takeFeedback()) {
        auto &sample = samples[event.frameId % samples.size()];
        if (sample.frame != event.frameId)
            continue;
        completedLegacyId = std::max(completedLegacyId, sample.legacyId);
        completedId = std::max(completedId, sample.id);
        if (headset)
            traceWsi("feedback", event.frameId, sample.legacyId, sample.id, event.sequence);
        if (!trace && !copyTrace)
            continue;
        finishSample(slots[sample.slot], sample);
        if (copyTrace)
            writeCopySample(sample, event);
        if (trace)
            std::fprintf(trace, "%llu,%u,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%llu,%u,%llu,%llu,%u,%llu\n",
                     static_cast<unsigned long long>(sample.frame), sample.slot,
                     static_cast<unsigned long long>(sample.before),
                     static_cast<unsigned long long>(sample.after),
                     static_cast<unsigned long long>(event.commitNs),
                     static_cast<unsigned long long>(event.presentedNs),
                     static_cast<unsigned long long>(event.sequence), event.refreshNs, event.flags,
                     unsigned(event.discarded), static_cast<unsigned long long>(sample.gpu),
                     unsigned(event.targetOutput), static_cast<unsigned long long>(sample.outputSpan),
                     static_cast<unsigned long long>(sample.outputSum), sample.writeRanges,
                     static_cast<unsigned long long>(event.sourceReadyNs));
        if (++pendingTrace == 90) {
            if (trace)
                std::fflush(trace);
            if (copyTrace)
                std::fflush(copyTrace);
            pendingTrace = 0;
        }
    }
}

FrameRing::FrameRing(Vulkan vk, const VkSwapchainCreateInfoKHR &ci, Transport mode,
                     VkQueue q, uint32_t acquireFamily, uint32_t renderFamily,
                     PFN_vkSetDeviceLoaderData loader, VkQueue copyQueue, uint32_t copyFamily,
                     const HeadsetLease &lease, std::shared_ptr<std::mutex> privateQueueMutex,
                     const std::vector<uint32_t> &appFamilies)
    : impl(std::make_unique<Impl>(vk, std::move(privateQueueMutex))) {
    impl->initialize(ci, mode, q, acquireFamily, renderFamily, loader, copyQueue, copyFamily, lease, appFamilies);
}

FrameRing::~FrameRing() {
    impl->stopOutput();
    if (!impl->submitted || impl->drain())
        return;
    // Keep the pending resources without allocating in the failure path.
    const std::lock_guard lock(Impl::deferredMutex);
    impl->nextDeferred = Impl::deferred;
    Impl::deferred = impl.release();
}

void FrameRing::drainDeferred(VkDevice device) {
    Impl *pending = nullptr;
    {
        const std::lock_guard lock(Impl::deferredMutex);
        for (auto **link = &Impl::deferred; *link;) {
            auto *ring = *link;
            if (ring->vk.device != device) {
                link = &ring->nextDeferred;
                continue;
            }
            *link = ring->nextDeferred;
            ring->nextDeferred = pending;
            pending = ring;
        }
    }
    if (!pending)
        return;
    // vkDestroyDevice synchronizes host access to every queue of this device.
    const auto result = pending->vk.deviceFn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device);
    const bool idle = result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
    if (!idle)
        std::fprintf(stderr, "wmr-v3: device idle failed during deferred cleanup (%d)\n", result);
    while (pending) {
        std::unique_ptr<Impl> ring(pending);
        pending = ring->nextDeferred;
        if (!idle && !ring->drain()) {
            // Completion is still unknown. Leave the GPU handles to device teardown.
            ring->releaseGpuResources = false;
            std::fprintf(stderr, "wmr-v3: pending swapchain resources left to device teardown\n");
        }
    }
}

void FrameRing::retireHeadset(bool keepScanout) {
    impl->headset->retire(keepScanout);
}

const std::vector<VkImage> &FrameRing::images() const {
    return impl->images;
}

uint64_t FrameRing::modifier() const { return impl->sharedModifier; }

VkResult FrameRing::displayCounter(uint64_t *value) const {
    return impl->headset ? impl->headset->counter(value) : VK_ERROR_FEATURE_NOT_PRESENT;
}

VkResult FrameRing::queueVblank(std::shared_ptr<DisplayEvent> event) {
    return impl->headset ? impl->headset->queueVblank(std::move(event)) : VK_ERROR_FEATURE_NOT_PRESENT;
}

void FrameRing::auditWrite(VkCommandBuffer command, VkImage image, bool begin) {
    auto &s = *impl;
    if (!s.outputQueries)
        return;
    const std::lock_guard lock(s.state);
    const auto match = std::find(s.images.begin(), s.images.end(), image);
    if (match == s.images.end())
        return;
    const size_t index = match - s.images.begin();
    auto &slot = s.slots[index];
    if (!slot.acquired || slot.writeRanges >= maxWriteRanges || slot.writing == begin)
        return;
    const auto query = (index * maxWriteRanges + slot.writeRanges) * 2;
    const auto timestamp = s.vk.deviceFn<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
    if (begin) {
        s.vk.deviceFn<PFN_vkCmdResetQueryPool>("vkCmdResetQueryPool")(command, s.outputQueries, query, 2);
        timestamp(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.outputQueries, query);
    } else {
        timestamp(command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.outputQueries, query + 1);
        ++slot.writeRanges;
    }
    slot.writing = begin;
}

VkResult FrameRing::acquire(uint64_t timeout, VkSemaphore semaphore, VkFence fence,
                            uint32_t *index) {
    if (!index || (!semaphore && !fence))
        return VK_ERROR_INITIALIZATION_FAILED;
    auto &s = *impl;
    if (s.headset)
        traceWsi("acquire_begin", 0, timeout);
    const std::lock_guard lock(s.state);
    if (s.headset)
        traceWsi("acquire_locked", 0);
    if (s.fault != VK_SUCCESS)
        return s.fault;
    s.presenter->dispatch();
    s.feedback();
    if (s.presenter->closeRequested())
        return VK_ERROR_OUT_OF_DATE_KHR;
    std::array<uint32_t, maxSlots> candidates{};
    std::array<uint64_t, maxSlots> points{};
    uint32_t count = 0;
    for (size_t i = 0; i < s.slots.size(); ++i) {
        const uint32_t next = (s.cursor + i) % s.slots.size();
        const auto &slot = s.slots[next];
        if (slot.acquired)
            continue;
        candidates[count] = next;
        points[count++] = slot.point;
    }
    const int available = count ? s.presenter->releasable(candidates.data(), points.data(), count, timeout) : -1;
    if (available < 0)
        return s.presenter->closeRequested() ? VK_ERROR_OUT_OF_DATE_KHR : timeout ? VK_TIMEOUT : VK_NOT_READY;
    const uint32_t next = candidates[available];
    auto &slot = s.slots[next];
    if (s.trace || s.copyTrace)
        s.finishSample(slot, s.samples[slot.frame % s.samples.size()]);
    VkResult result;
    if (s.headset) {
        result = s.signalAcquire(semaphore, fence);
    } else {
        const std::array waits{semaphoreInfo(slot.ready, slot.point),
                               semaphoreInfo(slot.released, slot.point)};
        const auto signal = semaphoreInfo(semaphore, 0);
        const auto cb = slot.point ? slot.acquireAgain : slot.acquireFirst;
        result = s.submit(s.acquireQueue, cb, slot.point ? waits.size() : 0, waits.data(),
                          semaphore ? 1 : 0, semaphore ? &signal : nullptr, fence);
    }
    if (result != VK_SUCCESS)
        return result == VK_ERROR_DEVICE_LOST ? s.fail(result) : result;
    s.submitted = true;
    *index = next;
    s.cursor = (next + 1) % s.slots.size();
    slot.acquired = true;
    slot.writeRanges = 0;
    slot.writing = false;
    if (s.headset)
        traceWsi("acquire_end", next, slot.point);
    return VK_SUCCESS;
}

VkResult FrameRing::present(VkQueue queue, const VkPresentInfoKHR &info) {
    if (info.swapchainCount != 1 || !info.pImageIndices ||
        (info.waitSemaphoreCount && !info.pWaitSemaphores))
        return VK_ERROR_INITIALIZATION_FAILED;
    auto &s = *impl;
    if (s.headset)
        traceWsi("present_begin", info.pImageIndices[0]);
    const std::lock_guard lock(s.state);
    if (s.fault != VK_SUCCESS)
        return s.fault;
    const uint32_t index = info.pImageIndices[0];
    if (index >= s.slots.size() || !s.slots[index].acquired)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto &slot = s.slots[index];
    uint64_t legacyId = 0, id = 0;
    for (auto *p = static_cast<const VkBaseInStructure *>(info.pNext); p; p = p->pNext) {
        if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_KHR) {
            const auto &ids = *reinterpret_cast<const VkPresentIdKHR *>(p);
            if (ids.swapchainCount != info.swapchainCount)
                return VK_ERROR_INITIALIZATION_FAILED;
            legacyId = ids.pPresentIds ? ids.pPresentIds[0] : 0;
        } else if (p->sType == VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR) {
            const auto &ids = *reinterpret_cast<const VkPresentId2KHR *>(p);
            if (ids.swapchainCount != info.swapchainCount)
                return VK_ERROR_INITIALIZATION_FAILED;
            id = ids.pPresentIds ? ids.pPresentIds[0] : 0;
        } else {
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    s.waits.clear();
    for (uint32_t i = 0; i < info.waitSemaphoreCount; ++i)
        s.waits.push_back(semaphoreInfo(info.pWaitSemaphores[i], 0));
    if (s.presenter->closeRequested()) {
        if (!s.waits.empty()) {
            const auto result = s.submit(queue, VK_NULL_HANDLE, s.waits.size(), s.waits.data(),
                                         0, nullptr, VK_NULL_HANDLE);
            if (result != VK_SUCCESS)
                return result == VK_ERROR_DEVICE_LOST ? s.fail(result) : result;
        }
        slot.acquired = false;
        return VK_ERROR_OUT_OF_DATE_KHR;
    }
    const bool tracing = s.trace || s.copyTrace;
    const uint64_t before = tracing ? monotonic() : 0;
    const auto copyQueue = s.copyQueue ? s.copyQueue : queue;
    auto nextPoint = slot.point + 1;
    if (copyQueue != queue && s.waits.empty()) {
        // A wait-less present still follows the renderer's queue. Relay that
        // queue order before the copy queue can read the local render image.
        const auto rendered = semaphoreInfo(slot.ready, nextPoint++);
        const auto retired = semaphoreInfo(slot.ready, slot.point);
        auto result = s.submit(queue, VK_NULL_HANDLE, slot.point ? 1 : 0, slot.point ? &retired : nullptr,
                               1, &rendered, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
            return result == VK_ERROR_DEVICE_LOST ? s.fail(result) : result;
        slot.relayPoint = rendered.value;
        s.waits.push_back(rendered);
    }
    const auto retired = semaphoreInfo(slot.ready, nextPoint);
    const std::array signals{semaphoreInfo(slot.scanoutReady, 0), retired};
    auto result = s.submit(copyQueue,
                           slot.point ? slot.presentAgain : slot.presentFirst,
                           s.waits.size(), s.waits.data(), s.headset ? signals.size() : 1,
                           s.headset ? signals.data() : &retired, VK_NULL_HANDLE);
    if (result != VK_SUCCESS)
        return s.fail(result);
    slot.point = nextPoint;
    slot.acquired = false;
    slot.frame = ++s.frame;
    auto &sample = s.samples[s.frame % s.samples.size()];
    sample = {s.frame, before, tracing ? monotonic() : 0, 0, legacyId, id, index, slot.writeRanges};
    if (s.copyTrace)
        sample.producerWaits = info.waitSemaphoreCount;
    if (s.headset)
        traceWsi("present_queued", s.frame, legacyId, id, index);
    try {
        if (s.headset) {
            const VkSemaphoreGetFdInfoKHR data{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR, nullptr,
                slot.scanoutReady, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
            int fd = -1;
            const auto exported = s.semaphoreFd(s.vk.device, &data, &fd);
            if (exported != VK_SUCCESS || fd < -1) {
                std::fprintf(stderr, "wmr-v3: export headset SYNC_FD failed (%d, fd=%d)\n", exported, fd);
                return s.fail(exported != VK_SUCCESS ? exported : VK_ERROR_INVALID_EXTERNAL_HANDLE);
            }
            const Fd syncFile(fd);
            s.headset->presentSyncFile(index, slot.point, s.frame, syncFile.get());
        }
#ifdef MUXLESS_DESKTOP_FIXTURE
        else {
            static_cast<DesktopPresenter *>(s.presenter.get())->presentPoint(index, slot.point, s.frame);
        }
#endif
    } catch (const std::exception &error) {
        std::fprintf(stderr, "wmr-v3: QueuePresent: %s\n", error.what());
        return s.fail(s.presenter->closeRequested() ? VK_ERROR_OUT_OF_DATE_KHR : VK_ERROR_SURFACE_LOST_KHR);
    }
    if (!s.headset && legacyId > s.queuedDesktopId) {
        s.queuedDesktopId = legacyId;
        s.desktopPresentQueued.notify_all();
    }
    return VK_SUCCESS;
}

VkResult FrameRing::waitPresent(uint64_t id, uint64_t timeout, bool newer) {
    auto &s = *impl;
    const auto start = monotonic();
    if (s.headset)
        traceWsi("wait_begin", id, timeout, newer);
    std::unique_lock lock(s.state);
    if (s.headset)
        traceWsi("wait_locked", id, timeout, newer);
    const auto finish = [&](VkResult result) {
        if (s.headset)
            traceWsi("wait_end", id, newer ? s.completedId : s.completedLegacyId, newer, 0, result);
        return result;
    };
    const auto deadline = start + std::min(timeout, UINT64_MAX - start);
    for (;;) {
        if (s.fault != VK_SUCCESS)
            return finish(s.fault);
        const auto generation = s.headset ? s.headset->feedbackGeneration() : 0;
        s.feedback();
        if ((newer ? s.completedId : s.completedLegacyId) >= id)
            return finish(VK_SUCCESS);
        if (s.presenter->closeRequested())
            return finish(VK_ERROR_OUT_OF_DATE_KHR);
        const auto now = monotonic();
        if (now >= deadline)
            return finish(VK_TIMEOUT);
        const auto remaining = timeout == UINT64_MAX ? timeout : deadline - now;
        if (s.headset) {
            // Another caller can consume feedback while this waiter releases
            // the ring lock. Wait for a generation change, independent of that queue.
            lock.unlock();
            s.headset->waitForFeedback(generation, remaining);
            lock.lock();
        } else if (!newer && id > s.queuedDesktopId) {
            // A legacy wait may precede the present call. Release the ring lock
            // so that call can queue it, and periodically dispatch window closure.
            s.desktopPresentQueued.wait_for(lock,
                std::chrono::nanoseconds(std::min(remaining, uint64_t{100'000'000})));
        } else {
            s.presenter->dispatch(true, remaining);
        }
    }
}

#undef VK_D
#undef VK_P

} // namespace v3
