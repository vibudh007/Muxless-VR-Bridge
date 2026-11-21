#include "desktop.hpp"

#include "linux-dmabuf-v1-client-protocol.h"
#include "linux-drm-syncobj-v1-client-protocol.h"
#include "presentation-time-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iterator>
#include <list>
#include <new>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <time.h>
#include <unistd.h>
#include <utility>

namespace {

constexpr uint64_t nsPerSecond = 1'000'000'000;
constexpr uint64_t setupTimeoutNs = 10 * nsPerSecond;
constexpr uint32_t maxDesktopSlots = 4;

struct GlobalRequirement {
    const wl_interface *interface;
    uint32_t version;
};

constexpr std::array desktopGlobals{
    GlobalRequirement{&wl_compositor_interface, 4},
    GlobalRequirement{&xdg_wm_base_interface, 1},
    GlobalRequirement{&zwp_linux_dmabuf_v1_interface, 4},
    GlobalRequirement{&wp_linux_drm_syncobj_manager_v1_interface, 1},
    GlobalRequirement{&wp_presentation_interface, 1},
    GlobalRequirement{&wp_viewporter_interface, 1},
    GlobalRequirement{&wl_output_interface, 4},
};

uint64_t nowNs() {
    timespec value{};
    if (clock_gettime(CLOCK_MONOTONIC, &value) < 0)
        throw std::system_error(errno, std::generic_category(), "read monotonic clock");
    return uint64_t(value.tv_sec) * nsPerSecond + uint64_t(value.tv_nsec);
}

void checkDrm(int result, const char *operation) {
    if (result) {
        const int error = result < -1 ? -result : errno;
        throw std::system_error(error ? error : EIO, std::generic_category(), operation);
    }
}

uint32_t high(uint64_t value) { return uint32_t(value >> 32); }
uint32_t low(uint64_t value) { return uint32_t(value); }
uint64_t combine(uint32_t hi, uint32_t lo) { return (uint64_t(hi) << 32) | lo; }

template<class T> T *requireProxy(T *proxy) {
    if (!proxy)
        throw std::bad_alloc();
    return proxy;
}

void requireListener(int result) {
    if (result != 0)
        throw std::runtime_error("Could not register a Wayland listener");
}

struct Fd {
    int value{-1};
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() {
        if (value >= 0)
            close(value);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};

std::string findDesktopRenderNode() {
    struct Devices {
        std::array<drmDevicePtr, 32> nodes{};
        int count{};
        ~Devices() { if (count > 0) drmFreeDevices(nodes.data(), count); }
    } devices;
    devices.count = std::min<int>(drmGetDevices2(0, devices.nodes.data(), devices.nodes.size()),
                                  devices.nodes.size());
    constexpr int required = (1 << DRM_NODE_PRIMARY) | (1 << DRM_NODE_RENDER);
    for (int i = 0; i < devices.count; ++i) {
        const auto *device = devices.nodes[i];
        if (!device || (device->available_nodes & required) != required)
            continue;
        const Fd fd(::open(device->nodes[DRM_NODE_PRIMARY], O_RDONLY | O_CLOEXEC));
        if (fd.value < 0)
            continue;
        const std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)>
            resources(drmModeGetResources(fd.value), drmModeFreeResources);
        if (!resources)
            continue;
        for (int j = 0; j < resources->count_connectors; ++j) {
            const std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>
                connector(drmModeGetConnectorCurrent(fd.value, resources->connectors[j]), drmModeFreeConnector);
            if (connector && connector->connector_type == DRM_MODE_CONNECTOR_eDP &&
                connector->connector_type_id == 1 && connector->connection == DRM_MODE_CONNECTED)
                return device->nodes[DRM_NODE_RENDER];
        }
    }
    return "/dev/dri/renderD129";
}

} // namespace

const char *desktopRenderNode() {
    const auto *node = std::getenv("V3_DISPLAY_DRM");
    if (node && *node)
        return node;
    static const auto detected = findDesktopRenderNode();
    return detected.c_str();
}

struct DesktopPresenter::Impl {
    struct Output {
        Impl *owner{};
        wl_output *proxy{};
        std::string name;
        uint32_t global{};
    };

    struct Slot {
        wl_buffer *buffer{};
        wp_linux_drm_syncobj_timeline_v1 *acquire{};
        wp_linux_drm_syncobj_timeline_v1 *release{};
        uint32_t releaseHandle{};
        uint64_t point{};
    };

