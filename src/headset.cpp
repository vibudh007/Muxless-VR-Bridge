// SPDX-License-Identifier: GPL-3.0-or-later
#include "headset.hpp"
#include "display-event.hpp"
#include "wsi-trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/sync_file.h>
#include <mutex>
#include <new>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace v3 {
namespace {

struct Fd {
    int value{-1};
    ~Fd() { if (value >= 0) close(value); }
};

uint64_t eventCookie() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

int drmFailureCode(int status) {
    const int error = status < -1 ? -status : errno;
    return error ? error : EIO;
}

[[noreturn]] void drmError(const char *operation, int status = -1) {
    throw std::system_error(drmFailureCode(status), std::generic_category(), operation);
}

void check(int status, const char *operation) {
    if (status)
        drmError(operation, status);
}

uint64_t monotonicNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint64_t(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

double refresh(const drmModeModeInfo &mode) {
    return double(mode.clock) * 1000 / mode.htotal / mode.vtotal / std::max(1u, unsigned(mode.vscan));
}

uint64_t readyTime(int fd) {
    if (fd < 0)
        return 0;
    sync_file_info info{};
    std::array<sync_fence_info, 16> fences{};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) || info.status != 1 ||
        !info.num_fences || info.num_fences > fences.size())
        return 0;
    info.sync_fence_info = reinterpret_cast<uintptr_t>(fences.data());
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) || info.status != 1 || info.num_fences > fences.size())
        return 0;
    uint64_t ready{};
    for (uint32_t i = 0; i < info.num_fences; ++i)
        ready = std::max(ready, uint64_t(fences[i].timestamp_ns));
    return ready;
}

int pendingDmaEngine(int fd) {
    std::array<sync_fence_info, 16> fences{};
    sync_file_info info{};
    info.num_fences = fences.size();
    info.sync_fence_info = reinterpret_cast<uintptr_t>(fences.data());
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) || info.num_fences > fences.size())
        return -1;
    int engine = -1;
    for (uint32_t i = 0; i < info.num_fences; ++i) {
        const auto &fence = fences[i];
        const auto &name = fence.obj_name;
        const auto end = std::find(name, name + sizeof(name), '\0');
        constexpr char prefix[] = "sdma";
        constexpr auto prefixSize = sizeof(prefix) - 1;
        if (fence.status != 0 || end <= name + prefixSize || std::memcmp(name, prefix, prefixSize))
            continue;
        int index = -1;
        const auto parsed = std::from_chars(name + prefixSize, end, index);
        if (parsed.ec != std::errc{} || parsed.ptr != end || index < 0)
            continue;
        if (engine >= 0 && engine != index)
            return -1;
        engine = index;
    }
    return engine;
}

} // namespace

uint32_t headsetRefresh(const drmModeModeInfo &mode) {
    const uint64_t clocks = uint64_t(mode.htotal) * mode.vtotal;
    if (!mode.hdisplay || !mode.vdisplay || !clocks || !mode.clock || mode.vscan > 1 ||
        (mode.flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN | DRM_MODE_FLAG_3D_MASK)))
        return 0;
    const auto millihertz = (uint64_t(mode.clock) * 1'000'000 + clocks / 2) / clocks;
    return millihertz <= UINT32_MAX ? static_cast<uint32_t>(millihertz) : 0;
}

bool sameHeadsetTiming(const drmModeModeInfo &a, const drmModeModeInfo &b) {
    return a.clock == b.clock && a.hdisplay == b.hdisplay && a.hsync_start == b.hsync_start &&
           a.hsync_end == b.hsync_end && a.htotal == b.htotal && a.hskew == b.hskew &&
           a.vdisplay == b.vdisplay && a.vsync_start == b.vsync_start && a.vsync_end == b.vsync_end &&
           a.vtotal == b.vtotal && a.vscan == b.vscan && a.flags == b.flags;
}

