#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <spdlog/logger.h>
#include <spdlog/sinks/ostream_sink.h>

#include "LoggerFactory.h"
#include "RecognitionWorker.h"

namespace {

using namespace std::chrono_literals;
using ocrservice::domain::AccessListFilter;
using ocrservice::domain::AccessListInsertResult;
using ocrservice::domain::AccessListRecord;
using ocrservice::domain::BgrImageView;
using ocrservice::domain::CaptureId;
using ocrservice::domain::DeviceId;
using ocrservice::domain::FinalizeRecognition;
using ocrservice::domain::GateAction;
using ocrservice::domain::HistoryCursorResult;
using ocrservice::domain::HistoryFilter;
using ocrservice::domain::HistoryVisitor;
using ocrservice::domain::IHistoryCursor;
using ocrservice::domain::ImageFile;
using ocrservice::domain::ImageMime;
using ocrservice::domain::ImageReader;
using ocrservice::domain::ModelFailureCode;
using ocrservice::domain::NewAccessListRecord;
using ocrservice::domain::NewRecognition;
using ocrservice::domain::PageRequest;
using ocrservice::domain::PageResult;
using ocrservice::domain::PlateNumber;
using ocrservice::domain::PublishAttempt;
using ocrservice::domain::PublishFailure;
using ocrservice::domain::RecognitionFailureCode;
using ocrservice::domain::RecognitionId;
using ocrservice::domain::RecognitionOutcome;
using ocrservice::domain::RecognitionRecord;
using ocrservice::domain::RecognitionSnapshot;
using ocrservice::domain::RecognitionStatus;
using ocrservice::domain::RepositoryFailure;
using ocrservice::domain::RepositoryResult;
using ocrservice::domain::SaveImageCommand;
using ocrservice::domain::Sha256Digest;
using ocrservice::domain::StorageFailure;
using ocrservice::domain::StorageResult;
using ocrservice::domain::StoredImage;
using ocrservice::domain::UtcTimePoint;
using ocrservice::domain::Uuid;
using ocrservice::queue::RecognitionTaskQueue;
using ocrservice::services::recognition::worker::DecodedBgrImage;
using ocrservice::services::recognition::worker::IStoredImageDecoder;
using ocrservice::services::recognition::worker::IWorkerClock;
using ocrservice::services::recognition::worker::OpenCvStoredImageDecoder;
using ocrservice::services::recognition::worker::RecognitionWorker;

RecognitionId recognitionId(const std::uint64_t sequence) {
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0U; index < sizeof(sequence); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        bytes[15U - index] = static_cast<std::uint8_t>((sequence >> shift) & 0xFFU);
    }
    return RecognitionId(Uuid::v4(bytes));
}

CaptureId captureId(const std::uint64_t sequence) {
    return CaptureId(recognitionId(sequence).value());
}

UtcTimePoint utc(const std::int64_t milliseconds) {
    return UtcTimePoint(milliseconds);
}

RecognitionRecord processingRecord(
    const RecognitionId& id,
    const std::uint64_t imageSize = 4U,
    const ImageMime mime = ImageMime::jpeg,
    const std::int64_t startedAt = 1000) {
    return RecognitionRecord(
        RecognitionSnapshot(
            id,
            1U,
            DeviceId::parse("device-001"),
            RecognitionStatus::processing,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            utc(900),
            utc(startedAt),
            std::nullopt,
            std::nullopt),
        captureId(100U),
        Sha256Digest(std::array<std::uint8_t, 32>{}),
        ocrservice::domain::RelativeImagePath::parseGenerated("2026/08/23/image.jpg"),
        mime,
        imageSize);
}

const char* failureMessage(const ModelFailureCode code) {
    switch (code) {
        case ModelFailureCode::plateNotFound:
            return "plate not found";
        case ModelFailureCode::plateRecognitionFailed:
            return "plate recognition failed";
        case ModelFailureCode::modelInferenceError:
            return "model inference failed";
    }
    return "model inference failed";
}

RecognitionRecord finalizedRecord(
    const RecognitionRecord& processing,
    const FinalizeRecognition& command) {
    const auto& original = processing.snapshot();
    std::optional<PlateNumber> plate;
    std::optional<RecognitionFailureCode> errorCode;
    std::optional<std::string> errorMessage;
    RecognitionStatus status = RecognitionStatus::failed;
    if (command.outcome.isSuccess()) {
        status = RecognitionStatus::succeeded;
        plate = command.outcome.plateNumber();
    } else {
        errorCode = ocrservice::domain::toRecognitionFailureCode(command.outcome.failureCode());
        errorMessage = failureMessage(command.outcome.failureCode());
    }
    return RecognitionRecord(
        RecognitionSnapshot(
            original.recognitionId(),
            original.revision() + 1U,
            original.deviceId(),
            status,
            std::move(plate),
            errorCode,
            std::move(errorMessage),
            original.capturedAt(),
            original.startedAt(),
            command.completedAtUtc,
            command.durationMs),
        processing.captureId(),
        processing.imageSha256(),
        processing.relativeImagePath(),
        processing.imageMime(),
        processing.imageSizeBytes());
}

