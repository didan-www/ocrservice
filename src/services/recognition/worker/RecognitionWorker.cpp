#include "RecognitionWorker.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "Identifiers.h"
#include "LoggerFactory.h"

namespace ocrservice::services::recognition::worker {
namespace {

constexpr std::size_t kMaximumCompressedImageBytes = 10U * 1024U * 1024U;
constexpr std::size_t kMaximumImageDimension = 8192U;
constexpr std::size_t kMaximumImagePixels = 40000000U;

bool hasExpectedSignature(
    const std::vector<std::uint8_t>& bytes,
    const domain::ImageMime mime) noexcept {
    if (mime == domain::ImageMime::jpeg) {
        return bytes.size() >= 3U && bytes[0] == 0xFFU && bytes[1] == 0xD8U &&
               bytes[2] == 0xFFU;
    }
    constexpr std::uint8_t pngSignature[] = {
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    return bytes.size() >= sizeof(pngSignature) &&
           std::equal(std::begin(pngSignature), std::end(pngSignature), bytes.begin());
}

std::vector<std::uint8_t> readCompressedImage(
    domain::IImageStorage& images,
    const domain::RecognitionRecord& record) {
    auto opened = images.openForRead(record.relativeImagePath());
    if (std::holds_alternative<domain::StorageFailure>(opened)) {
        throw std::runtime_error("stored image unavailable");
    }
    auto image = std::get<domain::ImageFile>(std::move(opened));
    const auto expectedSize = record.imageSizeBytes();
    if (expectedSize == 0U || expectedSize > kMaximumCompressedImageBytes ||
        expectedSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        image.mime() != record.imageMime() || image.sizeBytes() != expectedSize) {
        throw std::runtime_error("stored image metadata mismatch");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(expectedSize));
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        const auto count = image.read(bytes.data() + offset, bytes.size() - offset);
        if (count == 0U || count > bytes.size() - offset) {
            throw std::runtime_error("stored image short read");
        }
        offset += count;
    }
    std::uint8_t trailing = 0U;
    if (image.read(&trailing, 1U) != 0U) {
        throw std::runtime_error("stored image trailing data");
    }
    if (!hasExpectedSignature(bytes, record.imageMime())) {
        throw std::runtime_error("stored image format mismatch");
    }
    return bytes;
}

std::uint64_t elapsedMilliseconds(
    const std::uint64_t started,
    const std::uint64_t completed) noexcept {
    if (completed <= started) {
        return 0U;
    }
    return std::min(completed - started, domain::kJsonSafeIntegerMaximum);
}

const char* repositoryCode(const domain::RepositoryFailure failure) noexcept {
    switch (failure) {
        case domain::RepositoryFailure::unavailable:
            return "DATABASE_UNAVAILABLE";
        case domain::RepositoryFailure::conflict:
            return "DATABASE_CONFLICT";
        case domain::RepositoryFailure::notFound:
            return "RECORD_NOT_FOUND";
        case domain::RepositoryFailure::stateConflict:
            return "RECOGNITION_STATE_CONFLICT";
        case domain::RepositoryFailure::internal:
            return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}

const char* publishCode(const domain::PublishFailure failure) noexcept {
    switch (failure) {
        case domain::PublishFailure::notConnected:
            return "MQTT_NOT_CONNECTED";
        case domain::PublishFailure::brokerRejected:
            return "MQTT_BROKER_REJECTED";
        case domain::PublishFailure::transportError:
            return "MQTT_TRANSPORT_ERROR";
        case domain::PublishFailure::stopping:
            return "MQTT_STOPPING";
    }
    return "MQTT_PUBLISH_FAILED";
}

const char* modelCode(const domain::ModelFailureCode failure) noexcept {
    switch (failure) {
        case domain::ModelFailureCode::plateNotFound:
            return "PLATE_NOT_FOUND";
        case domain::ModelFailureCode::plateRecognitionFailed:
            return "PLATE_RECOGNITION_FAILED";
        case domain::ModelFailureCode::modelInferenceError:
            return "MODEL_INFERENCE_ERROR";
    }
    return "MODEL_INFERENCE_ERROR";
}

}  // namespace

std::uint64_t SystemWorkerClock::monotonicMilliseconds() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    if (value.count() <= 0) {
        return 0U;
    }
    return static_cast<std::uint64_t>(value.count());
}

domain::UtcTimePoint SystemWorkerClock::nowUtc() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return domain::UtcTimePoint(value.count());
}