namespace {

struct Property {
    uint32_t id{};
    uint64_t value{};
};

struct AtomicProperties {
    uint32_t connectorCrtc{}, mode{}, active{}, planeCrtc{}, fb{}, fence{};
    uint32_t srcX{}, srcY{}, srcWidth{}, srcHeight{};
    uint32_t crtcX{}, crtcY{}, crtcWidth{}, crtcHeight{}, rotation{};
};

template<size_t N>
std::array<Property, N> properties(int fd, uint32_t object, uint32_t type,
                                   const std::array<const char *, N> &names) {
    const std::unique_ptr<drmModeObjectProperties, decltype(&drmModeFreeObjectProperties)> entries(
        drmModeObjectGetProperties(fd, object, type), drmModeFreeObjectProperties);
    std::array<Property, N> result{};
    if (!entries)
        return result;
    for (uint32_t i = 0; i < entries->count_props; ++i) {
        const std::unique_ptr<drmModePropertyRes, decltype(&drmModeFreeProperty)> detail(
            drmModeGetProperty(fd, entries->props[i]), drmModeFreeProperty);
        if (!detail)
            continue;
        for (size_t j = 0; j < N; ++j) {
            if (!std::strcmp(detail->name, names[j])) {
                result[j] = {detail->prop_id, entries->prop_values[i]};
                break;
            }
        }
    }
    return result;
}

struct PlaneCandidate {
    uint32_t crtc{}, plane{};
    AtomicProperties props;
    std::vector<uint32_t> formats;
    bool primary{};
};

struct ScanoutResources {
    std::vector<PlaneCandidate> candidates;
    bool modifiers{};
};

std::vector<uint32_t> linearFormats(int fd, const drmModePlane &plane, const Property &inFormats,
                                    bool modifiers) {
    if (!modifiers) {
        if (!plane.count_formats)
            return {};
        return {plane.formats, plane.formats + plane.count_formats};
    }
    if (!inFormats.value)
        return {};
    const std::unique_ptr<drmModePropertyBlobRes, decltype(&drmModeFreePropertyBlob)> blob(
        drmModeGetPropertyBlob(fd, static_cast<uint32_t>(inFormats.value)), drmModeFreePropertyBlob);
    if (!blob || !blob->data || blob->length < sizeof(drm_format_modifier_blob))
        return {};
    const auto &header = *static_cast<const drm_format_modifier_blob *>(blob->data);
    const auto fits = [&](uint32_t offset, uint32_t count, size_t stride) {
        return offset >= sizeof(header) && offset <= blob->length &&
               count <= (blob->length - offset) / stride;
    };
    if (!fits(header.formats_offset, header.count_formats, sizeof(uint32_t)) ||
        !fits(header.modifiers_offset, header.count_modifiers, sizeof(drm_format_modifier)))
        return {};
    std::vector<uint32_t> formats;
    drmModeFormatModifierIterator iterator{};
    while (drmModeFormatModifierBlobIterNext(blob.get(), &iterator)) {
        if (iterator.mod == DRM_FORMAT_MOD_LINEAR)
            formats.push_back(iterator.fmt);
    }
    return formats;
}

ScanoutResources scanoutResources(int fd, uint32_t connector, const drmModeModeInfo *timing) {
    check(drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1), "Enable leased DRM planes");
    check(drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1), "Enable leased DRM atomic access");
    uint64_t modifiers{};
    check(drmGetCap(fd, DRM_CAP_ADDFB2_MODIFIERS, &modifiers), "Query DRM framebuffer modifiers");
    const std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)> con(
        drmModeGetConnectorCurrent(fd, connector), drmModeFreeConnector);
    if (!con)
        drmError("Query leased DRM connector");
    if (con->connection != DRM_MODE_CONNECTED)
        throw std::runtime_error("Leased DRM connector " + std::to_string(connector) + " is not connected");
    if (timing && (con->count_modes <= 0 ||
        !std::any_of(con->modes, con->modes + con->count_modes, [&](const drmModeModeInfo &mode) {
            return sameHeadsetTiming(mode, *timing);
        })))
        throw std::runtime_error("The leased connector no longer advertises the selected DRM timing");
    const std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)> resources(
        drmModeGetResources(fd), drmModeFreeResources);
    if (!resources)
        drmError("Query leased DRM CRTC resources");
    const std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)> planes(
        drmModeGetPlaneResources(fd), drmModeFreePlaneResources);
    if (!planes)
        drmError("Query leased DRM plane resources");
    const auto [connectorCrtc] = properties(fd, connector, DRM_MODE_OBJECT_CONNECTOR,
                                           std::array{"CRTC_ID"});
    if (!connectorCrtc.id)
        throw std::runtime_error("Leased DRM connector is missing atomic CRTC_ID");
    uint32_t possible{};
    for (int i = 0; i < con->count_encoders; ++i) {
        const std::unique_ptr<drmModeEncoder, decltype(&drmModeFreeEncoder)> encoder(
            drmModeGetEncoder(fd, con->encoders[i]), drmModeFreeEncoder);
        if (encoder)
            possible |= encoder->possible_crtcs;
    }
    ScanoutResources result;
    result.modifiers = modifiers != 0;
    for (uint32_t j = 0; j < planes->count_planes; ++j) {
        const std::unique_ptr<drmModePlane, decltype(&drmModeFreePlane)> candidate(
            drmModeGetPlane(fd, planes->planes[j]), drmModeFreePlane);
        if (!candidate || !(candidate->possible_crtcs & possible))
            continue;
        const auto values = properties(fd, candidate->plane_id, DRM_MODE_OBJECT_PLANE,
            std::array{"CRTC_ID", "FB_ID", "IN_FENCE_FD", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
                       "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H", "type", "IN_FORMATS", "rotation"});
        if (!std::all_of(values.begin(), values.begin() + 12, [](const Property &prop) { return prop.id; }) ||
            values[11].value == DRM_PLANE_TYPE_CURSOR)
            continue;
        const auto formats = linearFormats(fd, *candidate, values[12], result.modifiers);
        if (formats.empty())
            continue;
        for (int i = 0; i < std::min(resources->count_crtcs, 32); ++i) {
            if (!((candidate->possible_crtcs & possible) & (1u << i)))
                continue;
            const auto crtc = resources->crtcs[i];
            const auto [mode, active] = properties(fd, crtc, DRM_MODE_OBJECT_CRTC,
                                                   std::array{"MODE_ID", "ACTIVE"});
            if (!mode.id || !active.id)
                continue;
            const auto [planeCrtc, fb, fence, srcX, srcY, srcW, srcH, crtcX, crtcY, crtcW, crtcH,
                        type, inFormats, rotation] = values;
            result.candidates.push_back({crtc, candidate->plane_id,
                {connectorCrtc.id, mode.id, active.id, planeCrtc.id, fb.id, fence.id,
                 srcX.id, srcY.id, srcW.id, srcH.id, crtcX.id, crtcY.id, crtcW.id, crtcH.id, rotation.id},
                formats, type.value == DRM_PLANE_TYPE_PRIMARY});
        }
    }
    if (result.candidates.empty())
        throw std::runtime_error("Leased DRM connector has no atomic CRTC and plane with linear formats and IN_FENCE_FD");
    std::stable_partition(result.candidates.begin(), result.candidates.end(),
                          [](const PlaneCandidate &candidate) { return candidate.primary; });
    return result;
}

} // namespace

