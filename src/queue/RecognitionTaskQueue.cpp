#include "RecognitionTaskQueue.h"

#include <condition_variable>
#include <list>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ocrservice::queue {
namespace detail {

struct StartGateState final {
    bool open = false;
};

struct QueuedTask final {
    std::optional<domain::RecognitionTask> task;
    std::shared_ptr<StartGateState> gate;
};

struct QueueSharedState final {
    explicit QueueSharedState(const std::size_t configuredCapacity)
        : capacity(configuredCapacity) {}

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::list<std::shared_ptr<QueuedTask>> tasks;
    std::size_t reserved = 0U;
    const std::size_t capacity;
    bool accepting = true;
    bool stopRequested = false;
};

}  // namespace detail

TaskStartGate::TaskStartGate(
    std::shared_ptr<detail::QueueSharedState> queueState,
    std::shared_ptr<detail::StartGateState> gateState)
    : queueState_(std::move(queueState)), gateState_(std::move(gateState)) {}

TaskStartGate::TaskStartGate(TaskStartGate&& other) noexcept = default;
TaskStartGate& TaskStartGate::operator=(TaskStartGate&& other) noexcept {
    if (this != &other) {
        openNoThrow();
        queueState_ = std::move(other.queueState_);
        gateState_ = std::move(other.gateState_);
    }
    return *this;
}

TaskStartGate::~TaskStartGate() {
    openNoThrow();
}

void TaskStartGate::open() {
    if (!valid()) {
        throw std::logic_error("start gate is not valid");
    }
    {
        std::lock_guard<std::mutex> lock(queueState_->mutex);
        gateState_->open = true;
    }
    queueState_->changed.notify_all();
}

bool TaskStartGate::isOpen() const {
    if (!valid()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(queueState_->mutex);
    return gateState_->open;
}

bool TaskStartGate::valid() const noexcept {
    return queueState_ != nullptr && gateState_ != nullptr;
}

void TaskStartGate::openNoThrow() noexcept {
    if (!valid()) {
        return;
    }
    try {
        open();
    } catch (...) {
        // Destructors cannot report synchronization failures.
    }
}

QueueReservation::QueueReservation(
    std::shared_ptr<detail::QueueSharedState> state,
    std::list<std::shared_ptr<detail::QueuedTask>> pendingNode)
    : state_(std::move(state)), pendingNode_(std::move(pendingNode)) {}

QueueReservation::QueueReservation(QueueReservation&& other) noexcept
    : state_(std::move(other.state_)),
      pendingNode_(std::move(other.pendingNode_)),
      active_(other.active_) {
    other.active_ = false;
}

QueueReservation& QueueReservation::operator=(QueueReservation&& other) {
    if (this != &other) {
        cancel();
        state_ = std::move(other.state_);
        pendingNode_ = std::move(other.pendingNode_);
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

QueueReservation::~QueueReservation() {
    cancel();
}

TaskStartGate QueueReservation::commit(domain::RecognitionTask task) {
    if (!active_ || state_ == nullptr || pendingNode_.size() != 1U) {
        throw std::logic_error("queue reservation is not active");
    }
    static_assert(std::is_nothrow_move_constructible_v<domain::RecognitionTask>);
    const auto slot = pendingNode_.front();
    const auto gate = slot->gate;
    slot->task.emplace(std::move(task));
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->reserved == 0U) {
            throw std::logic_error("queue reservation count is inconsistent");
        }
        if (!state_->stopRequested) {
            state_->tasks.splice(state_->tasks.end(), pendingNode_);
        }
        --state_->reserved;
        active_ = false;
    }
    state_->changed.notify_all();
    pendingNode_.clear();
    return TaskStartGate(state_, gate);
}

void QueueReservation::cancel() {
    if (!active_ || state_ == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->reserved != 0U) {
            --state_->reserved;
        }
        active_ = false;
    }
    pendingNode_.clear();
    state_->changed.notify_all();
}

bool QueueReservation::active() const noexcept {
    return active_;
}

RecognitionTaskQueue::RecognitionTaskQueue(const std::size_t capacity)
    : state_(std::make_shared<detail::QueueSharedState>(capacity)) {}

RecognitionTaskQueue::~RecognitionTaskQueue() {
    requestStop();
}

std::optional<QueueReservation> RecognitionTaskQueue::tryReserve() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->accepting || state_->stopRequested ||
        state_->tasks.size() + state_->reserved >= state_->capacity) {
        return std::nullopt;
    }
    auto slot = std::make_shared<detail::QueuedTask>();
    slot->gate = std::make_shared<detail::StartGateState>();
    std::list<std::shared_ptr<detail::QueuedTask>> pendingNode;
    pendingNode.push_back(std::move(slot));
    ++state_->reserved;
    return QueueReservation(state_, std::move(pendingNode));
}

std::optional<domain::RecognitionTask> RecognitionTaskQueue::take() {
    const auto state = state_;
    std::unique_lock<std::mutex> lock(state->mutex);
    state->changed.wait(lock, [&state] {
        return state->stopRequested ||
               (!state->tasks.empty() && state->tasks.front()->gate->open);
    });
    if (state->stopRequested) {
        return std::nullopt;
    }
    auto task = std::move(*state->tasks.front()->task);
    state->tasks.pop_front();
    lock.unlock();
    state->changed.notify_all();
    return task;
}

void RecognitionTaskQueue::stopAccepting() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->accepting = false;
}

void RecognitionTaskQueue::requestStop() {
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->accepting = false;
        state_->stopRequested = true;
        state_->tasks.clear();
    }
    state_->changed.notify_all();
}

std::size_t RecognitionTaskQueue::queueDepth() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->tasks.size();
}

std::size_t RecognitionTaskQueue::reservedCount() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->reserved;
}

std::size_t RecognitionTaskQueue::capacity() const noexcept {
    return state_->capacity;
}

bool RecognitionTaskQueue::isAccepting() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->accepting;
}

bool RecognitionTaskQueue::isStopRequested() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stopRequested;
}

}  // namespace ocrservice::queue
