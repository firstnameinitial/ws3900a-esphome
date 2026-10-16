// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <atomic>
#include <cstdlib>
#include <memory>

namespace esphome::ecowitt_rf {

// Shared by the web task and main loop. A timed-out caller can release its reference
// while the loop is still building; the final owner frees the immutable payload.
struct StatusRequest {
    enum State { PENDING, READY, FAILED, CANCELLED };
    explicit StatusRequest(bool compact) : compact(compact) {}
    bool compact;
    std::atomic<State> state{PENDING};
    std::unique_ptr<char, decltype(&std::free)> body{nullptr, &std::free};
    size_t length = 0;

    void complete(char *data, size_t size) {
        body.reset(data); length = size;
        State expected = PENDING;
        state.compare_exchange_strong(expected, data ? READY : FAILED, std::memory_order_release);
    }
    void cancel() {
        State expected = PENDING;
        state.compare_exchange_strong(expected, CANCELLED, std::memory_order_acq_rel);
    }
    State status() const { return state.load(std::memory_order_acquire); }
};

}  // namespace esphome::ecowitt_rf