void enqueue(RecognitionTaskQueue& queue, const RecognitionId& id) {
    auto reservation = queue.tryReserve();
    if (!reservation) {
        throw std::runtime_error("test failed to reserve queue slot");
    }
    auto gate = reservation->commit(ocrservice::domain::RecognitionTask(id));
    gate.open();
}

template <typename Predicate>
bool waitUntil(Predicate predicate, const std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

class FakeRepository final : public ocrservice::domain::IRecognitionRepository {
public:
    std::optional<RecognitionRecord> current;
    std::function<RepositoryResult<std::optional<RecognitionRecord>>(const RecognitionId&)>
        findBehavior;
    std::function<RepositoryResult<RecognitionRecord>(const FinalizeRecognition&)>
        finalizeBehavior;
    std::function<void(std::size_t)> afterFind;
    std::function<void(std::size_t)> afterFinalize;

    RepositoryResult<std::optional<RecognitionRecord>> findByCapture(
        const DeviceId&,
        const CaptureId&) override {
        return RepositoryFailure::internal;
    }

    RepositoryResult<std::optional<RecognitionRecord>> findById(
        const RecognitionId& id) override {
        const auto call = findCalls_.fetch_add(1U) + 1U;
        RepositoryResult<std::optional<RecognitionRecord>> result = RepositoryFailure::internal;
        if (findBehavior) {
            result = findBehavior(id);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            result = current;
        }
        if (afterFind) {
            afterFind(call);
        }
        return result;
    }

    RepositoryResult<RecognitionRecord> insertProcessing(const NewRecognition&) override {
        return RepositoryFailure::internal;
    }

    RepositoryResult<RecognitionRecord> finalize(const FinalizeRecognition& command) override {
        const auto call = finalizeCalls_.fetch_add(1U) + 1U;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lastDurationMs_ = command.durationMs;
            lastCompletedAtMs_ = command.completedAtUtc.unixMilliseconds();
            lastOutcomeSucceeded_ = command.outcome.isSuccess();
            lastFailure_ = command.outcome.isSuccess() ? std::nullopt :
                                                        std::optional<ModelFailureCode>(
                                                            command.outcome.failureCode());
        }

        RepositoryResult<RecognitionRecord> result = RepositoryFailure::internal;
        if (finalizeBehavior) {
            result = finalizeBehavior(command);
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!current) {
                result = RepositoryFailure::notFound;
            } else if (current->snapshot().status() != RecognitionStatus::processing) {
                result = RepositoryFailure::stateConflict;
            } else {
                current = finalizedRecord(*current, command);
                result = *current;
            }
        }
        if (afterFinalize) {
            afterFinalize(call);
        }
        return result;
    }

    RepositoryResult<std::vector<RecognitionRecord>> failInterruptedOnStartup(
        UtcTimePoint) override {
        return RepositoryFailure::internal;
    }

    RepositoryResult<PageResult<RecognitionRecord>> queryHistory(
        const HistoryFilter&,
        const PageRequest&) override {
        return RepositoryFailure::internal;
    }

    RepositoryResult<std::unique_ptr<IHistoryCursor>> openHistoryCursor(
        const HistoryFilter&) override {
        return RepositoryFailure::internal;
    }

    RepositoryResult<HistoryCursorResult> visitHistory(
        const HistoryFilter&,
        const HistoryVisitor&) override {
        return RepositoryFailure::internal;
    }

    std::size_t findCalls() const noexcept { return findCalls_.load(); }
    std::size_t finalizeCalls() const noexcept { return finalizeCalls_.load(); }

    std::optional<RecognitionRecord> currentRecord() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return current;
    }

    std::uint64_t lastDurationMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastDurationMs_;
    }

    std::int64_t lastCompletedAtMs() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastCompletedAtMs_;
    }

    bool lastOutcomeSucceeded() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastOutcomeSucceeded_;
    }

    std::optional<ModelFailureCode> lastFailure() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastFailure_;
    }

private:
    mutable std::mutex mutex_;
    std::atomic<std::size_t> findCalls_{0U};
    std::atomic<std::size_t> finalizeCalls_{0U};
    std::uint64_t lastDurationMs_ = 0U;
    std::int64_t lastCompletedAtMs_ = 0;
    bool lastOutcomeSucceeded_ = false;
    std::optional<ModelFailureCode> lastFailure_;
};

struct ReaderSettings final {
    std::vector<std::uint8_t> bytes{0xFFU, 0xD8U, 0xFFU, 0x00U};
    std::optional<std::size_t> readableBytes;
    std::size_t maximumChunk = std::numeric_limits<std::size_t>::max();
    bool returnCountPastCapacity = false;
    bool throwOnRead = false;
};

class FakeReader final : public ImageReader {
public:
    explicit FakeReader(ReaderSettings settings) : settings_(std::move(settings)) {}

