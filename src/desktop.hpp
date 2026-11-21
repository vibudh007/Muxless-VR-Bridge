#pragma once

#include <cstdint>
#include <memory>
#include <vector>

const char *desktopRenderNode();

struct DesktopFeedback {
    uint64_t frameId{};
    uint64_t commitNs{};
    uint64_t presentedNs{};
    uint64_t sequence{};
    uint32_t refreshNs{};
    uint32_t flags{};
    bool discarded{};
    bool targetOutput{};
    uint64_t sourceReadyNs{};
    int dmaEngine{-1};
};

class FramePresenter {
public:
    virtual ~FramePresenter() = default;
    virtual int releasable(const uint32_t *slots, const uint64_t *points, uint32_t count,
                           uint64_t timeoutNs) = 0;
    virtual void dispatch(bool wait = false, uint64_t timeoutNs = UINT64_MAX) = 0;
    virtual std::vector<DesktopFeedback> takeFeedback() = 0;
    virtual bool closeRequested() const = 0;
};

class DesktopPresenter final : public FramePresenter {
public:
    explicit DesktopPresenter(const char *drmNode, const char *outputName = "eDP-1");
    ~DesktopPresenter() override;

    // File descriptors remain caller-owned. Each slot has two distinct DRM syncobj timelines.
    uint32_t add(int dmaBufFd, uint32_t width, uint32_t height, uint32_t fourcc,
                 uint32_t pitch, uint32_t offset, uint64_t modifier,
                 int acquireTimelineFd, int releaseTimelineFd);

    // The producer signals acquire at point and waits the previous release before overwriting.
    void presentPoint(uint32_t slot, uint64_t point, uint64_t frameId);

    // Returns an index into the slots array once the matching consumer fence exists.
    int releasable(const uint32_t *slots, const uint64_t *points, uint32_t count,
                   uint64_t timeoutNs) override;
    void dispatch(bool wait = false, uint64_t timeoutNs = UINT64_MAX) override;
    std::vector<DesktopFeedback> takeFeedback() override;
    bool closeRequested() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
