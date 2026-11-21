// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vulkan/vulkan.h>

namespace v3 {

class DisplayEvent;

class DisplayEventNotifier {
public:
    uint64_t generation() const {
        const std::lock_guard lock(mutex);
        return sequence;
    }

    bool waitAfter(uint64_t observed, uint64_t timeoutNs) {
        return waitUntil(observed, deadline(timeoutNs));
    }

    void notify() noexcept {
        {
            const std::lock_guard lock(mutex);
            ++sequence;
        }
        changed.notify_all();
    }

private:
    friend class DisplayEvent;
    using Clock = std::chrono::steady_clock;
    using Nanoseconds = std::chrono::nanoseconds;
    mutable std::mutex mutex;
    std::condition_variable changed;
    uint64_t sequence{};

    static Clock::time_point deadline(uint64_t timeoutNs) {
        if (timeoutNs == UINT64_MAX)
            return Clock::time_point::max();
        const auto now = Clock::now();
        const auto maxWait = std::chrono::duration_cast<Nanoseconds>(Clock::time_point::max() - now).count();
        return now + Nanoseconds(std::min(timeoutNs, uint64_t(maxWait)));
    }

    bool waitUntil(uint64_t observed, Clock::time_point limit) {
        std::unique_lock lock(mutex);
        const auto ready = [&] { return sequence != observed; };
        if (limit == Clock::time_point::max()) {
            changed.wait(lock, ready);
            return true;
        }
        return changed.wait_until(lock, limit, ready);
    }
};

// A DRM sequence event changes only host state. Queue submission and Vulkan
// external handle interop are independent of this notification.
class DisplayEvent {
public:
    explicit DisplayEvent(std::shared_ptr<DisplayEventNotifier> notifier)
        : changed(std::move(notifier)) {}

    VkResult status() const noexcept {
        return result.load(std::memory_order_acquire);
    }

    VkResult waitNs(uint64_t timeoutNs) const {
        const auto deadline = DisplayEventNotifier::deadline(timeoutNs);
        for (;;) {
            const auto observed = changed->generation();
            const auto value = status();
            if (value != VK_NOT_READY)
                return value;
            if (!timeoutNs || !changed->waitUntil(observed, deadline) ||
                (deadline != DisplayEventNotifier::Clock::time_point::max() &&
                 DisplayEventNotifier::Clock::now() >= deadline)) {
                const auto final = status();
                return final == VK_NOT_READY ? VK_TIMEOUT : final;
            }
        }
    }

    void signal() noexcept { finish(VK_SUCCESS); }
    void cancel() noexcept { finish(VK_ERROR_UNKNOWN); }

private:
    std::shared_ptr<DisplayEventNotifier> changed;
    std::atomic<VkResult> result{VK_NOT_READY};

    void finish(VkResult value) noexcept {
        auto pending = VK_NOT_READY;
        if (result.compare_exchange_strong(pending, value, std::memory_order_release,
                                           std::memory_order_relaxed))
            changed->notify();
    }
};

} // namespace v3