    std::size_t read(std::uint8_t* destination, const std::size_t capacity) override {
        if (settings_.throwOnRead) {
            throw std::runtime_error("raw reader exception /secret/image.jpg bearer-token");
        }
        if (settings_.returnCountPastCapacity) {
            settings_.returnCountPastCapacity = false;
            return capacity + 1U;
        }
        const auto readable = std::min(
            settings_.bytes.size(), settings_.readableBytes.value_or(settings_.bytes.size()));
        if (offset_ >= readable || capacity == 0U) {
            return 0U;
        }
        const auto count = std::min({capacity, settings_.maximumChunk, readable - offset_});
        std::memcpy(destination, settings_.bytes.data() + offset_, count);
        offset_ += count;
        return count;
    }

private:
    ReaderSettings settings_;
    std::size_t offset_ = 0U;
};

class FakeStorage final : public ocrservice::domain::IImageStorage {
public:
    ReaderSettings reader;
    ImageMime reportedMime = ImageMime::jpeg;
    std::uint64_t reportedSize = 4U;
    std::optional<StorageFailure> openFailure;
    bool throwOnOpen = false;
    std::atomic<std::size_t> openCalls{0U};

    StorageResult<StoredImage> saveAtomically(const SaveImageCommand&) override {
        return StorageFailure::internal;
    }

    StorageResult<ImageFile> openForRead(
        const ocrservice::domain::RelativeImagePath&) override {
        ++openCalls;
        if (throwOnOpen) {
            throw std::runtime_error("raw storage exception C:/secret/image.jpg password=value");
        }
        if (openFailure) {
            return *openFailure;
        }
        return ImageFile(std::make_unique<FakeReader>(reader), reportedMime, reportedSize);
    }

    void removeBestEffort(const ocrservice::domain::RelativeImagePath&) noexcept override {}
};

class FakeDecoder final : public IStoredImageDecoder {
public:
    bool shouldThrow = false;
    std::vector<std::uint8_t> decodedBytes{1U, 2U, 3U, 4U, 5U, 6U};
    std::atomic<std::size_t> calls{0U};

    DecodedBgrImage decode(const ocrservice::domain::ByteView) override {
        ++calls;
        if (shouldThrow) {
            throw std::runtime_error("raw decoder exception /secret/path");
        }
        return DecodedBgrImage(decodedBytes, 2U, 1U, 6U);
    }
};

class FakeRecognizer final : public ocrservice::domain::IPlateRecognizer {
public:
    std::function<RecognitionOutcome(BgrImageView)> behavior;
    std::atomic<std::size_t> calls{0U};

    RecognitionOutcome recognize(const BgrImageView image) override {
        ++calls;
        if (behavior) {
            return behavior(image);
        }
        return RecognitionOutcome::succeeded(PlateNumber::parse("ABC123"));
    }
};

class FakeClock final : public IWorkerClock {
public:
    std::vector<std::uint64_t> monotonicValues{100U, 125U};
    UtcTimePoint utcNow = utc(1100);

    std::uint64_t monotonicMilliseconds() noexcept override {
        const auto index = monotonicIndex_.fetch_add(1U);
        return index < monotonicValues.size() ? monotonicValues[index] :
                                                monotonicValues.back();
    }

    UtcTimePoint nowUtc() noexcept override { return utcNow; }

private:
    std::atomic<std::size_t> monotonicIndex_{0U};
};

enum class PublishBehavior { accepted, rejected, throwing };

class FakePublisher final : public ocrservice::domain::IMqttPublisher {
public:
    PublishBehavior managementBehavior = PublishBehavior::accepted;
    PublishBehavior deviceBehavior = PublishBehavior::accepted;
    PublishFailure rejectedFailure = PublishFailure::notConnected;
    std::function<void()> afterManagement;
    std::function<void()> afterDevice;

    PublishAttempt publishManagement(const RecognitionSnapshot&) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calls_.push_back("management");
        }
        if (afterManagement) {
            afterManagement();
        }
        return result(managementBehavior);
    }

    PublishAttempt publishDeviceFinal(
        const RecognitionSnapshot&,
        const GateAction action) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            calls_.push_back("device");
            actions_.push_back(action);
        }
        if (afterDevice) {
            afterDevice();
        }
        return result(deviceBehavior);
    }

    std::vector<std::string> calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return calls_;
    }

    std::vector<GateAction> actions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return actions_;
    }

private:
    PublishAttempt result(const PublishBehavior behavior) const {
        if (behavior == PublishBehavior::throwing) {
            throw std::runtime_error("raw MQTT exception password=value /secret/path");
        }
        return behavior == PublishBehavior::accepted ? PublishAttempt::accepted() :
                                                       PublishAttempt::rejected(rejectedFailure);
    }

    mutable std::mutex mutex_;
    std::vector<std::string> calls_;
    std::vector<GateAction> actions_;
};

struct Harness final {
    RecognitionTaskQueue queue{4U};
    FakeRepository repository;
    FakeStorage storage;
    FakeRecognizer recognizer;
    FakePublisher publisher;
    FakeClock clock;
    FakeDecoder decoder;
    RecognitionWorker worker;
    RecognitionId id = recognitionId(1U);