DecodedBgrImage::DecodedBgrImage(
    std::vector<std::uint8_t> bytes,
    const std::size_t width,
    const std::size_t height,
    const std::size_t rowStride)
    : bytes_(std::move(bytes)), width_(width), height_(height), rowStride_(rowStride) {
    (void)view();
}

domain::BgrImageView DecodedBgrImage::view() const {
    return domain::BgrImageView(
        bytes_.data(), bytes_.size(), width_, height_, rowStride_);
}

DecodedBgrImage OpenCvStoredImageDecoder::decode(const domain::ByteView compressedBytes) {
    if (compressedBytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("stored image is too large to decode");
    }
    const cv::Mat compressed(
        1,
        static_cast<int>(compressedBytes.size()),
        CV_8UC1,
        const_cast<std::uint8_t*>(compressedBytes.data()));
    const cv::Mat decoded = cv::imdecode(compressed, cv::IMREAD_COLOR);
    if (decoded.empty() || decoded.type() != CV_8UC3 || decoded.cols <= 0 || decoded.rows <= 0) {
        throw std::runtime_error("stored image decode failed");
    }
    const auto width = static_cast<std::size_t>(decoded.cols);
    const auto height = static_cast<std::size_t>(decoded.rows);
    if (width > kMaximumImageDimension || height > kMaximumImageDimension ||
        width > kMaximumImagePixels / height) {
        throw std::runtime_error("decoded image dimensions exceed limits");
    }

    cv::Mat contiguous = decoded.isContinuous() ? decoded : decoded.clone();
    if (contiguous.empty() || !contiguous.isContinuous()) {
        throw std::runtime_error("decoded image buffer unavailable");
    }
    const auto rowStride = width * 3U;
    const auto byteCount = rowStride * height;
    std::vector<std::uint8_t> bytes(contiguous.data, contiguous.data + byteCount);
    return DecodedBgrImage(std::move(bytes), width, height, rowStride);
}

RecognitionWorker::RecognitionWorker(
    queue::RecognitionTaskQueue& queue,
    domain::IRecognitionRepository& recognitions,
    domain::IImageStorage& images,
    domain::IPlateRecognizer& recognizer,
    domain::IMqttPublisher& mqtt,
    IWorkerClock& clock,
    IStoredImageDecoder& decoder,
    std::shared_ptr<logging::JsonLinesLogger> logger)
    : queue_(queue),
      recognitions_(recognitions),
      images_(images),
      recognizer_(recognizer),
      mqtt_(mqtt),
      clock_(clock),
      decoder_(decoder),
      logger_(std::move(logger)) {}

void RecognitionWorker::run() noexcept {
    try {
        while (const auto task = queue_.take()) {
            process(*task);
        }
    } catch (...) {
        logNoThrow(
            logging::LogLevel::error,
            "worker_loop_failed",
            "INTERNAL_ERROR");
    }
}

void RecognitionWorker::process(const domain::RecognitionTask& task) noexcept {
    const auto started = clock_.monotonicMilliseconds();
    try {
        auto found = recognitions_.findById(task.recognitionId());
        if (const auto* failure = std::get_if<domain::RepositoryFailure>(&found)) {
            if (*failure == domain::RepositoryFailure::notFound) {
                logNoThrow(
                    logging::LogLevel::info,
                    "stale_recognition_task",
                    "STALE_TASK",
                    &task.recognitionId());
                return;
            }
            logNoThrow(
                logging::LogLevel::error,
                "recognition_lookup_failed",
                repositoryCode(*failure),
                &task.recognitionId());
            return;
        }
        auto record = std::get<std::optional<domain::RecognitionRecord>>(std::move(found));
        if (!record || record->snapshot().status() != domain::RecognitionStatus::processing) {
            logNoThrow(
                logging::LogLevel::info,
                "stale_recognition_task",
                "STALE_TASK",
                &task.recognitionId());
            return;
        }

        auto outcome = recognize(*record);
        const auto completedMonotonic = clock_.monotonicMilliseconds();
        const auto durationMs = elapsedMilliseconds(started, completedMonotonic);
        const auto completedAtUtc = std::max(
            clock_.nowUtc(), record->snapshot().startedAt());
        if (!outcome.isSuccess()) {
            logNoThrow(
                logging::LogLevel::warning,
                "recognition_outcome_failed",
                modelCode(outcome.failureCode()),
                &task.recognitionId(),
                &record->snapshot().deviceId(),
                &durationMs);
        }

        auto finalized = recognitions_.finalize(domain::FinalizeRecognition{
            task.recognitionId(), std::move(outcome), completedAtUtc, durationMs});
        if (const auto* failure = std::get_if<domain::RepositoryFailure>(&finalized)) {
            logNoThrow(
                logging::LogLevel::error,
                "recognition_finalize_failed",
                repositoryCode(*failure),
                &task.recognitionId(),
                &record->snapshot().deviceId(),
                &durationMs);
            return;
        }
        auto committed = std::get<domain::RecognitionRecord>(std::move(finalized));
        const auto& committedSnapshot = committed.snapshot();
        const auto& originalSnapshot = record->snapshot();
        if (!(committedSnapshot.recognitionId() == task.recognitionId()) ||
            !(committedSnapshot.deviceId() == originalSnapshot.deviceId()) ||
            committedSnapshot.revision() != originalSnapshot.revision() + 1U ||
            committedSnapshot.status() == domain::RecognitionStatus::processing) {
            logNoThrow(
                logging::LogLevel::error,
                "recognition_finalize_invalid_result",
                "INTERNAL_ERROR",
                &task.recognitionId(),
                &record->snapshot().deviceId(),
                &durationMs);
            return;
        }
        publishFinal(committed);
    } catch (...) {
        logNoThrow(
            logging::LogLevel::error,
            "recognition_task_failed",
            "INTERNAL_ERROR",
            &task.recognitionId());
    }
}

