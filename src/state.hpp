// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace minimal {
// Independent of Vulkan: only a completed flip makes a displayed image reusable.
class BufferStates {
  public:
    enum class State { Free, Acquired, Pending, Displayed };
    explicit BufferStates(unsigned count) : states_(count, State::Free) {}
    int acquire() {
        for (unsigned i = 0; i < states_.size(); ++i)
            if (states_[i] == State::Free) {
                states_[i] = State::Acquired;
                return static_cast<int>(i);
            }
        return -1;
    }
    bool acquired(unsigned i) const {
        return i < states_.size() && states_[i] == State::Acquired;
    }
    void cancel(unsigned i) {
        require(acquired(i));
        states_[i] = State::Free;
    }
    bool pending() const {
        return pending_ >= 0;
    }
    void queue(unsigned i, uint64_t id) {
        require(!pending() && acquired(i) && id > completed_);
        states_[i] = State::Pending;
        pending_ = static_cast<int>(i);
        pendingId_ = id;
    }
    void flip() {
        require(pending());
        if (displayed_ >= 0)
            states_[displayed_] = State::Free;
        displayed_ = pending_;
        states_[displayed_] = State::Displayed;
        completed_ = pendingId_;
        pending_ = -1;
    }
    uint64_t completed() const {
        return completed_;
    }

  private:
    static void require(bool valid) {
        if (!valid)
            throw std::logic_error("Invalid presentation lifecycle");
    }
    std::vector<State> states_;
    int pending_ = -1, displayed_ = -1;
    uint64_t pendingId_ = 0, completed_ = 0;
};
} // namespace minimal