    struct Pending {
        Impl *owner{};
        struct wp_presentation_feedback *proxy{};
        DesktopFeedback feedback;
        std::list<Pending>::iterator entry;
    };

    struct Import {
        wl_buffer *buffer{};
        bool done{};

        ~Import() {
            if (buffer)
                wl_buffer_destroy(buffer);
        }
    };

    Fd drm;
    wl_display *display{};
    wl_registry *registry{};
    wl_compositor *compositor{};
    xdg_wm_base *shell{};
    zwp_linux_dmabuf_v1 *dmabuf{};
    wp_linux_drm_syncobj_manager_v1 *syncManager{};
    wp_presentation *presentation{};
    wp_viewporter *viewporter{};
    wl_surface *surface{};
    wp_viewport *viewport{};
    xdg_surface *xdgSurface{};
    xdg_toplevel *toplevel{};
    wp_linux_drm_syncobj_surface_v1 *surfaceSync{};
    std::list<Output> outputs;
    wl_output *output{};
    std::array<uint32_t, desktopGlobals.size()> announcedGlobals{};
    std::string drmNode;
    std::vector<Slot> slots;
    std::list<Pending> pending;
    std::vector<DesktopFeedback> completed;
    std::exception_ptr callbackError;
    int32_t width{}, height{}, viewportWidth{}, viewportHeight{};
    int32_t presentationClock{-1};
    bool configured{}, closed{}, flushPending{}, fullscreen{};

    ~Impl() {
        if (surface && configured && !wl_display_get_error(display)) {
            wl_surface_attach(surface, nullptr, 0, 0);
            wl_surface_commit(surface);
        }
        for (const auto &cue : pending)
            wp_presentation_feedback_destroy(cue.proxy);
        for (const auto &slot : slots)
            destroySlot(slot);
        if (surfaceSync)
            wp_linux_drm_syncobj_surface_v1_destroy(surfaceSync);
        if (viewport)
            wp_viewport_destroy(viewport);
        if (toplevel)
            xdg_toplevel_destroy(toplevel);
        if (xdgSurface)
            xdg_surface_destroy(xdgSurface);
        if (surface)
            wl_surface_destroy(surface);
        for (const auto &candidate : outputs) {
            if (candidate.proxy)
                wl_output_release(candidate.proxy);
        }
        if (viewporter)
            wp_viewporter_destroy(viewporter);
        if (syncManager)
            wp_linux_drm_syncobj_manager_v1_destroy(syncManager);
        if (presentation)
            wp_presentation_destroy(presentation);
        if (dmabuf)
            zwp_linux_dmabuf_v1_destroy(dmabuf);
        if (shell)
            xdg_wm_base_destroy(shell);
        if (compositor)
            wl_compositor_destroy(compositor);
        if (registry)
            wl_registry_destroy(registry);
        if (display) {
            wl_display_flush(display);
            wl_display_disconnect(display);
        }
    }

    void destroySlot(const Slot &slot) const noexcept {
        if (slot.buffer)
            wl_buffer_destroy(slot.buffer);
        if (slot.release)
            wp_linux_drm_syncobj_timeline_v1_destroy(slot.release);
        if (slot.acquire)
            wp_linux_drm_syncobj_timeline_v1_destroy(slot.acquire);
        if (slot.releaseHandle)
            drmSyncobjDestroy(drm.value, slot.releaseHandle);
    }

    void checkWayland(int result, const char *operation) const {
        if (callbackError)
            std::rethrow_exception(callbackError);
        if (result >= 0)
            return;
        const int error = wl_display_get_error(display);
        if (error == EPROTO) {
            const wl_interface *interface = nullptr;
            uint32_t id = 0;
            const auto code = wl_display_get_protocol_error(display, &interface, &id);
            throw std::runtime_error(std::string(operation) + ": " +
                                     (interface ? interface->name : "Wayland") + " object " +
                                     std::to_string(id) + ", error " + std::to_string(code));
        }
        throw std::system_error(error ? error : errno, std::generic_category(), operation);
    }

    template<class T>
    T *bind(uint32_t name, const wl_interface &interface, uint32_t version) const {
        return requireProxy(static_cast<T *>(wl_registry_bind(registry, name, &interface, version)));
    }

