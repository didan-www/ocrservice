#include "ReconnectDelay.h"

#include <algorithm>

namespace ocrservice::mqtt {

SystemReconnectDelay::SystemReconnectDelay() : random_(std::random_device{}()) {}

std::chrono::milliseconds SystemReconnectDelay::jittered(
    const std::chrono::milliseconds baseDelay) {
    const auto base = baseDelay.count();
    const auto spread = base / 5;
    std::uniform_int_distribution<std::int64_t> distribution(-spread, spread);
    return std::chrono::milliseconds(base + distribution(random_));
}

bool SystemReconnectDelay::waitFor(const std::chrono::milliseconds delay) {
    std::unique_lock<std::mutex> lock(mutex_);
    return !condition_.wait_for(lock, delay, [this] { return interrupted_; });
}

void SystemReconnectDelay::interrupt() noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupted_ = true;
    }
    condition_.notify_all();
}

}  // namespace ocrservice::mqtt
