#pragma once
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstdint>
#include <limits>

// One budget covers acquiring the real image and awaiting its acquire fence.
// A successful internal acquire is retained if the public call times out while
// waiting for that fence. The next call resumes it instead of reusing a live fence.
template<class Clock = std::chrono::steady_clock>
class AcquireState {
    bool pending_ = false;
    uint32_t index_ = 0;
    VkResult status_ = VK_SUCCESS;
public:
    bool pending() const { return pending_; }
    template<class Acquire, class Wait, class Reset>
    VkResult next(uint64_t timeout, uint32_t* index, Acquire acquire, Wait wait, Reset reset) {
        constexpr auto infinite = std::numeric_limits<uint64_t>::max();
        const auto start = timeout == infinite || timeout == 0
            ? typename Clock::time_point{} : Clock::now();
        auto remaining = [&]() -> uint64_t {
            if (timeout == infinite || timeout == 0) return timeout;
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count();
            const auto used = elapsed > 0 ? uint64_t(elapsed) : 0;
            return used < timeout ? timeout-used : 0;
        };
        if (!pending_) {
            status_ = acquire(remaining(), &index_);
            if (status_ != VK_SUCCESS && status_ != VK_SUBOPTIMAL_KHR) return status_;
            pending_ = true;
        }
        const auto ready = wait(remaining());
        if (ready != VK_SUCCESS) return ready;
        const auto cleared = reset();
        if (cleared != VK_SUCCESS) return cleared;
        pending_ = false;
        *index = index_;
        return status_;
    }
};