    template<class Fn> void handleEvent(Fn event) noexcept {
        if (callbackError)
            return;
        try {
            event();
        } catch (...) {
            callbackError = std::current_exception();
            closed = true;
        }
    }

    static void global(void *data, wl_registry *, uint32_t name, const char *interface,
                       uint32_t version) noexcept {
        auto &self = *static_cast<Impl *>(data);
        self.handleEvent([&] {
            for (size_t i = 0; i < desktopGlobals.size(); ++i) {
                if (!strcmp(interface, desktopGlobals[i].interface->name)) {
                    self.announcedGlobals[i] = std::max(self.announcedGlobals[i], version);
                    break;
                }
            }
            if (!version)
                return;
            if (!self.compositor && !strcmp(interface, wl_compositor_interface.name) && version >= 4)
                self.compositor = self.bind<wl_compositor>(name, wl_compositor_interface, 4);
            else if (!self.shell && !strcmp(interface, xdg_wm_base_interface.name)) {
                self.shell = self.bind<xdg_wm_base>(name, xdg_wm_base_interface, 1);
                requireListener(xdg_wm_base_add_listener(self.shell, &shellListener, &self));
            } else if (!self.dmabuf && !strcmp(interface, zwp_linux_dmabuf_v1_interface.name) && version >= 4)
                self.dmabuf = self.bind<zwp_linux_dmabuf_v1>(name, zwp_linux_dmabuf_v1_interface, 4);
            else if (!self.syncManager && !strcmp(interface, wp_linux_drm_syncobj_manager_v1_interface.name))
                self.syncManager = self.bind<wp_linux_drm_syncobj_manager_v1>(
                    name, wp_linux_drm_syncobj_manager_v1_interface, 1);
            else if (!self.presentation && !strcmp(interface, wp_presentation_interface.name)) {
                self.presentation = self.bind<wp_presentation>(
                    name, wp_presentation_interface, std::min(version, 2u));
                requireListener(wp_presentation_add_listener(self.presentation, &presentationListener, &self));
            } else if (!self.viewporter && !strcmp(interface, wp_viewporter_interface.name))
                self.viewporter = self.bind<wp_viewporter>(name, wp_viewporter_interface, 1);
            else if (!strcmp(interface, wl_output_interface.name) && version >= 4) {
                auto &candidate = self.outputs.emplace_back();
                candidate.owner = &self;
                candidate.proxy = self.bind<wl_output>(name, wl_output_interface, 4);
                candidate.global = name;
                requireListener(wl_output_add_listener(candidate.proxy, &outputListener, &candidate));
            }
        });
    }

    static void globalRemoved(void *data, wl_registry *, uint32_t name) {
        auto &self = *static_cast<Impl *>(data);
        for (auto &candidate : self.outputs) {
            if (candidate.global != name || !candidate.proxy)
                continue;
            if (candidate.proxy == self.output) {
                self.closed = true;
                self.output = nullptr;
            }
            wl_output_release(candidate.proxy);
            candidate.proxy = nullptr;
            break;
        }
    }

