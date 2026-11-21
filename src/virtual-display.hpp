#pragma once

#include <memory>
#include <sys/types.h>
#include <vulkan/vulkan.h>
#include <xf86drmMode.h>

namespace v3 {

PFN_vkVoidFunction virtualDisplayHook(const char *name);
bool virtualDisplay(VkDisplayKHR display);
bool virtualSurface(VkSurfaceKHR surface);
VkDisplayKHR headsetDisplayForSurface(VkSurfaceKHR surface);
bool virtualRefreshClock();
#ifdef MUXLESS_DESKTOP_FIXTURE
bool headsetOutput();
#else
constexpr bool headsetOutput() { return true; }
#endif

struct HeadsetLease {
    int fd;
    uint32_t connector;
    VkDisplayModeParametersKHR mode;
    drmModeModeInfo timing;
    dev_t device;
    HeadsetLease(int fd, uint32_t connector, VkDisplayModeParametersKHR mode = {},
                 drmModeModeInfo timing = {}, dev_t device = {})
        : fd(fd), connector(connector), mode(mode), timing(timing), device(device) {}
    ~HeadsetLease();
    HeadsetLease(const HeadsetLease &) = delete;
    HeadsetLease &operator=(const HeadsetLease &) = delete;
};

HeadsetLease copyHeadsetLease(VkPhysicalDevice physical, VkSurfaceKHR surface);
void releaseVirtualDisplays(VkInstance instance);

class DesktopVblank {
public:
    explicit DesktopVblank(const char *renderNode);
    ~DesktopVblank();
    VkResult queue(int fenceFd);
    VkResult counter(uint64_t *value) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