    Harness()
        : worker(
              queue,
              repository,
              storage,
              recognizer,
              publisher,
              clock,
              decoder) {
        repository.current = processingRecord(id);
        publisher.afterDevice = [this] { queue.requestStop(); };
    }

    void runOne() {
        enqueue(queue, id);
        worker.run();
    }
};

TEST(RecognitionWorkerTest, FinalizesSuccessThenPublishesManagementAndOpenDeviceResult) {
    Harness harness;
    harness.runOne();

    EXPECT_EQ(harness.repository.finalizeCalls(), 1U);
    EXPECT_TRUE(harness.repository.lastOutcomeSucceeded());
    EXPECT_EQ(harness.repository.lastDurationMs(), 25U);
    EXPECT_EQ(harness.publisher.calls(), (std::vector<std::string>{"management", "device"}));
    EXPECT_EQ(harness.publisher.actions(), (std::vector<GateAction>{GateAction::open}));
    const auto final = harness.repository.currentRecord();
    ASSERT_TRUE(final.has_value());
    EXPECT_EQ(final->snapshot().status(), RecognitionStatus::succeeded);
}

TEST(RecognitionWorkerTest, PreservesAllExplicitModelFailureOutcomesAndKeepsGateClosed) {
    const std::array<ModelFailureCode, 3> failures{
        ModelFailureCode::plateNotFound,
        ModelFailureCode::plateRecognitionFailed,
        ModelFailureCode::modelInferenceError};
    for (const auto failure : failures) {
        Harness harness;
        harness.recognizer.behavior = [failure](BgrImageView) {
            return RecognitionOutcome::failed(failure);
        };
        harness.runOne();
        EXPECT_EQ(harness.repository.lastFailure(), failure);
        EXPECT_EQ(
            harness.publisher.actions(),
            (std::vector<GateAction>{GateAction::keepClosed}));
    }
}

TEST(RecognitionWorkerTest, MapsModelExceptionToInferenceErrorAndContinuesToFinalization) {
    Harness harness;
    harness.recognizer.behavior = [](BgrImageView) -> RecognitionOutcome {
        throw std::runtime_error("raw model exception /models/private.onnx");
    };
    harness.runOne();

    EXPECT_EQ(harness.repository.lastFailure(), ModelFailureCode::modelInferenceError);
    EXPECT_EQ(harness.publisher.actions(), (std::vector<GateAction>{GateAction::keepClosed}));
}

TEST(RecognitionWorkerTest, MapsMissingShortTrailingAndOverreportedImageReadsToInferenceError) {
    enum class Case { missing, shortRead, trailing, overreported };
    for (const auto testCase : {Case::missing, Case::shortRead, Case::trailing, Case::overreported}) {
        Harness harness;
        if (testCase == Case::missing) {
            harness.storage.openFailure = StorageFailure::notFound;
        } else if (testCase == Case::shortRead) {
            harness.storage.reader.readableBytes = 3U;
        } else if (testCase == Case::trailing) {
            harness.storage.reader.bytes.push_back(0x44U);
        } else {
            harness.storage.reader.returnCountPastCapacity = true;
        }
        harness.runOne();
        EXPECT_EQ(harness.repository.lastFailure(), ModelFailureCode::modelInferenceError);
        EXPECT_EQ(harness.decoder.calls.load(), 0U);
    }
}

TEST(RecognitionWorkerTest, RejectsDatabaseFileSizeAndMimeMetadataDrift) {
    {
        Harness harness;
        harness.storage.reportedSize = 5U;
        harness.runOne();
        EXPECT_EQ(harness.repository.lastFailure(), ModelFailureCode::modelInferenceError);
        EXPECT_EQ(harness.decoder.calls.load(), 0U);
    }
    {
        Harness harness;
        harness.storage.reportedMime = ImageMime::png;
        harness.runOne();
        EXPECT_EQ(harness.repository.lastFailure(), ModelFailureCode::modelInferenceError);
        EXPECT_EQ(harness.decoder.calls.load(), 0U);
    }
}

TEST(RecognitionWorkerTest, RejectsActualMagicThatDoesNotMatchDatabaseMime) {
    Harness harness;
    harness.storage.reader.bytes = {0x89U, 0x50U, 0x4EU, 0x47U};
    harness.runOne();

    EXPECT_EQ(harness.repository.lastFailure(), ModelFailureCode::modelInferenceError);
    EXPECT_EQ(harness.decoder.calls.load(), 0U);
}

TEST(RecognitionWorkerTest, RejectsDamagedImageThatPassesMagicCheckButCannotDecode) {
    RecognitionTaskQueue queue(1U);
    FakeRepository repository;
    FakeStorage storage;
    FakeRecognizer recognizer;
    FakePublisher publisher;
    FakeClock clock;
    OpenCvStoredImageDecoder decoder;
    const auto id = recognitionId(2U);
    repository.current = processingRecord(id);
    publisher.afterDevice = [&queue] { queue.requestStop(); };
    RecognitionWorker worker(
        queue, repository, storage, recognizer, publisher, clock, decoder);

    enqueue(queue, id);
    worker.run();

    EXPECT_EQ(repository.lastFailure(), ModelFailureCode::modelInferenceError);
    EXPECT_EQ(recognizer.calls.load(), 0U);
}

