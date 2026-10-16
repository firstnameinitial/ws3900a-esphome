// SPDX-License-Identifier: GPL-3.0-only
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include "status_request.h"
using namespace esphome::ecowitt_rf;
static char *payload() { char *p = (char *)std::malloc(3); std::memcpy(p, "{}", 3); return p; }
int main() {
    auto ready = std::make_shared<StatusRequest>(false);
    std::thread producer([ready] { ready->complete(payload(), 2); });
    while (ready->status() == StatusRequest::PENDING) std::this_thread::yield();
    assert(ready->status() == StatusRequest::READY && ready->length == 2);
    assert(std::strcmp(ready->body.get(), "{}") == 0);
    ready->cancel(); assert(ready->status() == StatusRequest::READY); producer.join();
    auto failed = std::make_shared<StatusRequest>(true); failed->complete(nullptr, 0);
    assert(failed->status() == StatusRequest::FAILED);
    // Caller times out/releases while main loop still owns an in-flight response.
    auto caller = std::make_shared<StatusRequest>(false);
    std::weak_ptr<StatusRequest> lifetime = caller;
    std::atomic<bool> build{false};
    std::thread slow([worker = caller, &build] {
        while (!build.load()) std::this_thread::yield();
        worker->complete(payload(), 2);
        assert(worker->status() == StatusRequest::CANCELLED);
    });
    caller->cancel(); caller.reset(); assert(!lifetime.expired());
    build.store(true); slow.join(); assert(lifetime.expired()); // ASAN checks the final free
    std::puts("Status publication, timeout cancellation and cross-task buffer ownership passed");
}
