// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vk_layer.h>
namespace minimal {
struct Error : std::runtime_error {
    VkResult result;
    Error(VkResult r, const char *reason) : std::runtime_error(reason), result(r) {}
};
inline void check(VkResult r, const char *reason) {
    if (r != VK_SUCCESS)
        throw Error(r, reason);
}
inline void note(const char *message) {
    std::fprintf(stderr, "muxless-minimal: %s\n", message);
}
class Fd {
    int fd_ = -1;

  public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() {
        if (fd_ >= 0)
            close(fd_);
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&x) noexcept : fd_(x.release()) {}
    Fd &operator=(Fd &&other) noexcept {
        if (this != &other) {
            if (fd_ >= 0)
                close(fd_);
            fd_ = other.release();
        }
        return *this;
    }
    int get() const {
        return fd_;
    }
    int release() {
        int fd = fd_;
        fd_ = -1;
        return fd;
    }
};
template <class T> void *dispatchKey(T object) {
    return object ? *reinterpret_cast<void **>(object) : nullptr;
}
template <class Handle, class Object> Handle opaque(Object *object) {
    return reinterpret_cast<Handle>(object);
}
template <class T> VkResult enumerate(const std::vector<T> &source, uint32_t *count, T *out) {
    if (!out) {
        *count = static_cast<uint32_t>(source.size());
        return VK_SUCCESS;
    }
    const auto n = std::min(*count, static_cast<uint32_t>(source.size()));
    std::copy_n(source.begin(), n, out);
    *count = n;
    return n < source.size() ? VK_INCOMPLETE : VK_SUCCESS;
}
using Clock = std::chrono::steady_clock;
inline Clock::time_point deadline(uint64_t ns) {
    if (ns == UINT64_MAX)
        return Clock::time_point::max();
    const auto now = Clock::now();
    const auto room =
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::time_point::max() - now)
            .count();
    return ns >= static_cast<uint64_t>(room) ? Clock::time_point::max()
                                             : now + std::chrono::nanoseconds(ns);
}
struct Display;
struct Instance {
    VkInstance handle{};
    PFN_vkGetInstanceProcAddr next{};
    bool enabled = false;
    std::shared_ptr<Display> display;
    VkPhysicalDevice renderPhysical{};
    template <class T> T fn(const char *name) const {
        return reinterpret_cast<T>(next(handle, name));
    }
};
#define DEVICE_FUNCTIONS(X)                                                                        \
    X(DestroyDevice)                                                                               \
    X(GetDeviceQueue)                                                                              \
    X(GetDeviceQueue2)                                                                             \
    X(CreateImage)                                                                                 \
    X(DestroyImage)                                                                                \
    X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory)               \
        X(GetImageSubresourceLayout) X(GetMemoryFdKHR) X(CreateCommandPool) X(DestroyCommandPool)  \
            X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)                    \
                X(ResetCommandBuffer) X(CmdCopyImage) X(CmdPipelineBarrier) X(CreateSemaphore) X(DestroySemaphore) \
                    X(GetSemaphoreFdKHR) X(CreateFence) X(DestroyFence) X(GetFenceStatus)          \
                        X(WaitForFences) X(ResetFences) X(QueueSubmit) X(DeviceWaitIdle)
struct Device {
    VkDevice handle{};
    VkPhysicalDevice physical{};
    std::shared_ptr<Instance> instance;
    PFN_vkGetDeviceProcAddr next{};
#define MEMBER(name) PFN_vk##name name{};
    DEVICE_FUNCTIONS(MEMBER)
#undef MEMBER
    std::unordered_map<VkQueue, uint32_t> families;
    std::vector<uint32_t> enabledFamilies;
    VkQueue acquisitionQueue{};
    uint32_t acquisitionFamily{};
    std::mutex acquisitionMutex;
    PFN_vkSetDeviceLoaderData setLoaderData{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::shared_ptr<void> performanceLease;
    template <class T> T fn(const char *name) const {
        return reinterpret_cast<T>(next(handle, name));
    }
    void load() {
#define LOAD(name) name = fn<PFN_vk##name>("vk" #name);
        DEVICE_FUNCTIONS(LOAD)
#undef LOAD
    }
};
inline std::mutex registryMutex;
inline std::unordered_map<void *, std::shared_ptr<Instance>> instances;
inline std::unordered_map<void *, std::shared_ptr<Device>> devices;
template <class T> std::shared_ptr<Instance> instanceFor(T h) {
    std::lock_guard<std::mutex> g(registryMutex);
    auto p = instances.find(dispatchKey(h));
    return p == instances.end() ? nullptr : p->second;
}
template <class T> std::shared_ptr<Device> deviceFor(T h) {
    std::lock_guard<std::mutex> g(registryMutex);
    auto p = devices.find(dispatchKey(h));
    return p == devices.end() ? nullptr : p->second;
}
inline VkResult report(const std::exception &e) {
    note(e.what());
    auto *error = dynamic_cast<const Error *>(&e);
    return error ? error->result : VK_ERROR_INITIALIZATION_FAILED;
}
} // namespace minimal
