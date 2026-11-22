#pragma once

#include <memory>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

namespace v3 {

enum class Transport { direct, copy, peer, ace };

struct Vulkan {
    VkPhysicalDevice physical;
    VkDevice device;
    PFN_vkGetInstanceProcAddr instanceProc;
    VkInstance instance;
    PFN_vkGetDeviceProcAddr deviceProc;
    PFN_vkGetCalibratedTimestampsKHR calibrate{};

    template<class T> auto deviceFn(const char *name) const {
        return reinterpret_cast<T>(deviceProc(device, name));
    }
    template<class T> auto physicalFn(const char *name) const {
        return reinterpret_cast<T>(instanceProc(instance, name));
    }
};

// The copy paths render to a second image in device-local memory.
struct HeadsetLease;
class DisplayEvent;

bool supportsLinearFrameFeed(VkPhysicalDevice, PFN_vkGetPhysicalDeviceImageFormatProperties2,
                             VkFormat, VkExtent2D, VkImageUsageFlags renderUsage);

class FrameRing {
public:
    FrameRing(Vulkan, const VkSwapchainCreateInfoKHR &, Transport, VkQueue acquireQueue,
              uint32_t acquireFamily, uint32_t renderFamily, PFN_vkSetDeviceLoaderData,
              VkQueue copyQueue, uint32_t copyFamily, const HeadsetLease &,
              std::shared_ptr<std::mutex> privateQueueMutex,
              const std::vector<uint32_t> &appFamilies = {});
    ~FrameRing();
    static void drainDeferred(VkDevice);
    void retireHeadset(bool keepScanout);
    const std::vector<VkImage> &images() const;
    uint64_t modifier() const;
    VkResult acquire(uint64_t timeout, VkSemaphore, VkFence, uint32_t *index);
    VkResult present(VkQueue, const VkPresentInfoKHR &);
    VkResult waitPresent(uint64_t id, uint64_t timeout, bool newer);
    VkResult displayCounter(uint64_t *value) const;
    VkResult queueVblank(std::shared_ptr<DisplayEvent>);
    void auditWrite(VkCommandBuffer, VkImage, bool begin);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
