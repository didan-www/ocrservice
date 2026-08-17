#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "Recognition.h"

namespace ocrservice::queue {

constexpr std::size_t kDefaultRecognitionWorkerCount = 1U;
constexpr std::size_t kDefaultRecognitionQueueCapacity = 20U;

namespace detail {
struct QueueSharedState;
struct StartGateState;
}  // namespace detail

class TaskStartGate final {
public:
    TaskStartGate(TaskStartGate&& other) noexcept;
    TaskStartGate& operator=(TaskStartGate&& other) noexcept;
    ~TaskStartGate();

    TaskStartGate(const TaskStartGate&) = delete;
    TaskStartGate& operator=(const TaskStartGate&) = delete;

    void open();
    bool isOpen() const;
    bool valid() const noexcept;

private:
    friend class QueueReservation;

    TaskStartGate(
        std::shared_ptr<detail::QueueSharedState> queueState,
        std::shared_ptr<detail::StartGateState> gateState);
    void openNoThrow() noexcept;

    std::shared_ptr<detail::QueueSharedState> queueState_;
    std::shared_ptr<detail::StartGateState> gateState_;
};

class QueueReservation final {
public:
    QueueReservation(QueueReservation&& other) noexcept;
    QueueReservation& operator=(QueueReservation&& other);
    ~QueueReservation();

    QueueReservation(const QueueReservation&) = delete;
    QueueReservation& operator=(const QueueReservation&) = delete;

    TaskStartGate commit(domain::RecognitionTask task);
    void cancel();
    bool active() const noexcept;

private:
    friend class RecognitionTaskQueue;

    explicit QueueReservation(std::shared_ptr<detail::QueueSharedState> state);

    std::shared_ptr<detail::QueueSharedState> state_;
    bool active_ = true;
};

class RecognitionTaskQueue final {
public:
    explicit RecognitionTaskQueue(
        std::size_t capacity = kDefaultRecognitionQueueCapacity);
    ~RecognitionTaskQueue();

    RecognitionTaskQueue(const RecognitionTaskQueue&) = delete;
    RecognitionTaskQueue& operator=(const RecognitionTaskQueue&) = delete;
    RecognitionTaskQueue(RecognitionTaskQueue&&) = delete;
    RecognitionTaskQueue& operator=(RecognitionTaskQueue&&) = delete;

    std::optional<QueueReservation> tryReserve();

    // Blocks until the oldest committed task is armed or stop is requested.
    // A null result permanently ends the calling worker loop.
    std::optional<domain::RecognitionTask> take();

    void stopAccepting();
    void requestStop();

    std::size_t queueDepth() const;
    std::size_t reservedCount() const;
    std::size_t capacity() const noexcept;
    bool isAccepting() const;
    bool isStopRequested() const;

private:
    std::shared_ptr<detail::QueueSharedState> state_;
};

}  // namespace ocrservice::queue