std::vector<uint32_t> headsetLinearFormats(int leaseFd, uint32_t connectorId) {
    auto resources = scanoutResources(leaseFd, connectorId, nullptr);
    std::vector<uint32_t> result;
    for (const auto &candidate : resources.candidates)
        result.insert(result.end(), candidate.formats.begin(), candidate.formats.end());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

struct HeadsetPresenter::Impl {
    struct Slot {
        uint32_t gem{}, fb{};
        uint64_t point{}, finished{};
    };
    struct Flip {
        int slot{-1};
        uint64_t frameId{}, submittedNs{};
        int fence{-1};
        int dmaEngine{-1};
    };
    struct VblankRequest {
        std::shared_ptr<DisplayEvent> event;
        uint64_t sequence{};
    };
    AtomicProperties props;
    ScanoutResources resources;

    Fd drm, stop;
    uint32_t connector{}, crtc{}, plane{}, modeBlob{}, format{};
    drmModeModeInfo mode{};
    uint32_t refreshNs{};
    std::array<Slot, 4> slots{};
    uint32_t count{};
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::thread reader;
    const uint64_t flipCookie{eventCookie()};
    bool active{}, lost{}, retiring{}, stopping{}, keepScanout{}, traceReady{}, traceDmaEngine{};
    int displayed{-1};
    Flip pending;
    std::vector<DesktopFeedback> feedback;
    uint64_t generation{};
    std::unordered_map<uint64_t, VblankRequest> vblanks;
    mutable bool haveBase{};
    mutable uint64_t baseSequence{};

    ~Impl();
    void open(int leaseFd, uint32_t id, const drmModeModeInfo &);
    void select(uint32_t fb, uint32_t fourcc);
    int commit(uint32_t fb, int fence, uint32_t flags, bool setMode, bool disable = false);
    void clear(Slot &) noexcept;
    void disable() noexcept;
    void stopReader();
    void readEvents();
    void flipped(uint32_t sequence, uint64_t timestamp) noexcept;
    void vblank(uint64_t cookie, uint64_t sequence, uint64_t timestamp) noexcept;
    void cancelVblanks() noexcept;
    void fail() noexcept {
        lost = true;
        cancelVblanks();
        changed.notify_all();
    }

    template<class Predicate>
    void wait(std::unique_lock<std::mutex> &lock, uint64_t timeout, Predicate done) {
        if (timeout == UINT64_MAX)
            changed.wait(lock, done);
        else if (timeout)
            changed.wait_for(lock, std::chrono::nanoseconds(std::min(timeout, uint64_t(INT64_MAX / 2))), done);
    }
};

void HeadsetPresenter::Impl::open(int leaseFd, uint32_t id, const drmModeModeInfo &timing) {
    drm.value = fcntl(leaseFd, F_DUPFD_CLOEXEC, 0);
    if (drm.value < 0)
        drmError("Duplicate headset lease");
    connector = id;
    const auto *trace = std::getenv("V3_TRACE");
    const auto *copyTrace = std::getenv("V3_COPY_TIMING");
    traceReady = (trace && *trace) || (copyTrace && *copyTrace);
    const auto *engineTrace = std::getenv("V3_DMA_ENGINE_TRACE");
    traceDmaEngine = engineTrace && !std::strcmp(engineTrace, "1");
    if (!headsetRefresh(timing))
        throw std::runtime_error("Cannot use the selected DRM mode for headset pacing");
    mode = timing;
    const auto period = std::llround(1'000'000'000 / refresh(mode));
    if (period <= 0 || period > UINT32_MAX)
        throw std::runtime_error("Selected DRM mode period is outside the headset timer range");
    refreshNs = static_cast<uint32_t>(period);
    resources = scanoutResources(drm.value, connector, &mode);
    uint64_t monotonic{};
    check(drmGetCap(drm.value, DRM_CAP_TIMESTAMP_MONOTONIC, &monotonic), "Query DRM event clock");
    if (!monotonic)
        throw std::runtime_error("The headset lease lacks monotonic flip timestamps");
    check(drmModeCreatePropertyBlob(drm.value, &mode, sizeof(mode), &modeBlob), "Create headset mode blob");
    stop.value = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (stop.value < 0)
        drmError("Create headset event stop descriptor");
}

void HeadsetPresenter::Impl::select(uint32_t fb, uint32_t fourcc) {
    int lastError{};
    for (const auto &candidate : resources.candidates) {
        if (std::find(candidate.formats.begin(), candidate.formats.end(), fourcc) == candidate.formats.end())
            continue;
        crtc = candidate.crtc;
        plane = candidate.plane;
        props = candidate.props;
        const int result = commit(fb, -1, DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET, true);
        if (!result) {
            try {
                reader = std::thread([this] { readEvents(); });
            } catch (...) {
                plane = crtc = 0;
                throw;
            }
            return;
        }
        lastError = drmFailureCode(result);
        if (lastError != EINVAL && lastError != ERANGE && lastError != ENOSPC && lastError != EOPNOTSUPP)
            break;
    }
    plane = crtc = 0;
    if (lastError)
        throw std::system_error(lastError, std::generic_category(),
                                "Test headset mode and framebuffer on leased planes");
    throw std::runtime_error("No leased DRM plane accepts the selected linear framebuffer format");
}

int HeadsetPresenter::Impl::commit(uint32_t fb, int fence, uint32_t flags, bool setMode, bool disable) {
    const std::unique_ptr<drmModeAtomicReq, decltype(&drmModeAtomicFree)> request(
        drmModeAtomicAlloc(), drmModeAtomicFree);
    if (!request)
        return -ENOMEM;
    const auto add = [&](uint32_t object, uint32_t prop, uint64_t value) {
        const int status = drmModeAtomicAddProperty(request.get(), object, prop, value);
        if (status < 0)
            drmError("Add headset atomic property", status);
    };
    if (setMode || disable) {
        add(connector, props.connectorCrtc, disable ? 0 : crtc);
        add(crtc, props.mode, disable ? 0 : modeBlob);
        add(crtc, props.active, disable ? 0 : 1);
        add(plane, props.planeCrtc, disable ? 0 : crtc);
    }
    if (setMode && !disable) {
        add(plane, props.srcX, 0);
        add(plane, props.srcY, 0);
        add(plane, props.srcWidth, uint64_t(mode.hdisplay) << 16);
        add(plane, props.srcHeight, uint64_t(mode.vdisplay) << 16);
        add(plane, props.crtcX, 0);
        add(plane, props.crtcY, 0);
        add(plane, props.crtcWidth, mode.hdisplay);
        add(plane, props.crtcHeight, mode.vdisplay);
        if (props.rotation)
            add(plane, props.rotation, DRM_MODE_ROTATE_0);
    }
    add(plane, props.fb, fb);
    if (!disable)
        add(plane, props.fence, uint64_t(int64_t(fence)));
    return drmModeAtomicCommit(drm.value, request.get(), flags,
                               reinterpret_cast<void *>(uintptr_t(flipCookie)));
}

void HeadsetPresenter::Impl::clear(Slot &slot) noexcept {
    if (slot.fb) {
        // Older libdrm headers lack CLOSEFB even on a kernel that supports it.
        struct CloseFramebuffer { uint32_t id, padding; } request{slot.fb, 0};
        const int status = drmIoctl(drm.value, DRM_IOWR(0xD0, CloseFramebuffer), &request);
        if (status) {
            const int error = drmFailureCode(status);
            const bool scanout = keepScanout &&
                ((displayed >= 0 && &slot == &slots[displayed]) ||
                 (pending.slot >= 0 && &slot == &slots[pending.slot]));
            if (!scanout && (error == EINVAL || error == ENOTTY || error == EOPNOTSUPP))
                drmModeRmFB(drm.value, slot.fb);
        }
    }
    if (slot.gem && std::none_of(slots.begin(), slots.end(), [&](const Slot &other) {
            return &slot != &other && slot.gem == other.gem;
        }))
        drmCloseBufferHandle(drm.value, slot.gem);
    slot = {};
}

void HeadsetPresenter::Impl::stopReader() {
    if (reader.joinable()) {
        {
            std::unique_lock lock(mutex);
            retiring = true;
            cancelVblanks();
            changed.notify_all();
            wait(lock, uint64_t(refreshNs) * 4, [&] {
                return lost || pending.slot < 0;
            });
            stopping = true;
        }
        while (eventfd_write(stop.value, 1) && errno == EINTR) {}
        reader.join();
    }
}

void HeadsetPresenter::Impl::disable() noexcept {
    if (active && pending.slot < 0) {
        try {
            commit(0, -1, DRM_MODE_ATOMIC_ALLOW_MODESET, false, true);
        } catch (...) {
            // An unplugged or revoked lease is already inaccessible.
        }
    }
}

HeadsetPresenter::Impl::~Impl() {
    stopReader();
    if (!keepScanout)
        disable();
    for (auto &slot : slots)
        clear(slot);
    if (pending.fence >= 0)
        close(pending.fence);
    if (modeBlob)
        drmModeDestroyPropertyBlob(drm.value, modeBlob);
}

void HeadsetPresenter::Impl::flipped(uint32_t sequence, uint64_t timestamp) noexcept {
    traceWsi("flip_received", sequence, timestamp);
    const std::lock_guard lock(mutex);
    if (pending.slot < 0 || stopping)
        return;
    if (displayed >= 0)
        slots[displayed].finished = slots[displayed].point;
    displayed = pending.slot;
    const Fd fence{std::exchange(pending.fence, -1)};
    try {
        // Match wp_presentation's vsync, hardware clock/completion and zero-copy bits.
        DesktopFeedback sample{pending.frameId, pending.submittedNs, timestamp, sequence,
                               refreshNs, 15, false, true};
        sample.sourceReadyNs = readyTime(fence.value);
        sample.dmaEngine = pending.dmaEngine;
        feedback.push_back(sample);
        ++generation;
        if (pending.frameId == 1)
            std::fprintf(stderr, "wmr-v3: first headset page flip connector=%u CRTC=%u sequence=%u\n",
                         connector, crtc, sequence);
        traceWsi("flip_complete", pending.frameId, sequence, timestamp, sample.sourceReadyNs);
    } catch (...) {
        fail();
    }
    pending = {};
    changed.notify_all();
}

void HeadsetPresenter::Impl::readEvents() {
    // Only this thread reads the lease FD.
    static thread_local Impl *owner;
    owner = this;
    drmEventContext context{};
    context.version = DRM_EVENT_CONTEXT_VERSION;
    context.page_flip_handler2 = [](int, unsigned seq, unsigned sec, unsigned usec, unsigned id, void *data) {
        if (reinterpret_cast<uintptr_t>(data) == owner->flipCookie && id == owner->crtc)
            owner->flipped(seq, uint64_t(sec) * 1'000'000'000 + uint64_t(usec) * 1000);
    };
    context.sequence_handler = [](int, uint64_t sequence, uint64_t timestamp, uint64_t cookie) {
        owner->vblank(cookie, sequence, timestamp);
    };
    pollfd descriptors[]{{drm.value, POLLIN, 0}, {stop.value, POLLIN, 0}};
    for (;;) {
        const int result = poll(descriptors, 2, -1);
        if (result < 0 && errno == EINTR)
            continue;
        if (descriptors[1].revents)
            return;
        const auto events = descriptors[0].revents;
        if (result >= 0 && !(events & (POLLERR | POLLHUP | POLLNVAL))) {
            if (!(events & POLLIN))
                continue;
            errno = 0;
            if (!drmHandleEvent(drm.value, &context) || errno == EINTR || errno == EAGAIN)
                continue;
        }
        const std::lock_guard lock(mutex);
        fail();
        return;
    }
}

void HeadsetPresenter::Impl::cancelVblanks() noexcept {
    for (const auto &request : vblanks)
        request.second.event->cancel();
    vblanks.clear();
}

void HeadsetPresenter::Impl::vblank(uint64_t cookie, uint64_t sequence, uint64_t timestamp) noexcept {
    traceWsi("sequence_received", cookie, sequence, timestamp);
    const std::lock_guard lock(mutex);
    const auto request = vblanks.find(cookie);
    if (request == vblanks.end())
        return;
    const auto target = request->second.sequence;
    auto event = std::move(request->second.event);
    vblanks.erase(request);
    const int status = sequence < target ? -ERANGE : 0;
    traceWsi("sequence_signal", cookie, sequence, timestamp, target, status);
    if (status) {
        event->cancel();
        fail();
    } else {
        event->signal();
    }
}

HeadsetPresenter::HeadsetPresenter(int leaseFd, uint32_t connectorId,
                                   const drmModeModeInfo &mode) : impl_(std::make_unique<Impl>()) {
    impl_->open(leaseFd, connectorId, mode);
}

HeadsetPresenter::~HeadsetPresenter() = default;

bool HeadsetPresenter::supportsBuffer(uint32_t fourcc, uint64_t modifier) const {
    const auto &s = *impl_;
    const std::lock_guard lock(s.mutex);
    return modifier == DRM_FORMAT_MOD_LINEAR &&
        std::any_of(s.resources.candidates.begin(), s.resources.candidates.end(), [&](const auto &candidate) {
            return (!s.plane || (candidate.plane == s.plane && candidate.crtc == s.crtc)) &&
                   std::find(candidate.formats.begin(), candidate.formats.end(), fourcc) != candidate.formats.end();
        });
}

void HeadsetPresenter::retire(bool keepScanout) {
    auto &s = *impl_;
    s.stopReader();
    // Leave the last framebuffer on the leased CRTC until the replacement takes over.
    const std::lock_guard lock(s.mutex);
    s.keepScanout = keepScanout;
    if (!keepScanout) {
        s.disable();
        s.active = false;
    }
}

uint32_t HeadsetPresenter::addBuffer(int dmaBufFd, uint32_t width, uint32_t height, uint32_t fourcc,
                                    uint32_t pitch, uint32_t offset, uint64_t modifier) {
    auto &s = *impl_;
    const std::lock_guard lock(s.mutex);
    if (width != s.mode.hdisplay || height != s.mode.vdisplay)
        throw std::runtime_error("Headset framebuffer extent " + std::to_string(width) + "x" +
                                 std::to_string(height) + " does not match the leased mode " +
                                 std::to_string(s.mode.hdisplay) + "x" + std::to_string(s.mode.vdisplay));
    if (modifier != DRM_FORMAT_MOD_LINEAR || !pitch || (s.count && fourcc != s.format))
        throw std::runtime_error("Headset framebuffer does not match the linear scanout format");
    if (s.count >= s.slots.size() || s.active || s.retiring)
        throw std::logic_error("Cannot add more framebuffers to the headset presenter");
    auto &slot = s.slots[s.count];
    try {
        check(drmPrimeFDToHandle(s.drm.value, dmaBufFd, &slot.gem), "Import headset DMA-BUF");
        const uint32_t handles[4]{slot.gem}, pitches[4]{pitch}, offsets[4]{offset};
        const uint64_t modifiers[4]{modifier};
        const auto result = s.resources.modifiers
            ? drmModeAddFB2WithModifiers(s.drm.value, width, height, fourcc, handles, pitches,
                                         offsets, modifiers, &slot.fb, DRM_MODE_FB_MODIFIERS)
            : drmModeAddFB2(s.drm.value, width, height, fourcc, handles, pitches, offsets, &slot.fb, 0);
        check(result, "Create headset framebuffer");
        if (s.count == 0) {
            s.select(slot.fb, fourcc);
            s.format = fourcc;
            std::fprintf(stderr, "wmr-v3: preflight headset KMS TEST_ONLY passed connector=%u CRTC=%u "
                         "plane=%u mode=%ux%u@%.6fHz fourcc=0x%08x modifier=0x%llx pitch=%u offset=%u\n",
                         s.connector, s.crtc, s.plane, width, height, refresh(s.mode), fourcc,
                         static_cast<unsigned long long>(modifier), pitch, offset);
        }
    } catch (...) {
        std::fprintf(stderr, "wmr-v3: preflight headset buffer setup failed connector=%u CRTC=%u plane=%u "
                     "slot=%u extent=%ux%u fourcc=0x%08x modifier=0x%llx pitch=%u offset=%u\n",
                     s.connector, s.crtc, s.plane, s.count, width, height, fourcc,
                     static_cast<unsigned long long>(modifier), pitch, offset);
        s.clear(slot);
        throw;
    }
    return s.count++;
}

void HeadsetPresenter::presentSyncFile(uint32_t index, uint64_t point, uint64_t frameId, int syncFileFd) {
    auto &s = *impl_;
    traceWsi("flip_begin", frameId, index, point);
    std::unique_lock lock(s.mutex);
    s.wait(lock, uint64_t(s.refreshNs) * 16, [&] { return s.lost || s.retiring || s.pending.slot < 0; });
    if (s.retiring)
        throw std::runtime_error("The headset swapchain was replaced");
    if (s.pending.slot >= 0)
        s.fail();
    if (s.lost)
        throw std::runtime_error("The headset lease was lost");
    if (index >= s.count || point <= s.slots[index].point || s.displayed == int(index) || syncFileFd < -1)
        throw std::logic_error("Invalid headset frame point or sync-file descriptor");
    auto &slot = s.slots[index];
    try {
        const auto flags = DRM_MODE_PAGE_FLIP_EVENT |
                           (s.active ? DRM_MODE_ATOMIC_NONBLOCK : DRM_MODE_ATOMIC_ALLOW_MODESET);
        // Linux replaces the physical scheduler name once the fence signals.
        const auto engine = s.traceDmaEngine && syncFileFd >= 0 ? pendingDmaEngine(syncFileFd) : -1;
        const auto submitted = monotonicNs();
        check(s.commit(slot.fb, syncFileFd, flags, !s.active), "Commit headset framebuffer");
        traceWsi("flip_commit", frameId, index, point, submitted);
        const int traceFence = s.traceReady && syncFileFd >= 0 ? fcntl(syncFileFd, F_DUPFD_CLOEXEC, 0) : -1;
        s.pending = {int(index), frameId, submitted, traceFence, engine};
        s.active = true;
        slot.point = point;
    } catch (...) {
        s.fail();
        throw;
    }
}

int HeadsetPresenter::releasable(const uint32_t *indices, const uint64_t *points, uint32_t count,
                                uint64_t timeoutNs) {
    auto &s = *impl_;
    std::unique_lock lock(s.mutex);
    int available = -1;
    const auto find = [&] {
        if (s.lost || s.retiring)
            return true;
        for (uint32_t i = 0; i < count; ++i) {
            if (indices[i] < s.count && s.slots[indices[i]].finished >= points[i]) {
                available = int(i);
                return true;
            }
        }
        return false;
    };
    if (count && !find())
        s.wait(lock, timeoutNs, find);
    return available;
}

void HeadsetPresenter::dispatch(bool wait, uint64_t timeoutNs) {
    if (!wait)
        return;
    auto &s = *impl_;
    std::unique_lock lock(s.mutex);
    s.wait(lock, timeoutNs, [&] { return s.lost || s.retiring || !s.feedback.empty(); });
}

uint64_t HeadsetPresenter::feedbackGeneration() const {
    const auto &s = *impl_;
    const std::lock_guard lock(s.mutex);
    return s.generation;
}

void HeadsetPresenter::waitForFeedback(uint64_t generation, uint64_t timeoutNs) {
    auto &s = *impl_;
    std::unique_lock lock(s.mutex);
    s.wait(lock, timeoutNs, [&] { return s.lost || s.retiring || s.generation != generation; });
}

void HeadsetPresenter::wakeFeedbackWaiters() {
    auto &s = *impl_;
    const std::lock_guard lock(s.mutex);
    ++s.generation;
    s.changed.notify_all();
}

std::vector<DesktopFeedback> HeadsetPresenter::takeFeedback() {
    auto &s = *impl_;
    const std::lock_guard lock(s.mutex);
    std::vector<DesktopFeedback> result;
    result.swap(s.feedback);
    return result;
}

bool HeadsetPresenter::closeRequested() const {
    const auto &s = *impl_;
    const std::lock_guard lock(s.mutex);
    return s.lost || s.retiring;
}

VkResult HeadsetPresenter::queueVblank(std::shared_ptr<DisplayEvent> event) {
    auto &s = *impl_;
    const std::lock_guard lock(s.mutex);
    if (s.lost || s.retiring)
        return VK_ERROR_OUT_OF_DATE_KHR;
    if (!s.active || !event)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    try {
        const auto request = s.vblanks.emplace(eventCookie(), Impl::VblankRequest{std::move(event)}).first;
        const int status = drmCrtcQueueSequence(s.drm.value, s.crtc,
                                               DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS,
                                               1, &request->second.sequence, request->first);
        const int error = status ? drmFailureCode(status) : 0;
        traceWsi("sequence_armed", request->first, request->second.sequence, 0, 0, status);
        if (!status)
            return VK_SUCCESS;
        request->second.event->cancel();
        s.vblanks.erase(request);
        std::fprintf(stderr, "wmr-v3: headset display event queue failed on CRTC %u (errno %d)\n", s.crtc, error);
        if (error == ENODEV || error == ENOENT || error == EIO)
            s.fail();
        return VK_ERROR_SURFACE_LOST_KHR;
    } catch (const std::bad_alloc &) {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
}

VkResult HeadsetPresenter::counter(uint64_t *value) const {
    const auto &s = *impl_;
    const std::lock_guard lock(s.mutex);
    if (s.lost || s.retiring)
        return VK_ERROR_OUT_OF_DATE_KHR;
    if (s.displayed < 0) {
        *value = 0;
        return VK_SUCCESS;
    }
    uint64_t sequence{}, timestamp{};
    if (drmCrtcGetSequence(s.drm.value, s.crtc, &sequence, &timestamp))
        return VK_ERROR_SURFACE_LOST_KHR;
    if (!s.haveBase) {
        s.baseSequence = sequence;
        s.haveBase = true;
    }
    if (sequence < s.baseSequence)
        return VK_ERROR_SURFACE_LOST_KHR;
    *value = sequence - s.baseSequence;
    traceWsi("counter", *value, sequence, timestamp, s.baseSequence);
    return VK_SUCCESS;
}

} // namespace v3