    static constexpr wl_registry_listener registryListener{global, globalRemoved};
    static constexpr wl_output_listener outputListener{
        [](void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t,
           const char *, const char *, int32_t) {},
        [](void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {},
        [](void *, wl_output *) {},
        [](void *, wl_output *, int32_t) {},
        [](void *data, wl_output *, const char *name) {
            auto &candidate = *static_cast<Output *>(data);
            candidate.owner->handleEvent([&] { candidate.name = name; });
        },
        [](void *, wl_output *, const char *) {},
    };
    static constexpr wp_presentation_listener presentationListener{
        [](void *data, wp_presentation *, uint32_t id) { static_cast<Impl *>(data)->presentationClock = id; },
    };
    static constexpr xdg_wm_base_listener shellListener{
        [](void *, xdg_wm_base *proxy, uint32_t serial) { xdg_wm_base_pong(proxy, serial); },
    };
    static constexpr xdg_surface_listener surfaceListener{
        [](void *data, xdg_surface *proxy, uint32_t serial) {
            xdg_surface_ack_configure(proxy, serial);
            static_cast<Impl *>(data)->configured = true;
        },
    };
    static constexpr xdg_toplevel_listener toplevelListener{
        [](void *data, xdg_toplevel *, int32_t w, int32_t h, wl_array *) {
            auto &self = *static_cast<Impl *>(data);
            if (w > 0 && h > 0) {
                self.width = self.fullscreen ? w : static_cast<int32_t>(std::min<int64_t>(w, int64_t(h) * 2));
                self.height = self.fullscreen ? h : std::max(1, self.width / 2);
            }
        },
        [](void *data, xdg_toplevel *) { static_cast<Impl *>(data)->closed = true; },
        nullptr,
        nullptr,
    };
    static constexpr zwp_linux_buffer_params_v1_listener importListener{
        [](void *data, zwp_linux_buffer_params_v1 *, wl_buffer *buffer) {
            auto &result = *static_cast<Import *>(data);
            result.buffer = buffer;
            result.done = true;
        },
        [](void *data, zwp_linux_buffer_params_v1 *) { static_cast<Import *>(data)->done = true; },
    };
    void finish(const Pending &cue) {
        completed.push_back(cue.feedback);
        wp_presentation_feedback_destroy(cue.proxy);
        pending.erase(cue.entry);
    }

    static constexpr wp_presentation_feedback_listener feedbackListener{
        [](void *data, struct wp_presentation_feedback *, wl_output *presentedOutput) {
            auto &cue = *static_cast<Pending *>(data);
            cue.feedback.targetOutput |= presentedOutput == cue.owner->output;
        },
        [](void *data, struct wp_presentation_feedback *, uint32_t secHi,
           uint32_t secLo, uint32_t ns, uint32_t refresh, uint32_t seqHi,
           uint32_t seqLo, uint32_t flags) {
            auto &cue = *static_cast<Pending *>(data);
            cue.feedback.presentedNs = combine(secHi, secLo) * nsPerSecond + ns;
            cue.feedback.sequence = combine(seqHi, seqLo);
            cue.feedback.refreshNs = refresh;
            cue.feedback.flags = flags;
            cue.owner->handleEvent([&] { cue.owner->finish(cue); });
        },
        [](void *data, struct wp_presentation_feedback *) {
            auto &cue = *static_cast<Pending *>(data);
            cue.feedback.discarded = true;
            cue.owner->handleEvent([&] { cue.owner->finish(cue); });
        },
    };

    template<class Complete> void waitFor(Complete complete, const char *operation) {
        const auto deadline = nowNs() + setupTimeoutNs;
        while (!complete() && !closed) {
            const auto now = nowNs();
            if (now >= deadline)
                throw std::system_error(ETIMEDOUT, std::generic_category(), operation);
            dispatch(true, deadline - now);
        }
        if (closed)
            throw std::runtime_error(std::string(operation) + ": window closed or selected output removed");
    }

    void roundtrip(const char *operation) {
        static constexpr wl_callback_listener syncListener{
            [](void *data, wl_callback *, uint32_t) { *static_cast<bool *>(data) = true; },
        };
        bool done = false;
        const std::unique_ptr<wl_callback, decltype(&wl_callback_destroy)> sync(
            requireProxy(wl_display_sync(display)), wl_callback_destroy);
        requireListener(wl_callback_add_listener(sync.get(), &syncListener, &done));
        waitFor([&] { return done; }, operation);
    }

    void checkGlobals() const {
        std::string missing;
        for (size_t i = 0; i < desktopGlobals.size(); ++i) {
            const auto &required = desktopGlobals[i];
            if (announcedGlobals[i] >= required.version)
                continue;
            if (!missing.empty())
                missing += ", ";
            missing += std::string(required.interface->name) + " v" + std::to_string(required.version) +
                       (announcedGlobals[i] ? " (advertised v" + std::to_string(announcedGlobals[i]) + ")"
                                            : " (not advertised)");
        }
        if (!missing.empty())
            throw std::runtime_error("Wayland compositor lacks required desktop globals: " + missing);
    }

    void open(const char *node, const char *outputName) {
        if (!node || !*node || !outputName || !*outputName)
            throw std::runtime_error("Desktop presentation requires a DRM node and Wayland output name");
        drmNode = node;
        drm.value = ::open(node, O_RDWR | O_CLOEXEC);
        if (drm.value < 0) {
            const int error = errno;
            throw std::system_error(error, std::generic_category(), "open desktop DRM node " + drmNode);
        }
        uint64_t timelineSupport{};
        const int capability = drmGetCap(drm.value, DRM_CAP_SYNCOBJ_TIMELINE, &timelineSupport);
        if (capability) {
            const int error = capability < -1 ? -capability : errno;
            throw std::system_error(error ? error : EIO, std::generic_category(),
                                    "query DRM_CAP_SYNCOBJ_TIMELINE on " + drmNode);
        }
        if (!timelineSupport)
            throw std::runtime_error("Desktop DRM node " + drmNode + " lacks DRM_CAP_SYNCOBJ_TIMELINE");
        const auto *socket = std::getenv("V3_WAYLAND_DISPLAY");
        display = wl_display_connect(socket);
        if (!display) {
            const int error = errno;
            throw std::system_error(error, std::generic_category(),
                                    socket ? std::string("connect to Wayland socket ") + socket :
                                             "connect to the default Wayland display");
        }
        registry = requireProxy(wl_display_get_registry(display));
        requireListener(wl_registry_add_listener(registry, &registryListener, this));
        roundtrip("bind Wayland globals");
        checkGlobals();
        roundtrip("discover Wayland outputs and presentation clock");
        if (presentationClock != CLOCK_MONOTONIC)
            throw std::runtime_error("Wayland presentation clock " + std::to_string(presentationClock) +
                                     " differs from CLOCK_MONOTONIC (" + std::to_string(CLOCK_MONOTONIC) + ")");
        for (const auto &candidate : outputs) {
            if (candidate.proxy && candidate.name == outputName) {
                output = candidate.proxy;
                break;
            }
        }
        if (!output) {
            std::string available;
            for (const auto &candidate : outputs) {
                if (!candidate.proxy || candidate.name.empty())
                    continue;
                if (!available.empty())
                    available += ", ";
                available += candidate.name;
            }
            throw std::runtime_error(std::string("Wayland output ") + outputName + " unavailable (named outputs: " +
                                     (available.empty() ? "none" : available) + ")");
        }
        surface = requireProxy(wl_compositor_create_surface(compositor));
        viewport = requireProxy(wp_viewporter_get_viewport(viewporter, surface));
        surfaceSync = requireProxy(wp_linux_drm_syncobj_manager_v1_get_surface(syncManager, surface));
        xdgSurface = requireProxy(xdg_wm_base_get_xdg_surface(shell, surface));
        requireListener(xdg_surface_add_listener(xdgSurface, &surfaceListener, this));
        toplevel = requireProxy(xdg_surface_get_toplevel(xdgSurface));
        requireListener(xdg_toplevel_add_listener(toplevel, &toplevelListener, this));
        xdg_toplevel_set_title(toplevel, "Muxless VR Bridge");
        xdg_toplevel_set_app_id(toplevel, "muxless-vr-bridge");
        width = 1440;
        height = 720;
        const auto *requestFullscreen = std::getenv("V3_FULLSCREEN");
        fullscreen = requestFullscreen && !std::strcmp(requestFullscreen, "1");
        if (fullscreen)
            xdg_toplevel_set_fullscreen(toplevel, output);
        wl_surface_commit(surface);
        waitFor([&] { return configured; }, "configure Wayland window");
    }

    wp_linux_drm_syncobj_timeline_v1 *importTimeline(int fd, const char *operation,
                                                    uint32_t *retained = nullptr) const {
        uint32_t handle{};
        checkDrm(drmSyncobjFDToHandle(drm.value, fd, &handle), operation);
        if (retained)
            *retained = handle;
        else
            checkDrm(drmSyncobjDestroy(drm.value, handle), "drop local validation syncobj handle");
        return requireProxy(wp_linux_drm_syncobj_manager_v1_import_timeline(syncManager, fd));
    }

    void flush() {
        const int result = wl_display_flush(display);
        flushPending = result < 0 && errno == EAGAIN;
        if (!flushPending)
            checkWayland(result, "flush Wayland requests");
    }

    void dispatch(bool wait, uint64_t timeoutNs = UINT64_MAX) {
        const auto initial = wl_display_dispatch_pending(display);
        checkWayland(initial, "dispatch pending Wayland events");
        if (closed)
            return;
        if (initial > 0)
            wait = false;
        const auto start = nowNs();
        const auto deadline = start + std::min(timeoutNs, UINT64_MAX - start);
        while (true) {
            flush();
            pollfd connection{
                wl_display_get_fd(display),
                short(POLLIN | (flushPending ? POLLOUT : 0)),
                0,
            };
            const auto now = nowNs();
            const auto remaining = wait && deadline > now ? deadline - now : 0;
            const timespec duration{time_t(remaining / nsPerSecond), long(remaining % nsPerSecond)};
            if (wl_display_prepare_read(display) != 0) {
                checkWayland(wl_display_dispatch_pending(display), "dispatch queued Wayland events");
                return;
            }
            const int result = ppoll(&connection, 1, wait && timeoutNs == UINT64_MAX ? nullptr : &duration, nullptr);
            if (result > 0 && (connection.revents & (POLLNVAL | POLLERR | POLLHUP))) {
                wl_display_cancel_read(display);
                throw std::system_error(ECONNRESET, std::generic_category(), "Wayland connection closed");
            }
            if (result > 0 && (connection.revents & POLLIN)) {
                checkWayland(wl_display_read_events(display), "read Wayland events");
                const int dispatched = wl_display_dispatch_pending(display);
                checkWayland(dispatched, "dispatch Wayland events");
                if (dispatched || closed || !wait || !remaining)
                    return;
                continue;
            }
            const int pollError = errno;
            wl_display_cancel_read(display);
            if (result < 0 && pollError == EINTR)
                continue;
            if (result < 0)
                throw std::system_error(pollError, std::generic_category(), "poll Wayland");
            if (connection.revents & POLLOUT) {
                if (wait)
                    continue;
                flush();
            }
            return;
        }
    }
};

DesktopPresenter::DesktopPresenter(const char *drmNode, const char *outputName)
    : impl_(std::make_unique<Impl>()) {
    impl_->open(drmNode, outputName);
}

DesktopPresenter::~DesktopPresenter() = default;

uint32_t DesktopPresenter::add(int dmaBufFd, uint32_t width, uint32_t height, uint32_t fourcc,
                               uint32_t pitch, uint32_t offset, uint64_t modifier,
                               int acquireTimelineFd, int releaseTimelineFd) {
    auto &self = *impl_;
    if (self.slots.size() >= maxDesktopSlots)
        throw std::logic_error("desktop presenter supports at most four buffers");
    Impl::Slot slot;
    try {
        if (self.closed)
            throw std::runtime_error("Wayland window closed");
        if (dmaBufFd < 0 || acquireTimelineFd < 0 || releaseTimelineFd < 0)
            throw std::runtime_error("missing DMA-BUF, acquire, or release file descriptor");
        if (dmaBufFd == acquireTimelineFd || dmaBufFd == releaseTimelineFd || acquireTimelineFd == releaseTimelineFd)
            throw std::runtime_error("DMA-BUF, acquire, and release require distinct file descriptors");
        if (!width || !height || width > INT32_MAX || height > INT32_MAX || !pitch)
            throw std::runtime_error("invalid Wayland buffer extent or stride");
        slot.acquire = self.importTimeline(acquireTimelineFd, "import acquire DMA-BUF syncobj");
        slot.release = self.importTimeline(releaseTimelineFd, "import release DMA-BUF syncobj", &slot.releaseHandle);
        using Params = std::unique_ptr<zwp_linux_buffer_params_v1, decltype(&zwp_linux_buffer_params_v1_destroy)>;
        Impl::Import result;
        const Params params(requireProxy(zwp_linux_dmabuf_v1_create_params(self.dmabuf)),
                            zwp_linux_buffer_params_v1_destroy);
        requireListener(zwp_linux_buffer_params_v1_add_listener(params.get(), &Impl::importListener, &result));
        zwp_linux_buffer_params_v1_add(params.get(), dmaBufFd, 0, offset, pitch, high(modifier), low(modifier));
        zwp_linux_buffer_params_v1_create(params.get(), width, height, fourcc, 0);
        self.waitFor([&] { return result.done; }, "wait for Wayland DMA-BUF import");
        if (!result.buffer)
            throw std::runtime_error("Wayland compositor rejected the DMA-BUF");
        slot.buffer = std::exchange(result.buffer, nullptr);
        self.slots.push_back(slot);
    } catch (const std::bad_alloc &) {
        self.destroySlot(slot);
        throw;
    } catch (const std::exception &error) {
        self.destroySlot(slot);
        char layout[104];
        std::snprintf(layout, sizeof(layout), "%ux%u fourcc=0x%08x modifier=0x%016llx pitch=%u offset=%u",
                      width, height, fourcc, static_cast<unsigned long long>(modifier), pitch, offset);
        throw std::runtime_error("Desktop buffer " + std::to_string(self.slots.size()) + " (" + layout +
                                 ", local DRM " + self.drmNode + "): " + error.what());
    }
    return uint32_t(self.slots.size() - 1);
}

void DesktopPresenter::presentPoint(uint32_t index, uint64_t point, uint64_t frameId) {
    auto &self = *impl_;
    auto &slot = self.slots.at(index);
    if (self.closed)
        throw std::runtime_error("Wayland window closed");
    if (!slot.buffer || point <= slot.point)
        throw std::logic_error("desktop buffer point did not advance");

    const uint64_t commitNs = nowNs();
    auto &cue = self.pending.emplace_back();
    cue.owner = &self;
    cue.entry = std::prev(self.pending.end());
    cue.feedback.frameId = frameId;
    try {
        cue.proxy = requireProxy(wp_presentation_feedback(self.presentation, self.surface));
        requireListener(wp_presentation_feedback_add_listener(cue.proxy, &Impl::feedbackListener, &cue));
    } catch (...) {
        if (cue.proxy)
            wp_presentation_feedback_destroy(cue.proxy);
        self.pending.pop_back();
        throw;
    }

    if (self.width > 0 && self.height > 0 &&
        (self.width != self.viewportWidth || self.height != self.viewportHeight)) {
        wp_viewport_set_destination(self.viewport, self.width, self.height);
        self.viewportWidth = self.width;
        self.viewportHeight = self.height;
    }
    wp_linux_drm_syncobj_surface_v1_set_acquire_point(self.surfaceSync, slot.acquire, high(point), low(point));
    wp_linux_drm_syncobj_surface_v1_set_release_point(self.surfaceSync, slot.release, high(point), low(point));
    wl_surface_attach(self.surface, slot.buffer, 0, 0);
    wl_surface_damage_buffer(self.surface, 0, 0, INT32_MAX, INT32_MAX);
    cue.feedback.commitNs = commitNs;
    wl_surface_commit(self.surface);
    slot.point = point;
    self.flush();
}

int DesktopPresenter::releasable(const uint32_t *indices, const uint64_t *points,
                                uint32_t count, uint64_t timeoutNs) {
    if (!count)
        return -1;
    auto &self = *impl_;
    std::array<uint32_t, maxDesktopSlots> handles{};
    std::array<uint64_t, maxDesktopSlots> values{};
    if (count > handles.size())
        throw std::logic_error("desktop presenter supports at most four queued buffers");

    for (uint32_t i = 0; i < count; ++i) {
        const auto &slot = self.slots.at(indices[i]);
        if (!points[i])
            return int(i);
        if (points[i] > slot.point)
            throw std::logic_error("release point has not been committed");
        handles[i] = slot.releaseHandle;
        values[i] = points[i];
    }

    const uint64_t start = nowNs();
    const uint64_t deadline = start + std::min(timeoutNs, uint64_t(INT64_MAX) - start);
    constexpr uint64_t closeCheckInterval = 100'000'000;
    while (!self.closed) {
        const auto chunk = std::min(deadline, nowNs() + closeCheckInterval);
        uint32_t selected{};
        const int result = drmSyncobjTimelineWait(self.drm.value, handles.data(), values.data(), count,
                                                  int64_t(chunk), DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
                                                  &selected);
        if (!result)
            return int(selected);
        const int error = result < -1 ? -result : errno;
        if (error == EINTR)
            continue;
        if (error != ETIME && error != ETIMEDOUT)
            checkDrm(result, "find a materialized desktop buffer release fence");
        if (chunk == deadline)
            break;
        self.dispatch(false);
    }
    return -1;
}

void DesktopPresenter::dispatch(bool wait, uint64_t timeoutNs) { impl_->dispatch(wait, timeoutNs); }

std::vector<DesktopFeedback> DesktopPresenter::takeFeedback() {
    impl_->dispatch(false);
    return std::exchange(impl_->completed, {});
}

bool DesktopPresenter::closeRequested() const { return impl_->closed; }
