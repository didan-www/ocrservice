#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "Ports.h"
#include "RecognitionTaskQueue.h"

namespace ocrservice::logging {
enum class LogLevel;
class JsonLinesLogger;
}

namespace ocrservice::services::recognition::worker {

class IWorkerClock {
public:
    virtual ~IWorkerClock() = default;
    virtual std::uint64_t monotonicMilliseconds() noexcept = 0;
    virtual domain::UtcTimePoint nowUtc() noexcept = 0;
};

class SystemWorkerClock final : public IWorkerClock {
public:
    std::uint64_t monotonicMilliseconds() noexcept override;
    domain::UtcTimePoint nowUtc() noexcept override;
};

class DecodedBgrImage final {
public:
    DecodedBgrImage(
        std::vector<std::uint8_t> bytes,
        std::size_t width,
        std::size_t height,
        std::size_t rowStride);

    domain::BgrImageView view() const;

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t width_;
    std::size_t height_;
    std::size_t rowStride_;
};

class IStoredImageDecoder {
public:
    virtual ~IStoredImageDecoder() = default;
    virtual DecodedBgrImage decode(domain::ByteView compressedBytes) = 0;
};

class OpenCvStoredImageDecoder final : public IStoredImageDecoder {
public:
    DecodedBgrImage decode(domain::ByteView compressedBytes) override;
};

class RecognitionWorker final {
public:
    RecognitionWorker(
        queue::RecognitionTaskQueue& queue,
        domain::IRecognitionRepository& recognitions,
        domain::IImageStorage& images,
        domain::IPlateRecognizer& recognizer,
        domain::IMqttPublisher& mqtt,
        IWorkerClock& clock,
        IStoredImageDecoder& decoder,
        std::shared_ptr<logging::JsonLinesLogger> logger = nullptr);

    void run() noexcept;

private:
    void process(const domain::RecognitionTask& task) noexcept;
    domain::RecognitionOutcome recognize(const domain::RecognitionRecord& record) noexcept;
    void publishFinal(const domain::RecognitionRecord& record) noexcept;
    void logNoThrow(
        logging::LogLevel level,
        const char* event,
        const char* code,
        const domain::RecognitionId* recognitionId = nullptr,
        const domain::DeviceId* deviceId = nullptr,
        const std::uint64_t* durationMs = nullptr) noexcept;

    queue::RecognitionTaskQueue& queue_;
    domain::IRecognitionRepository& recognitions_;
    domain::IImageStorage& images_;
    domain::IPlateRecognizer& recognizer_;
    domain::IMqttPublisher& mqtt_;
    IWorkerClock& clock_;
    IStoredImageDecoder& decoder_;
    std::shared_ptr<logging::JsonLinesLogger> logger_;
};

}  // namespace ocrservice::services::recognition::worker