TEST(OpenCvStoredImageDecoderTest, DecodesValidJpegAndPngIntoOwningContiguousBgrBuffers) {
    OpenCvStoredImageDecoder decoder;
    for (const std::string extension : {".jpg", ".png"}) {
        constexpr int width = 16;
        constexpr int height = 12;
        const cv::Scalar expectedBgr(12, 34, 56);
        const cv::Mat source(height, width, CV_8UC3, expectedBgr);
        std::vector<std::uint8_t> compressed;
        std::vector<int> parameters;
        if (extension == ".jpg") {
            parameters = {cv::IMWRITE_JPEG_QUALITY, 100};
        }
        ASSERT_TRUE(cv::imencode(extension, source, compressed, parameters));
        ASSERT_FALSE(compressed.empty());

        auto decoded = decoder.decode(
            ocrservice::domain::ByteView(compressed.data(), compressed.size()));
        std::fill(compressed.begin(), compressed.end(), 0U);
        compressed.clear();
        compressed.shrink_to_fit();

        const auto view = decoded.view();
        EXPECT_EQ(view.width(), static_cast<std::size_t>(width));
        EXPECT_EQ(view.height(), static_cast<std::size_t>(height));
        EXPECT_EQ(view.rowStride(), static_cast<std::size_t>(width * 3));
        EXPECT_EQ(view.byteSize(), static_cast<std::size_t>(width * height * 3));
        const auto center = static_cast<std::size_t>((height / 2) * width * 3 +
                                                     (width / 2) * 3);
        EXPECT_NEAR(view.data()[center], expectedBgr[0], 3.0);
        EXPECT_NEAR(view.data()[center + 1U], expectedBgr[1], 3.0);
        EXPECT_NEAR(view.data()[center + 2U], expectedBgr[2], 3.0);
    }
}

TEST(RecognitionWorkerTest, MapsInjectedEmptyDecodeFailureToInferenceError) {
    Harness harness;
    harness.decoder.shouldThrow = true;
    harness.runOne();
    EXPECT_EQ(harness.repository.lastFailure(), ModelFailureCode::modelInferenceError);
    EXPECT_EQ(harness.recognizer.calls.load(), 0U);
}

TEST(RecognitionWorkerTest, ClampsCompletedAtOnWallClockRollbackAndUsesMonotonicDuration) {
    Harness harness;
    harness.clock.utcNow = utc(500);
    harness.clock.monotonicValues = {900U, 940U};
    harness.runOne();

    EXPECT_EQ(harness.repository.lastCompletedAtMs(), 1000);
    EXPECT_EQ(harness.repository.lastDurationMs(), 40U);
}

TEST(RecognitionWorkerTest, ClampsMonotonicRollbackToZeroAndSaturatesHugeDuration) {
    {
        Harness harness;
        harness.clock.monotonicValues = {100U, 50U};
        harness.runOne();
        EXPECT_EQ(harness.repository.lastDurationMs(), 0U);
    }
    {
        Harness harness;
        harness.clock.monotonicValues = {0U, std::numeric_limits<std::uint64_t>::max()};
        harness.runOne();
        EXPECT_EQ(
            harness.repository.lastDurationMs(),
            ocrservice::domain::kJsonSafeIntegerMaximum);
    }
}

TEST(RecognitionWorkerTest, DropsMissingAndAlreadyFinalRecordsWithoutInferenceOrPublish) {
    {
        Harness harness;
        harness.repository.current = std::nullopt;
        harness.repository.afterFind = [&harness](std::size_t) { harness.queue.requestStop(); };
        harness.runOne();
        EXPECT_EQ(harness.repository.finalizeCalls(), 0U);
        EXPECT_EQ(harness.recognizer.calls.load(), 0U);
        EXPECT_TRUE(harness.publisher.calls().empty());
    }
    {
        Harness harness;
        const auto processing = *harness.repository.current;
        harness.repository.current = finalizedRecord(
            processing,
            FinalizeRecognition{
                harness.id,
                RecognitionOutcome::succeeded(PlateNumber::parse("ABC123")),
                utc(1100),
                20U});
        harness.repository.afterFind = [&harness](std::size_t) { harness.queue.requestStop(); };
        harness.runOne();
        EXPECT_EQ(harness.repository.finalizeCalls(), 0U);
        EXPECT_EQ(harness.recognizer.calls.load(), 0U);
        EXPECT_TRUE(harness.publisher.calls().empty());
    }
}

TEST(RecognitionWorkerTest, TreatsRepositoryNotFoundFailureAsStaleTask) {
    Harness harness;
    harness.repository.findBehavior = [](const RecognitionId&) {
        return RepositoryResult<std::optional<RecognitionRecord>>(RepositoryFailure::notFound);
    };
    harness.repository.afterFind = [&harness](std::size_t) { harness.queue.requestStop(); };
    harness.runOne();
    EXPECT_EQ(harness.repository.finalizeCalls(), 0U);
    EXPECT_TRUE(harness.publisher.calls().empty());
}

