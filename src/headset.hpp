#pragma once

#include "desktop.hpp"
#include <vulkan/vulkan.h>

struct _drmModeModeInfo;

namespace v3 {

class DisplayEvent;

uint32_t headsetRefresh(const _drmModeModeInfo &mode);
bool sameHeadsetTiming(const _drmModeModeInfo &, const _drmModeModeInfo &);
std::vector<uint32_t> headsetLinearFormats(int leaseFd, uint32_t connectorId);

class HeadsetPresenter final : public FramePresenter {
public:
    HeadsetPresenter(int leaseFd, uint32_t connectorId, const _drmModeModeInfo &timing);
    ~HeadsetPresenter() override;
    void retire(bool keepScanout);
    bool supportsBuffer(uint32_t fourcc, uint64_t modifier) const;

    // Both file descriptors remain caller-owned. The kernel takes its own
    // references to a framebuffer and input fence when it accepts the ioctl.
    uint32_t addBuffer(int dmaBufFd, uint32_t width, uint32_t height, uint32_t fourcc,
                       uint32_t pitch, uint32_t offset, uint64_t modifier);
    void presentSyncFile(uint32_t slot, uint64_t point, uint64_t frameId, int syncFileFd);
    int releasable(const uint32_t *slots, const uint64_t *points, uint32_t count,
                   uint64_t timeoutNs) override;
    void dispatch(bool wait = false, uint64_t timeoutNs = UINT64_MAX) override;
    std::vector<DesktopFeedback> takeFeedback() override;
    bool closeRequested() const override;

    VkResult queueVblank(std::shared_ptr<DisplayEvent> event);
    VkResult counter(uint64_t *value) const;
    uint64_t feedbackGeneration() const;
    void waitForFeedback(uint64_t generation, uint64_t timeoutNs);
    void wakeFeedbackWaiters();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