domain::RecognitionOutcome RecognitionWorker::recognize(
    const domain::RecognitionRecord& record) noexcept {
    try {
        auto compressed = readCompressedImage(images_, record);
        auto decoded = decoder_.decode(domain::ByteView(compressed.data(), compressed.size()));
        return recognizer_.recognize(decoded.view());
    } catch (...) {
        return domain::RecognitionOutcome::failed(
            domain::ModelFailureCode::modelInferenceError);
    }
}

void RecognitionWorker::publishFinal(const domain::RecognitionRecord& record) noexcept {
    const auto& snapshot = record.snapshot();
    try {
        const auto attempt = mqtt_.publishManagement(snapshot);
        if (!attempt.wasAccepted()) {
            const auto code = attempt.failure() ? publishCode(*attempt.failure()) :
                                                  "MQTT_PUBLISH_FAILED";
            logNoThrow(
                logging::LogLevel::warning,
                "management_final_publish_failed",
                code,
                &snapshot.recognitionId(),
                &snapshot.deviceId(),
                snapshot.durationMs() ? &*snapshot.durationMs() : nullptr);
        }
    } catch (...) {
        logNoThrow(
            logging::LogLevel::warning,
            "management_final_publish_exception",
            "MQTT_PUBLISH_EXCEPTION",
            &snapshot.recognitionId(),
            &snapshot.deviceId(),
            snapshot.durationMs() ? &*snapshot.durationMs() : nullptr);
    }

    try {
        const auto action = domain::gateActionFor(snapshot.status());
        if (!action) {
            throw std::logic_error("final recognition has no gate action");
        }
        const auto attempt = mqtt_.publishDeviceFinal(snapshot, *action);
        if (!attempt.wasAccepted()) {
            const auto code = attempt.failure() ? publishCode(*attempt.failure()) :
                                                  "MQTT_PUBLISH_FAILED";
            logNoThrow(
                logging::LogLevel::warning,
                "device_final_publish_failed",
                code,
                &snapshot.recognitionId(),
                &snapshot.deviceId(),
                snapshot.durationMs() ? &*snapshot.durationMs() : nullptr);
        }
    } catch (...) {
        logNoThrow(
            logging::LogLevel::warning,
            "device_final_publish_exception",
            "MQTT_PUBLISH_EXCEPTION",
            &snapshot.recognitionId(),
            &snapshot.deviceId(),
            snapshot.durationMs() ? &*snapshot.durationMs() : nullptr);
    }
}

void RecognitionWorker::logNoThrow(
    const logging::LogLevel level,
    const char* const event,
    const char* const code,
    const domain::RecognitionId* const recognitionId,
    const domain::DeviceId* const deviceId,
    const std::uint64_t* const durationMs) noexcept {
    if (!logger_) {
        return;
    }
    try {
        logger_->log(
            level,
            logging::LogEvent(
                "recognition_worker",
                event,
                code,
                "system",
                recognitionId ? std::optional<std::string>(recognitionId->toString()) :
                                std::nullopt,
                deviceId ? std::optional<std::string>(deviceId->value()) : std::nullopt,
                durationMs ? std::optional<std::uint64_t>(*durationMs) : std::nullopt));
    } catch (...) {
    }
}

}  // namespace ocrservice::services::recognition::worker