TEST(RecognitionWorkerTest, LeavesProcessingOnLookupTechnicalFailuresAndExceptions) {
    for (const auto failure : {RepositoryFailure::unavailable, RepositoryFailure::conflict,
                               RepositoryFailure::stateConflict, RepositoryFailure::internal}) {
        Harness harness;
        harness.repository.findBehavior = [failure](const RecognitionId&) {
            return RepositoryResult<std::optional<RecognitionRecord>>(failure);
        };
        harness.repository.afterFind = [&harness](std::size_t) { harness.queue.requestStop(); };
        harness.runOne();
        EXPECT_EQ(harness.repository.finalizeCalls(), 0U);
        EXPECT_EQ(harness.storage.openCalls.load(), 0U);
        EXPECT_TRUE(harness.publisher.calls().empty());
    }
    {
        Harness harness;
        harness.repository.findBehavior = [&harness](const RecognitionId&)
            -> RepositoryResult<std::optional<RecognitionRecord>> {
            harness.queue.requestStop();
            throw std::runtime_error("raw SQL exception password=value");
        };
        harness.runOne();
        EXPECT_EQ(harness.repository.finalizeCalls(), 0U);
        EXPECT_EQ(harness.storage.openCalls.load(), 0U);
        EXPECT_TRUE(harness.publisher.calls().empty());
    }
}

TEST(RecognitionWorkerTest, CallsFinalizeExactlyOnceAndNeverPublishesAnyFinalizeFailure) {
    for (const auto failure : {RepositoryFailure::notFound, RepositoryFailure::stateConflict,
                               RepositoryFailure::unavailable, RepositoryFailure::conflict,
                               RepositoryFailure::internal}) {
        Harness harness;
        harness.repository.finalizeBehavior = [failure](const FinalizeRecognition&) {
            return RepositoryResult<RecognitionRecord>(failure);
        };
        harness.repository.afterFinalize = [&harness](std::size_t) {
            harness.queue.requestStop();
        };
        harness.runOne();
        EXPECT_EQ(harness.repository.finalizeCalls(), 1U);
        EXPECT_TRUE(harness.publisher.calls().empty());
    }
    {
        Harness harness;
        harness.repository.finalizeBehavior = [&harness](const FinalizeRecognition&)
            -> RepositoryResult<RecognitionRecord> {
            harness.queue.requestStop();
            throw std::runtime_error("raw commit uncertainty SQL statement");
        };
        harness.runOne();
        EXPECT_EQ(harness.repository.finalizeCalls(), 1U);
        EXPECT_TRUE(harness.publisher.calls().empty());
    }
}

TEST(RecognitionWorkerTest, DoesNotPublishInvalidFinalizeReturnRecords) {
    {
        Harness harness;
        const auto other = processingRecord(recognitionId(99U));
        harness.repository.finalizeBehavior = [other](const FinalizeRecognition& command) {
            return RepositoryResult<RecognitionRecord>(finalizedRecord(other, command));
        };
        harness.repository.afterFinalize = [&harness](std::size_t) {
            harness.queue.requestStop();
        };
        harness.runOne();
        EXPECT_TRUE(harness.publisher.calls().empty());
    }
    {
        Harness harness;
        const auto processing = *harness.repository.current;
        harness.repository.finalizeBehavior = [processing](const FinalizeRecognition&) {
            return RepositoryResult<RecognitionRecord>(processing);
        };
        harness.repository.afterFinalize = [&harness](std::size_t) {
            harness.queue.requestStop();
        };
        harness.runOne();
        EXPECT_TRUE(harness.publisher.calls().empty());
    }
}

TEST(RecognitionWorkerTest, PublishesOnlyAfterFinalizeReturnsCommittedRecord) {
    Harness harness;
    harness.repository.finalizeBehavior = [&harness](const FinalizeRecognition& command) {
        EXPECT_TRUE(harness.publisher.calls().empty());
        return RepositoryResult<RecognitionRecord>(
            finalizedRecord(*harness.repository.current, command));
    };
    harness.runOne();
    EXPECT_EQ(harness.publisher.calls(), (std::vector<std::string>{"management", "device"}));
}

TEST(RecognitionWorkerTest, AttemptsDevicePublishAfterManagementRejectsOrThrows) {
    for (const auto behavior : {PublishBehavior::rejected, PublishBehavior::throwing}) {
        Harness harness;
        harness.publisher.managementBehavior = behavior;
        harness.runOne();
        EXPECT_EQ(
            harness.publisher.calls(),
            (std::vector<std::string>{"management", "device"}));
    }
}

TEST(RecognitionWorkerTest, ContainsDevicePublishRejectionAndException) {
    for (const auto behavior : {PublishBehavior::rejected, PublishBehavior::throwing}) {
        Harness harness;
        harness.publisher.deviceBehavior = behavior;
        harness.runOne();
        EXPECT_EQ(
            harness.publisher.calls(),
            (std::vector<std::string>{"management", "device"}));
        EXPECT_EQ(harness.repository.finalizeCalls(), 1U);
    }
}

