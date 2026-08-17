#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "RecognitionTaskQueue.h"

namespace {

using namespace std::chrono_literals;
using ocrservice::domain::RecognitionId;
using ocrservice::domain::RecognitionTask;
using ocrservice::domain::Uuid;
using ocrservice::queue::RecognitionTaskQueue;

RecognitionTask task(const std::uint64_t sequence) {
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0U; index < sizeof(sequence); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        bytes[15U - index] = static_cast<std::uint8_t>((sequence >> shift) & 0xFFU);
    }
    return RecognitionTask(RecognitionId(Uuid::v4(bytes)));
}

std::string idOf(const RecognitionTask& value) {
    return value.recognitionId().toString();
}

template <typename Predicate>
bool waitUntilFor(const std::chrono::steady_clock::duration timeout, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

TEST(RecognitionTaskQueueTest, ExposesTeachingDefaultsAndConfigurableCapacityBoundaries) {
    EXPECT_EQ(ocrservice::queue::kDefaultRecognitionWorkerCount, 1U);
    EXPECT_EQ(ocrservice::queue::kDefaultRecognitionQueueCapacity, 20U);

    RecognitionTaskQueue defaults;
    EXPECT_EQ(defaults.capacity(), 20U);

    RecognitionTaskQueue zero(0U);
    EXPECT_FALSE(zero.tryReserve().has_value());
    EXPECT_EQ(zero.queueDepth(), 0U);
    EXPECT_EQ(zero.reservedCount(), 0U);

    RecognitionTaskQueue maximum(std::numeric_limits<std::size_t>::max());
    auto first = maximum.tryReserve();
    auto second = maximum.tryReserve();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(maximum.reservedCount(), 2U);
}

TEST(RecognitionTaskQueueTest, CountsReservationsAgainstCapacityAndReportsFullImmediately) {
    RecognitionTaskQueue queue(1U);
    auto reservation = queue.tryReserve();
    ASSERT_TRUE(reservation.has_value());
    EXPECT_EQ(queue.reservedCount(), 1U);
    EXPECT_EQ(queue.queueDepth(), 0U);
    EXPECT_FALSE(queue.tryReserve().has_value());

    auto gate = reservation->commit(task(1U));
    EXPECT_EQ(queue.reservedCount(), 0U);
    EXPECT_EQ(queue.queueDepth(), 1U);
    EXPECT_FALSE(queue.tryReserve().has_value());
    gate.open();
    const auto taken = queue.take();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(idOf(*taken), idOf(task(1U)));
    EXPECT_TRUE(queue.tryReserve().has_value());
}

TEST(RecognitionTaskQueueTest, ExplicitAndAutomaticCancellationReturnSlots) {
    RecognitionTaskQueue queue(1U);
    {
        auto reservation = queue.tryReserve();
        ASSERT_TRUE(reservation.has_value());
        reservation->cancel();
        EXPECT_FALSE(reservation->active());
        EXPECT_EQ(queue.reservedCount(), 0U);
        reservation->cancel();
    }
    {
        auto reservation = queue.tryReserve();
        ASSERT_TRUE(reservation.has_value());
        EXPECT_EQ(queue.reservedCount(), 1U);
    }
    EXPECT_EQ(queue.reservedCount(), 0U);
    EXPECT_TRUE(queue.tryReserve().has_value());
}

TEST(RecognitionTaskQueueTest, StartGatePreventsConsumptionUntilExplicitlyOpened) {
    RecognitionTaskQueue queue(1U);
    auto reservation = queue.tryReserve();
    ASSERT_TRUE(reservation.has_value());
    auto gate = reservation->commit(task(2U));
    EXPECT_TRUE(gate.valid());
    EXPECT_FALSE(gate.isOpen());

    auto consumer = std::async(std::launch::async, [&queue] { return queue.take(); });
    EXPECT_EQ(consumer.wait_for(50ms), std::future_status::timeout);
    EXPECT_EQ(queue.queueDepth(), 1U);
    gate.open();
    gate.open();
    ASSERT_EQ(consumer.wait_for(2s), std::future_status::ready);
    const auto taken = consumer.get();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(idOf(*taken), idOf(task(2U)));
    EXPECT_EQ(queue.queueDepth(), 0U);
}

TEST(RecognitionTaskQueueTest, StartGateScopeExitCannotLeaveAnOrphanedTask) {
    RecognitionTaskQueue queue(1U);
    auto reservation = queue.tryReserve();
    ASSERT_TRUE(reservation.has_value());
    {
        auto gate = reservation->commit(task(3U));
        EXPECT_FALSE(gate.isOpen());
    }
    const auto taken = queue.take();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(idOf(*taken), idOf(task(3U)));
}

TEST(RecognitionTaskQueueTest, PreservesFifoWhenLaterGateOpensFirst) {
    RecognitionTaskQueue queue(2U);
    auto firstReservation = queue.tryReserve();
    auto secondReservation = queue.tryReserve();
    ASSERT_TRUE(firstReservation.has_value());
    ASSERT_TRUE(secondReservation.has_value());
    auto firstGate = firstReservation->commit(task(10U));
    auto secondGate = secondReservation->commit(task(11U));
    secondGate.open();

    auto consumer = std::async(std::launch::async, [&queue] { return queue.take(); });
    EXPECT_EQ(consumer.wait_for(50ms), std::future_status::timeout);
    firstGate.open();
    ASSERT_EQ(consumer.wait_for(2s), std::future_status::ready);
    const auto firstTaken = consumer.get();
    ASSERT_TRUE(firstTaken.has_value());
    EXPECT_EQ(idOf(*firstTaken), idOf(task(10U)));
    const auto secondTaken = queue.take();
    ASSERT_TRUE(secondTaken.has_value());
    EXPECT_EQ(idOf(*secondTaken), idOf(task(11U)));
}

TEST(RecognitionTaskQueueTest, RejectsDuplicateCommitWithoutLeakingCapacity) {
    RecognitionTaskQueue queue(1U);
    auto reservation = queue.tryReserve();
    ASSERT_TRUE(reservation.has_value());
    auto gate = reservation->commit(task(20U));
    EXPECT_FALSE(reservation->active());
    EXPECT_THROW(reservation->commit(task(21U)), std::logic_error);
    EXPECT_EQ(queue.queueDepth(), 1U);
    EXPECT_EQ(queue.reservedCount(), 0U);
    gate.open();
    EXPECT_TRUE(queue.take().has_value());
}

TEST(RecognitionTaskQueueTest, StopAcceptingRejectsNewReservationsButHonorsExistingOnes) {
    RecognitionTaskQueue queue(1U);
    auto reservation = queue.tryReserve();
    ASSERT_TRUE(reservation.has_value());
    queue.stopAccepting();
    EXPECT_FALSE(queue.isAccepting());
    EXPECT_FALSE(queue.tryReserve().has_value());

    auto gate = reservation->commit(task(30U));
    gate.open();
    const auto taken = queue.take();
    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(idOf(*taken), idOf(task(30U)));
}

TEST(RecognitionTaskQueueTest, StopWakesConsumersAndDropsEveryUnstartedOpportunity) {
    RecognitionTaskQueue queue(2U);
    auto reservation = queue.tryReserve();
    ASSERT_TRUE(reservation.has_value());
    auto closedGate = reservation->commit(task(40U));
    auto consumer = std::async(std::launch::async, [&queue] { return queue.take(); });
    EXPECT_EQ(consumer.wait_for(50ms), std::future_status::timeout);

    queue.requestStop();
    ASSERT_EQ(consumer.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(consumer.get().has_value());
    EXPECT_TRUE(queue.isStopRequested());
    EXPECT_FALSE(queue.isAccepting());
    EXPECT_EQ(queue.queueDepth(), 0U);
    EXPECT_FALSE(queue.tryReserve().has_value());
    closedGate.open();
    EXPECT_FALSE(queue.take().has_value());
}

TEST(RecognitionTaskQueueTest, ReservationCommittedAfterStopIsReleasedButNeverExecuted) {
    RecognitionTaskQueue queue(1U);
    auto reservation = queue.tryReserve();
    ASSERT_TRUE(reservation.has_value());
    queue.requestStop();
    auto gate = reservation->commit(task(50U));
    EXPECT_EQ(queue.reservedCount(), 0U);
    EXPECT_EQ(queue.queueDepth(), 0U);
    gate.open();
    EXPECT_FALSE(queue.take().has_value());
}

TEST(RecognitionTaskQueueTest, StopDoesNotRevokeATaskAlreadyReturnedToWorker) {
    RecognitionTaskQueue queue(2U);
    auto firstReservation = queue.tryReserve();
    auto secondReservation = queue.tryReserve();
    ASSERT_TRUE(firstReservation.has_value());
    ASSERT_TRUE(secondReservation.has_value());
    auto firstGate = firstReservation->commit(task(60U));
    auto secondGate = secondReservation->commit(task(61U));
    firstGate.open();
    secondGate.open();

    const auto current = queue.take();
    ASSERT_TRUE(current.has_value());
    queue.requestStop();
    EXPECT_EQ(idOf(*current), idOf(task(60U)));
    EXPECT_EQ(queue.queueDepth(), 0U);
    EXPECT_FALSE(queue.take().has_value());
}

TEST(RecognitionTaskQueueTest, ConcurrentReservationsNeverExceedCapacity) {
    constexpr std::size_t kCapacity = 8U;
    constexpr std::size_t kThreadCount = 64U;
    RecognitionTaskQueue queue(kCapacity);
    std::atomic<bool> start{false};
    std::atomic<std::size_t> successCount{0U};
    std::mutex heldMutex;
    std::vector<ocrservice::queue::QueueReservation> held;
    held.reserve(kCapacity);
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t index = 0U; index < kThreadCount; ++index) {
        threads.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto reservation = queue.tryReserve();
            if (reservation.has_value()) {
                successCount.fetch_add(1U, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(heldMutex);
                held.push_back(std::move(*reservation));
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(successCount.load(), kCapacity);
    EXPECT_EQ(queue.reservedCount(), kCapacity);
    EXPECT_FALSE(queue.tryReserve().has_value());
    held.clear();
    EXPECT_EQ(queue.reservedCount(), 0U);
}

TEST(RecognitionTaskQueueTest, ConcurrentProducersAndConsumersCompleteWithoutLossOrDeadlock) {
    constexpr std::size_t kTaskCount = 500U;
    constexpr std::size_t kProducerCount = 4U;
    constexpr std::size_t kConsumerCount = 4U;
    RecognitionTaskQueue queue(7U);
    std::atomic<std::size_t> nextSequence{1U};
    std::atomic<std::size_t> consumedCount{0U};
    std::atomic<bool> abort{false};
    std::atomic<std::size_t> producersFinished{0U};
    std::mutex consumedMutex;
    std::set<std::string> consumedIds;

    std::vector<std::thread> consumers;
    consumers.reserve(kConsumerCount);
    for (std::size_t index = 0U; index < kConsumerCount; ++index) {
        consumers.emplace_back([&] {
            while (const auto item = queue.take()) {
                {
                    std::lock_guard<std::mutex> lock(consumedMutex);
                    consumedIds.insert(idOf(*item));
                }
                consumedCount.fetch_add(1U, std::memory_order_release);
            }
        });
    }

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (std::size_t index = 0U; index < kProducerCount; ++index) {
        producers.emplace_back([&] {
            while (!abort.load(std::memory_order_acquire)) {
                const auto sequence = nextSequence.fetch_add(1U, std::memory_order_relaxed);
                if (sequence > kTaskCount) {
                    break;
                }
                while (!abort.load(std::memory_order_acquire)) {
                    auto reservation = queue.tryReserve();
                    if (reservation.has_value()) {
                        auto gate = reservation->commit(task(sequence));
                        gate.open();
                        break;
                    }
                    std::this_thread::yield();
                }
            }
            producersFinished.fetch_add(1U, std::memory_order_release);
        });
    }
    const bool producersCompleted = waitUntilFor(5s, [&] {
        return producersFinished.load(std::memory_order_acquire) == kProducerCount;
    });
    if (!producersCompleted) {
        abort.store(true, std::memory_order_release);
        queue.requestStop();
    }
    for (auto& thread : producers) {
        thread.join();
    }

    bool completed = false;
    if (producersCompleted) {
        completed = waitUntilFor(5s, [&] {
            return consumedCount.load(std::memory_order_acquire) == kTaskCount;
        });
    }
    if (!completed) {
        abort.store(true, std::memory_order_release);
        queue.requestStop();
    }
    queue.requestStop();
    for (auto& thread : consumers) {
        thread.join();
    }

    EXPECT_TRUE(producersCompleted);
    EXPECT_TRUE(completed);
    EXPECT_EQ(consumedCount.load(), kTaskCount);
    EXPECT_EQ(consumedIds.size(), kTaskCount);
    EXPECT_EQ(queue.queueDepth(), 0U);
    EXPECT_EQ(queue.reservedCount(), 0U);
}

}  // namespace
