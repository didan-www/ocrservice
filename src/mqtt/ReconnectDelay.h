#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <random>

namespace ocrservice::mqtt {

class IReconnectDelay {
public:
    virtual ~IReconnectDelay() = default;
    virtual std::chrono::milliseconds jittered(
        std::chrono::milliseconds baseDelay) = 0;
    virtual bool waitFor(std::chrono::milliseconds delay) = 0;
    virtual void interrupt() noexcept = 0;
};

class SystemReconnectDelay final : public IReconnectDelay {
public:
    SystemReconnectDelay();

    std::chrono::milliseconds jittered(
        std::chrono::milliseconds baseDelay) override;
    bool waitFor(std::chrono::milliseconds delay) override;
    void interrupt() noexcept override;

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool interrupted_ = false;
    std::mt19937_64 random_;
};

}  // namespace ocrservice::mqtt