TEST(RecognitionWorkerTest, SequentialDuplicateTaskDoesNotRepublishFinalRecord) {
    Harness harness;
    harness.publisher.afterDevice = nullptr;
    harness.repository.afterFind = [&harness](const std::size_t call) {
        if (call == 2U) {
            harness.queue.requestStop();
        }
    };
    enqueue(harness.queue, harness.id);
    enqueue(harness.queue, harness.id);
    harness.worker.run();

    EXPECT_EQ(harness.repository.findCalls(), 2U);
    EXPECT_EQ(harness.repository.finalizeCalls(), 1U);
    EXPECT_EQ(harness.publisher.calls(), (std::vector<std::string>{"management", "device"}));
}

TEST(RecognitionWorkerTest, ConcurrentDuplicateOnlyFinalizeWinnerPublishes) {
    RecognitionTaskQueue queue(2U);
    FakeRepository repository;
    FakeStorage storage;
    FakeRecognizer recognizer;
    FakePublisher publisher;
    FakeClock firstClock;
    FakeClock secondClock;
    FakeDecoder firstDecoder;
    FakeDecoder secondDecoder;
    const auto id = recognitionId(3U);
    repository.current = processingRecord(id);

    std::atomic<std::size_t> entered{0U};
    recognizer.behavior = [&](BgrImageView) {
        ++entered;
        if (!waitUntil([&] { return entered.load() == 2U; })) {
            throw std::runtime_error("test concurrent recognizer barrier timed out");
        }
        return RecognitionOutcome::succeeded(PlateNumber::parse("ABC123"));
    };
    repository.afterFinalize = [&](const std::size_t call) {
        if (call == 2U) {
            queue.requestStop();
        }
    };
    RecognitionWorker first(
        queue, repository, storage, recognizer, publisher, firstClock, firstDecoder);
    RecognitionWorker second(
        queue, repository, storage, recognizer, publisher, secondClock, secondDecoder);
    enqueue(queue, id);
    enqueue(queue, id);
    std::thread firstThread([&] { first.run(); });
    std::thread secondThread([&] { second.run(); });
    firstThread.join();
    secondThread.join();

    EXPECT_EQ(repository.finalizeCalls(), 2U);
    EXPECT_EQ(publisher.calls(), (std::vector<std::string>{"management", "device"}));
}

TEST(RecognitionWorkerTest, StopLetsTakenTaskFinishAndPublish) {
    RecognitionTaskQueue queue(1U);
    FakeRepository repository;
    FakeStorage storage;
    FakeRecognizer recognizer;
    FakePublisher publisher;
    FakeClock clock;
    FakeDecoder decoder;
    const auto id = recognitionId(4U);
    repository.current = processingRecord(id);
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
    recognizer.behavior = [&](BgrImageView) {
        entered = true;
        if (!waitUntil([&] { return released.load(); })) {
            throw std::runtime_error("test blocked recognizer timed out");
        }
        return RecognitionOutcome::succeeded(PlateNumber::parse("ABC123"));
    };
    RecognitionWorker worker(queue, repository, storage, recognizer, publisher, clock, decoder);
    enqueue(queue, id);
    std::thread thread([&] { worker.run(); });
    const bool observedEntry = waitUntil([&] { return entered.load(); });
    queue.requestStop();
    released = true;
    thread.join();

    ASSERT_TRUE(observedEntry);
    EXPECT_EQ(repository.finalizeCalls(), 1U);
    EXPECT_EQ(publisher.calls(), (std::vector<std::string>{"management", "device"}));
}

TEST(RecognitionWorkerTest, StopDropsTasksNotYetTaken) {
    RecognitionTaskQueue queue(2U);
    FakeRepository repository;
    FakeStorage storage;
    FakeRecognizer recognizer;
    FakePublisher publisher;
    FakeClock clock;
    FakeDecoder decoder;
    const auto firstId = recognitionId(5U);
    const auto secondId = recognitionId(6U);
    repository.current = processingRecord(firstId);
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
    recognizer.behavior = [&](BgrImageView) {
        entered = true;
        if (!waitUntil([&] { return released.load(); })) {
            throw std::runtime_error("test blocked recognizer timed out");
        }
        return RecognitionOutcome::succeeded(PlateNumber::parse("ABC123"));
    };
    RecognitionWorker worker(queue, repository, storage, recognizer, publisher, clock, decoder);
    enqueue(queue, firstId);
    std::thread thread([&] { worker.run(); });
    const bool observedEntry = waitUntil([&] { return entered.load(); });
    enqueue(queue, secondId);
    queue.requestStop();
    released = true;
    thread.join();

    ASSERT_TRUE(observedEntry);
    EXPECT_EQ(repository.findCalls(), 1U);
    EXPECT_EQ(repository.finalizeCalls(), 1U);
}

