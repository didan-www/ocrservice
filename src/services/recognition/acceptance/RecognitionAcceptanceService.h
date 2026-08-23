#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <variant>

#include "ImageValidator.h"
#include "Ports.h"
#include "RecognitionTaskQueue.h"

namespace ocrservice::logging {
class JsonLinesLogger;
}

namespace ocrservice::services::recognition::acceptance {

enum class AcceptanceFailure {
    deviceUnauthorized,
    deviceForbidden,
    deviceDisabled,
    captureIdConflict,
    imageTooLarge,
    imageInvalid,
    recognitionQueueFull,
    databaseUnavailable,
    imageStorageError,
    serviceUnavailable,
    internal
};

class DevicePrincipal final {
public:
    explicit DevicePrincipal(domain::DeviceRecord record);
    const domain::DeviceRecord& record() const noexcept;

private:
    domain::DeviceRecord record_;
};

class AuthorizedDevice final {
public:
    explicit AuthorizedDevice(domain::DeviceId deviceId);
    const domain::DeviceId& deviceId() const noexcept;

private:
    domain::DeviceId deviceId_;
};

class ValidatedUploadImage final {
public:
    ValidatedUploadImage(
        domain::ByteView bytes,
        domain::ImageFormat format,
        domain::Sha256Digest digest) noexcept;

    domain::ByteView bytes() const noexcept;
    domain::ImageFormat format() const noexcept;
    const domain::Sha256Digest& digest() const noexcept;

private:
    domain::ByteView bytes_;
    domain::ImageFormat format_;
    domain::Sha256Digest digest_;
};

struct AcceptanceSucceeded final {
    domain::RecognitionId recognitionId;
    domain::CaptureId captureId;
    domain::RecognitionStatus status;
};

using AuthenticationResult = std::variant<DevicePrincipal, AcceptanceFailure>;
using AuthorizationResult = std::variant<AuthorizedDevice, AcceptanceFailure>;
using ImageValidationResult = std::variant<ValidatedUploadImage, AcceptanceFailure>;
using AcceptanceResult = std::variant<AcceptanceSucceeded, AcceptanceFailure>;

class IUploadImageValidator {
public:
    virtual ~IUploadImageValidator() = default;
    virtual ImageValidationResult validate(std::string_view bytes) noexcept = 0;
};

class OpenCvUploadImageValidator final : public IUploadImageValidator {
public:
    ImageValidationResult validate(std::string_view bytes) noexcept override;

private:
    storage::ImageValidator validator_;
};

class IRecognitionIdGenerator {
public:
    virtual ~IRecognitionIdGenerator() = default;
    virtual std::optional<domain::RecognitionId> next() noexcept = 0;
};

class OpenSslRecognitionIdGenerator final : public IRecognitionIdGenerator {
public:
    std::optional<domain::RecognitionId> next() noexcept override;
};

class IAcceptanceClock {
public:
    virtual ~IAcceptanceClock() = default;
    virtual domain::UtcTimePoint nowUtc() = 0;
};

class SystemAcceptanceClock final : public IAcceptanceClock {
public:
    domain::UtcTimePoint nowUtc() override;
};

class IRecognitionAcceptanceService {
public:
    virtual ~IRecognitionAcceptanceService() = default;

    virtual AuthenticationResult authenticateToken(std::string_view token) = 0;
    virtual AuthorizationResult authorizeDevice(
        const DevicePrincipal& principal,
        const domain::DeviceId& pathDeviceId) = 0;
    virtual AcceptanceResult acceptUpload(
        const AuthorizedDevice& device,
        const domain::CaptureId& captureId,
        domain::UtcTimePoint capturedAtUtc,
        const ValidatedUploadImage& image,
        const domain::Uuid& requestId) = 0;
    virtual void stopAcceptingAndWait() = 0;
    virtual bool isAccepting() const = 0;
};

class RecognitionAcceptanceService final : public IRecognitionAcceptanceService {
public:
    RecognitionAcceptanceService(
        domain::IDeviceRepository& devices,
        domain::IRecognitionRepository& recognitions,
        domain::IImageStorage& images,
        domain::IMqttPublisher& mqtt,
        queue::RecognitionTaskQueue& queue,
        IRecognitionIdGenerator& recognitionIds,
        IAcceptanceClock& clock,
        std::shared_ptr<logging::JsonLinesLogger> logger = nullptr);

    AuthenticationResult authenticateToken(std::string_view token) override;
    AuthorizationResult authorizeDevice(
        const DevicePrincipal& principal,
        const domain::DeviceId& pathDeviceId) override;
    AcceptanceResult acceptUpload(
        const AuthorizedDevice& device,
        const domain::CaptureId& captureId,
        domain::UtcTimePoint capturedAtUtc,
        const ValidatedUploadImage& image,
        const domain::Uuid& requestId) override;
    void stopAcceptingAndWait() override;
    bool isAccepting() const override;

private:
    class InFlightGuard final {
    public:
        explicit InFlightGuard(RecognitionAcceptanceService& owner) noexcept;
        ~InFlightGuard();
        InFlightGuard(InFlightGuard&& other) noexcept;
        InFlightGuard(const InFlightGuard&) = delete;
        InFlightGuard& operator=(const InFlightGuard&) = delete;
        InFlightGuard& operator=(InFlightGuard&&) = delete;

    private:
        RecognitionAcceptanceService* owner_;
    };

    std::optional<InFlightGuard> tryEnter();
    void leave() noexcept;
    void logFailureNoThrow(
        std::string_view event,
        std::string_view code,
        const domain::Uuid& requestId,
        const domain::RecognitionId& recognitionId,
        const domain::DeviceId& deviceId) noexcept;

    domain::IDeviceRepository& devices_;
    domain::IRecognitionRepository& recognitions_;
    domain::IImageStorage& images_;
    domain::IMqttPublisher& mqtt_;
    queue::RecognitionTaskQueue& queue_;
    IRecognitionIdGenerator& recognitionIds_;
    IAcceptanceClock& clock_;
    std::shared_ptr<logging::JsonLinesLogger> logger_;
    mutable std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    bool accepting_ = true;
    std::size_t inFlight_ = 0U;
};

}  // namespace ocrservice::services::recognition::acceptance
