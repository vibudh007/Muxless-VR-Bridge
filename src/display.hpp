// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "base.hpp"
#include "state.hpp"
#include <cerrno>
#include <drm_fourcc.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
namespace minimal {
struct Display {
    Fd fd;
    uint32_t connector{}, crtc{}, plane{}, widthMM{}, heightMM{};
    std::string name;
    struct Mode {
        drmModeModeInfo drm;
    };
    std::vector<Mode> modes;
    std::vector<uint32_t> formats;
    explicit Display(int input, uint32_t id) : fd(fcntl(input, F_DUPFD_CLOEXEC, 0)), connector(id) {
        if (fd.get() < 0)
            throw Error(VK_ERROR_SURFACE_LOST_KHR, "Cannot duplicate display lease");
        if (drmSetClientCap(fd.get(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) ||
            drmSetClientCap(fd.get(), DRM_CLIENT_CAP_ATOMIC, 1))
            throw Error(VK_ERROR_FEATURE_NOT_PRESENT, "Atomic display access unavailable");
        auto con = std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>(
            drmModeGetConnector(fd.get(), id), drmModeFreeConnector);
        auto res = std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)>(
            drmModeGetResources(fd.get()), drmModeFreeResources);
        if (!con || !res || con->connection != DRM_MODE_CONNECTED || !con->count_modes)
            throw Error(VK_ERROR_SURFACE_LOST_KHR, "Leased connector is unavailable");
        name = std::string(drmModeGetConnectorTypeName(con->connector_type)) + "-" +
               std::to_string(con->connector_type_id);
        widthMM = con->mmWidth;
        heightMM = con->mmHeight;
        for (int i = 0; i < con->count_modes; ++i)
            modes.push_back({con->modes[i]});
        uint32_t possible = 0;
        for (int i = 0; i < con->count_encoders; ++i) {
            auto *enc = drmModeGetEncoder(fd.get(), con->encoders[i]);
            if (enc) {
                possible |= enc->possible_crtcs;
                drmModeFreeEncoder(enc);
            }
        }
        auto planes = std::unique_ptr<drmModePlaneRes, decltype(&drmModeFreePlaneResources)>(
            drmModeGetPlaneResources(fd.get()), drmModeFreePlaneResources);
        if (!planes)
            throw Error(VK_ERROR_SURFACE_LOST_KHR, "No leased planes");
        for (int c = 0; c < res->count_crtcs && !plane; ++c) {
            if (!(possible & (1u << c)))
                continue;
            for (uint32_t i = 0; i < planes->count_planes && !plane; ++i) {
                auto *p = drmModeGetPlane(fd.get(), planes->planes[i]);
                if (!p)
                    continue;
                if ((p->possible_crtcs & (1u << c)) &&
                    propertyValue(p->plane_id, DRM_MODE_OBJECT_PLANE, "type") ==
                        DRM_PLANE_TYPE_PRIMARY) {
                    crtc = res->crtcs[c];
                    plane = p->plane_id;
                    formats.assign(p->formats, p->formats + p->count_formats);
                }
                drmModeFreePlane(p);
            }
        }
        if (!plane)
            throw Error(VK_ERROR_FEATURE_NOT_PRESENT,
                        "No compatible leased CRTC and primary plane");
    }
    void bindLease(int lease) {
        // Enumeration may use an unprivileged device fd; acquisition supplies the actual lease.
        // Preserve public display/mode identities while selecting only lease-owned resources.
        Display granted(lease, connector);
        fd = std::move(granted.fd);
        crtc = granted.crtc;
        plane = granted.plane;
        formats = std::move(granted.formats);
        note("Bound the acquisition fd and its leased display resources");
    }
    uint32_t property(uint32_t object, uint32_t type, const char *name) const {
        uint32_t id = 0;
        auto *list = drmModeObjectGetProperties(fd.get(), object, type);
        if (list) {
            for (uint32_t i = 0; i < list->count_props; ++i) {
                auto *p = drmModeGetProperty(fd.get(), list->props[i]);
                if (p) {
                    if (!strcmp(p->name, name))
                        id = p->prop_id;
                    drmModeFreeProperty(p);
                }
            }
            drmModeFreeObjectProperties(list);
        }
        if (!id)
            throw Error(VK_ERROR_FEATURE_NOT_PRESENT, name);
        return id;
    }
    uint64_t propertyValue(uint32_t object, uint32_t type, const char *name) const {
        uint64_t value = UINT64_MAX;
        auto *list = drmModeObjectGetProperties(fd.get(), object, type);
        if (list) {
            for (uint32_t i = 0; i < list->count_props; ++i) {
                auto *p = drmModeGetProperty(fd.get(), list->props[i]);
                if (p) {
                    if (!strcmp(p->name, name))
                        value = list->prop_values[i];
                    drmModeFreeProperty(p);
                }
            }
            drmModeFreeObjectProperties(list);
        }
        return value;
    }
    static double hz(const Mode &m) {
        double rate = double(m.drm.clock) * 1000 / m.drm.htotal / m.drm.vtotal;
        if (m.drm.flags & DRM_MODE_FLAG_INTERLACE)
            rate *= 2;
        if (m.drm.flags & DRM_MODE_FLAG_DBLSCAN)
            rate /= 2;
        if (m.drm.vscan > 1)
            rate /= m.drm.vscan;
        return rate;
    }
};
struct Surface {
    std::shared_ptr<Display> display;
    Display::Mode *mode;
    VkExtent2D extent;
};
class Presenter {
    struct Framebuffer {
        uint32_t gem = 0, fb = 0;
    };
    std::shared_ptr<Display> d_;
    Display::Mode mode_;
    uint32_t blob_ = 0;
    std::vector<Framebuffer> buffers_;
    BufferStates states_;
    std::mutex lock_;
    std::condition_variable changed_;
    Fd wake_{eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    std::thread reader_;
    bool active_ = false, stop_ = false;
    VkResult error_ = VK_SUCCESS;
    uint64_t sequenceBase_ = 0;
    bool haveBase_ = false;
    struct Properties {
        uint32_t connectorCrtc, mode, active, fb, crtc, sx, sy, sw, sh, dx, dy, dw, dh, fence;
    } p_{};
    static void flipped(int, unsigned, unsigned, unsigned, void *data) noexcept {
        auto *self = static_cast<Presenter *>(data);
        try {
            self->states_.flip();
            if (self->states_.completed() == 1)
                note("First frame displayed (kernel page-flip event)");
        } catch (...) {
            self->error_ = VK_ERROR_SURFACE_LOST_KHR;
        }
    }
    void events() {
        for (;;) {
            pollfd fds[2]{{d_->fd.get(), POLLIN, 0}, {wake_.get(), POLLIN, 0}};
            int r;
            do {
                r = poll(fds, 2, -1);
            } while (r < 0 && errno == EINTR);
            std::lock_guard<std::mutex> g(lock_);
            if (stop_)
                return;
            if (r < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
                error_ = VK_ERROR_SURFACE_LOST_KHR;
            else if (fds[0].revents & POLLIN) {
                drmEventContext context{};
                context.version = 2;
                context.page_flip_handler = flipped;
                if (drmHandleEvent(d_->fd.get(), &context))
                    error_ = VK_ERROR_SURFACE_LOST_KHR;
            }
            changed_.notify_all();
            if (error_ != VK_SUCCESS)
                return;
        }
    }
    int commit(unsigned image, int fence, bool test, bool disable = false) {
        auto *req = drmModeAtomicAlloc();
        if (!req)
            return -ENOMEM;
        bool valid = true;
        const auto add = [&](uint32_t obj, uint32_t prop, uint64_t value) {
            if (drmModeAtomicAddProperty(req, obj, prop, value) < 0)
                valid = false;
        };
        if (disable) {
            add(d_->plane, p_.fb, 0);
            add(d_->plane, p_.crtc, 0);
            add(d_->connector, p_.connectorCrtc, 0);
            add(d_->crtc, p_.active, 0);
            add(d_->crtc, p_.mode, 0);
        } else {
            if (!active_ || test) {
                add(d_->connector, p_.connectorCrtc, d_->crtc);
                add(d_->crtc, p_.mode, blob_);
                add(d_->crtc, p_.active, 1);
                add(d_->plane, p_.crtc, d_->crtc);
                add(d_->plane, p_.sx, 0);
                add(d_->plane, p_.sy, 0);
                add(d_->plane, p_.dx, 0);
                add(d_->plane, p_.dy, 0);
                add(d_->plane, p_.sw, uint64_t(mode_.drm.hdisplay) << 16);
                add(d_->plane, p_.sh, uint64_t(mode_.drm.vdisplay) << 16);
                add(d_->plane, p_.dw, mode_.drm.hdisplay);
                add(d_->plane, p_.dh, mode_.drm.vdisplay);
            }
            add(d_->plane, p_.fb, buffers_.at(image).fb);
            add(d_->plane, p_.fence, static_cast<uint64_t>(static_cast<int64_t>(fence)));
        }
        // Establish the display synchronously once. A modeset completion can be
        // delivered before its commit tail finishes; do not submit a normal flip
        // into that partially initialized state. Steady-state flips stay nonblocking.
        uint32_t flags = disable ? DRM_MODE_ATOMIC_ALLOW_MODESET
                         : test  ? (DRM_MODE_ATOMIC_TEST_ONLY | DRM_MODE_ATOMIC_ALLOW_MODESET)
                                 : (DRM_MODE_PAGE_FLIP_EVENT |
                                    (active_ ? DRM_MODE_ATOMIC_NONBLOCK
                                             : DRM_MODE_ATOMIC_ALLOW_MODESET));
        int result = valid ? drmModeAtomicCommit(d_->fd.get(), req, flags, this) : -1;
        const int savedError = errno;
        drmModeAtomicFree(req);
        errno = savedError;
        return result;
    }

  public:
    Presenter(std::shared_ptr<Display> d, const Display::Mode &mode, unsigned count)
        : d_(std::move(d)), mode_(mode), states_(count) {
        const auto plane = [&](const char *n) {
            return d_->property(d_->plane, DRM_MODE_OBJECT_PLANE, n);
        };
        p_ = {d_->property(d_->connector, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID"),
              d_->property(d_->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID"),
              d_->property(d_->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE"),
              plane("FB_ID"),
              plane("CRTC_ID"),
              plane("SRC_X"),
              plane("SRC_Y"),
              plane("SRC_W"),
              plane("SRC_H"),
              plane("CRTC_X"),
              plane("CRTC_Y"),
              plane("CRTC_W"),
              plane("CRTC_H"),
              plane("IN_FENCE_FD")};
        if (wake_.get() < 0 ||
            drmModeCreatePropertyBlob(d_->fd.get(), &mode_.drm, sizeof(mode_.drm), &blob_))
            throw Error(VK_ERROR_INITIALIZATION_FAILED, "Display setup failed");
    }
    void add(int dmaBuf, uint64_t pitch, uint64_t offset, uint32_t fourcc) {
        if (pitch > UINT32_MAX || offset > UINT32_MAX ||
            std::find(d_->formats.begin(), d_->formats.end(), fourcc) == d_->formats.end())
            throw Error(VK_ERROR_FORMAT_NOT_SUPPORTED, "Display layout unsupported");
        Framebuffer f;
        if (drmPrimeFDToHandle(d_->fd.get(), dmaBuf, &f.gem))
            throw Error(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Display cannot import shared memory");
        uint32_t h[4]{f.gem}, p[4]{uint32_t(pitch)}, o[4]{uint32_t(offset)};
        uint64_t modifiers[4]{};
        if (drmModeAddFB2WithModifiers(d_->fd.get(), mode_.drm.hdisplay, mode_.drm.vdisplay, fourcc,
                                       h, p, o, modifiers, &f.fb, DRM_MODE_FB_MODIFIERS)) {
            drm_gem_close close{};
            close.handle = f.gem;
            drmIoctl(d_->fd.get(), DRM_IOCTL_GEM_CLOSE, &close);
            throw Error(VK_ERROR_FORMAT_NOT_SUPPORTED, "Display rejected framebuffer");
        }
        buffers_.push_back(f);
    }
    void start() {
        if (commit(0, -1, true))
            throw Error(VK_ERROR_INITIALIZATION_FAILED,
                        (std::string("Display TEST_ONLY failed: ") + strerror(errno)).c_str());
        reader_ = std::thread([this] { events(); });
    }
    VkResult acquire(uint64_t timeout, uint32_t *out) {
        auto until = deadline(timeout);
        std::unique_lock<std::mutex> g(lock_);
        for (;;) {
            if (error_ != VK_SUCCESS)
                return error_;
            int i = states_.acquire();
            if (i >= 0) {
                *out = unsigned(i);
                return VK_SUCCESS;
            }
            if (!timeout)
                return VK_NOT_READY;
            if (changed_.wait_until(g, until) == std::cv_status::timeout)
                return VK_TIMEOUT;
        }
    }
    bool acquired(unsigned i) {
        std::lock_guard<std::mutex> g(lock_);
        return states_.acquired(i);
    }
    void cancel(unsigned i) {
        std::lock_guard<std::mutex> g(lock_);
        states_.cancel(i);
        changed_.notify_all();
    }
    VkResult present(unsigned image, int fence, uint64_t id) {
        std::unique_lock<std::mutex> g(lock_);
        if (!states_.acquired(image) || id <= states_.completed())
            return VK_ERROR_INITIALIZATION_FAILED;
        changed_.wait(g, [&] { return !states_.pending() || error_ != VK_SUCCESS; });
        if (error_ != VK_SUCCESS)
            return error_;
        if (commit(image, fence, false)) {
            error_ = VK_ERROR_SURFACE_LOST_KHR;
            changed_.notify_all();
            return error_;
        }
        states_.queue(image, id);
        active_ = true;
        return VK_SUCCESS;
    }
    VkResult wait(uint64_t id, uint64_t timeout) {
        auto until = deadline(timeout);
        std::unique_lock<std::mutex> g(lock_);
        while (states_.completed() < id) {
            if (error_ != VK_SUCCESS)
                return error_;
            if (!timeout)
                return VK_TIMEOUT;
            if (changed_.wait_until(g, until) == std::cv_status::timeout)
                return VK_TIMEOUT;
        }
        return VK_SUCCESS;
    }
    VkResult counter(uint64_t *out) {
        std::lock_guard<std::mutex> g(lock_);
        uint64_t sequence = 0, ns = 0;
        // Accepting an initial commit does not mean its display engine is ready.
        // Never query a CRTC while the initial modeset is still in flight.
        if (!states_.completed()) {
            *out = 0;
            return VK_SUCCESS;
        }
        if (drmCrtcGetSequence(d_->fd.get(), d_->crtc, &sequence, &ns))
            return VK_ERROR_SURFACE_LOST_KHR;
        if (!ns) {
            *out = 0;
            return VK_SUCCESS;
        }
        if (!haveBase_) {
            sequenceBase_ = sequence;
            haveBase_ = true;
        }
        *out = sequence - sequenceBase_;
        return VK_SUCCESS;
    }
    ~Presenter() {
        {
            std::unique_lock<std::mutex> g(lock_);
            changed_.wait_for(g, std::chrono::seconds(2),
                              [&] { return !states_.pending() || error_ != VK_SUCCESS; });
            if (active_ && commit(0, -1, false, true)) {
                const int failure = errno;
                auto *lease = drmModeGetLease(d_->fd.get());
                if (lease && lease->count == 0)
                    note("Lease already revoked at teardown");
                else
                    note((std::string("Display detach failed: ") + strerror(failure)).c_str());
                if (lease)
                    drmFree(lease);
            }
            stop_ = true;
        }
        if (reader_.joinable()) {
            uint64_t one = 1;
            (void)write(wake_.get(), &one, sizeof(one));
            reader_.join();
        }
        for (auto &f : buffers_) {
            drmModeRmFB(d_->fd.get(), f.fb);
            drm_gem_close close{};
            close.handle = f.gem;
            drmIoctl(d_->fd.get(), DRM_IOCTL_GEM_CLOSE, &close);
        }
        if (blob_)
            drmModeDestroyPropertyBlob(d_->fd.get(), blob_);
    }
};
} // namespace minimal