TEST(RecognitionWorkerTest, TaskExceptionDoesNotPreventFollowingTaskFromSucceeding) {
    RecognitionTaskQueue queue(2U);
    FakeRepository repository;
    FakeStorage storage;
    FakeRecognizer recognizer;
    FakePublisher publisher;
    FakeClock clock;
    FakeDecoder decoder;
    const auto firstId = recognitionId(7U);
    const auto secondId = recognitionId(8U);
    repository.current = processingRecord(secondId);
    repository.findBehavior = [&](const RecognitionId& id)
        -> RepositoryResult<std::optional<RecognitionRecord>> {
        if (id == firstId) {
            throw std::runtime_error("first task raw SQL failure");
        }
        return repository.current;
    };
    publisher.afterDevice = [&queue] { queue.requestStop(); };
    RecognitionWorker worker(queue, repository, storage, recognizer, publisher, clock, decoder);
    enqueue(queue, firstId);
    enqueue(queue, secondId);
    worker.run();

    EXPECT_EQ(repository.findCalls(), 2U);
    EXPECT_EQ(repository.finalizeCalls(), 1U);
    EXPECT_EQ(publisher.calls(), (std::vector<std::string>{"management", "device"}));
}

TEST(RecognitionWorkerTest, OwningDecodedBufferRemainsValidThroughoutRecognizerCall) {
    Harness harness;
    const auto expected = harness.decoder.decodedBytes;
    harness.recognizer.behavior = [expected](const BgrImageView image) {
        EXPECT_EQ(image.byteSize(), expected.size());
        EXPECT_TRUE(std::equal(expected.begin(), expected.end(), image.data()));
        return RecognitionOutcome::succeeded(PlateNumber::parse("ABC123"));
    };
    harness.runOne();
    EXPECT_TRUE(harness.repository.lastOutcomeSucceeded());
}

TEST(RecognitionWorkerTest, LogsStableSanitizedFieldsWithoutRawExceptionOrPath) {
    std::ostringstream output;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    auto spdlogger = std::make_shared<spdlog::logger>("worker-log-test", sink);
    spdlogger->set_pattern("%v");
    auto logger = std::make_shared<ocrservice::logging::JsonLinesLogger>(spdlogger);

    RecognitionTaskQueue queue(1U);
    FakeRepository repository;
    FakeStorage storage;
    FakeRecognizer recognizer;
    FakePublisher publisher;
    FakeClock clock;
    FakeDecoder decoder;
    const auto id = recognitionId(9U);
    repository.current = processingRecord(id);
    storage.throwOnOpen = true;
    publisher.managementBehavior = PublishBehavior::rejected;
    publisher.rejectedFailure = PublishFailure::transportError;
    publisher.deviceBehavior = PublishBehavior::throwing;
    publisher.afterDevice = [&queue] { queue.requestStop(); };
    RecognitionWorker worker(
        queue, repository, storage, recognizer, publisher, clock, decoder, logger);
    enqueue(queue, id);
    worker.run();
    logger->flush();

    const auto logged = output.str();
    EXPECT_NE(logged.find("\"requestId\":\"system\""), std::string::npos);
    EXPECT_NE(logged.find("MODEL_INFERENCE_ERROR"), std::string::npos);
    EXPECT_NE(logged.find("MQTT_TRANSPORT_ERROR"), std::string::npos);
    EXPECT_NE(logged.find("MQTT_PUBLISH_EXCEPTION"), std::string::npos);
    EXPECT_EQ(logged.find("password=value"), std::string::npos);
    EXPECT_EQ(logged.find("secret/image.jpg"), std::string::npos);
    EXPECT_EQ(logged.find("raw storage exception"), std::string::npos);
    EXPECT_EQ(logged.find("raw MQTT exception"), std::string::npos);
}

TEST(RecognitionWorkerTest, LogsStableClassificationForEveryRejectedPublishFailure) {
    const std::array<std::pair<PublishFailure, const char*>, 4> cases{{
        {PublishFailure::notConnected, "MQTT_NOT_CONNECTED"},
        {PublishFailure::brokerRejected, "MQTT_BROKER_REJECTED"},
        {PublishFailure::transportError, "MQTT_TRANSPORT_ERROR"},
        {PublishFailure::stopping, "MQTT_STOPPING"},
    }};
    for (const auto& [failure, expectedCode] : cases) {
        std::ostringstream output;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
        auto spdlogger = std::make_shared<spdlog::logger>("worker-rejection-log-test", sink);
        spdlogger->set_pattern("%v");
        auto logger = std::make_shared<ocrservice::logging::JsonLinesLogger>(spdlogger);

        RecognitionTaskQueue queue(1U);
        FakeRepository repository;
        FakeStorage storage;
        FakeRecognizer recognizer;
        FakePublisher publisher;
        FakeClock clock;
        FakeDecoder decoder;
        const auto id = recognitionId(10U);
        repository.current = processingRecord(id);
        publisher.managementBehavior = PublishBehavior::rejected;
        publisher.rejectedFailure = failure;
        publisher.afterDevice = [&queue] { queue.requestStop(); };
        RecognitionWorker worker(
            queue, repository, storage, recognizer, publisher, clock, decoder, logger);
        enqueue(queue, id);
        worker.run();
        logger->flush();

        EXPECT_NE(output.str().find(expectedCode), std::string::npos);
    }
}

}  // namespace
