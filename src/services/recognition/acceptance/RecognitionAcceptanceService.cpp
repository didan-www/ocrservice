#include "RecognitionAcceptanceService.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "LoggerFactory.h"

namespace ocrservice::services::recognition::acceptance {
namespace {

constexpr std::size_t kMaximumRecognitionIdCandidates = 8U;

class StoredImageCleanup final {
public:
    StoredImageCleanup(
        domain::IImageStorage& images,
        const domain::RelativeImagePath& path) noexcept
        : images_(&images), path_(&path) {}

    ~StoredImageCleanup() {
        if (images_ != nullptr) {
            images_->removeBestEffort(*path_);
        }
    }

    StoredImageCleanup(const StoredImageCleanup&) = delete;
    StoredImageCleanup& operator=(const StoredImageCleanup&) = delete;

    void disarm() noexcept { images_ = nullptr; }

private:
    domain::IImageStorage* images_;
    const domain::RelativeImagePath* path_;
};

static_assert(std::is_nothrow_constructible_v<
              StoredImageCleanup,
              domain::IImageStorage&,
              const domain::RelativeImagePath&>);

AcceptanceFailure mapRepositoryFailure(const domain::RepositoryFailure failure) noexcept {
    return failure == domain::RepositoryFailure::unavailable ?
               AcceptanceFailure::databaseUnavailable : AcceptanceFailure::internal;
}

AcceptanceFailure mapStorageFailure(const domain::StorageFailure failure) noexcept {
    switch (failure) {
        case domain::StorageFailure::writeFailed:
            return AcceptanceFailure::imageStorageError;
        case domain::StorageFailure::invalidImage:
            return AcceptanceFailure::imageInvalid;
        case domain::StorageFailure::notFound:
        case domain::StorageFailure::alreadyExists:
        case domain::StorageFailure::readFailed:
        case domain::StorageFailure::internal:
            return AcceptanceFailure::internal;
    }
    return AcceptanceFailure::internal;
}

enum class TokenDigestFailure { invalidShape, internal };

using TokenDigestResult = std::variant<domain::Sha256Digest, TokenDigestFailure>;

TokenDigestResult tokenDigest(const std::string_view token) noexcept {
    if (token.empty() || token.size() > 256U) {
        return TokenDigestFailure::invalidShape;
    }
    for (const char rawCharacter : token) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        if (character < 0x21U || character > 0x7EU || character == 0x2CU) {
            return TokenDigestFailure::invalidShape;
        }
    }
    std::array<std::uint8_t, 32> digest{};
    unsigned int digestLength = 0U;
    if (EVP_Digest(
            token.data(),
            token.size(),
            digest.data(),
            &digestLength,
            EVP_sha256(),
            nullptr) != 1 ||
        digestLength != digest.size()) {
        return TokenDigestFailure::internal;
    }
    return domain::Sha256Digest(digest);
}

bool constantTimeEqual(
    const domain::Sha256Digest& left,
    const domain::Sha256Digest& right) noexcept {
    return CRYPTO_memcmp(left.bytes().data(), right.bytes().data(), left.bytes().size()) == 0;
}

bool isSameUpload(
    const domain::RecognitionRecord& record,
    const ValidatedUploadImage& image,
    const domain::UtcTimePoint capturedAtUtc) noexcept {
    return constantTimeEqual(record.imageSha256(), image.digest()) &&
           record.snapshot().capturedAt() == capturedAtUtc;
}

AcceptanceResult existingResult(
    const domain::RecognitionRecord& record,
    const domain::CaptureId& captureId,
    const ValidatedUploadImage& image,
    const domain::UtcTimePoint capturedAtUtc) {
    if (!isSameUpload(record, image, capturedAtUtc)) {
        return AcceptanceFailure::captureIdConflict;
    }
    return AcceptanceSucceeded{
        record.snapshot().recognitionId(), captureId, record.snapshot().status()};
}

}  // namespace

DevicePrincipal::DevicePrincipal(domain::DeviceRecord record) : record_(std::move(record)) {}
const domain::DeviceRecord& DevicePrincipal::record() const noexcept { return record_; }

AuthorizedDevice::AuthorizedDevice(domain::DeviceId deviceId)
    : deviceId_(std::move(deviceId)) {}
const domain::DeviceId& AuthorizedDevice::deviceId() const noexcept { return deviceId_; }

ValidatedUploadImage::ValidatedUploadImage(
    const domain::ByteView bytes,
    const domain::ImageFormat format,
    domain::Sha256Digest digest) noexcept
    : bytes_(bytes), format_(format), digest_(std::move(digest)) {}
domain::ByteView ValidatedUploadImage::bytes() const noexcept { return bytes_; }
domain::ImageFormat ValidatedUploadImage::format() const noexcept { return format_; }
const domain::Sha256Digest& ValidatedUploadImage::digest() const noexcept { return digest_; }

ImageValidationResult OpenCvUploadImageValidator::validate(
    const std::string_view bytes) noexcept {
    if (bytes.size() > storage::kMaximumCompressedImageBytes) {
        return AcceptanceFailure::imageTooLarge;
    }
    if (bytes.empty()) {
        return AcceptanceFailure::imageInvalid;
    }
    try {
        const domain::ByteView view(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        auto result = validator_.validate(view);
        if (const auto* failure = std::get_if<domain::StorageFailure>(&result)) {
            return *failure == domain::StorageFailure::internal ?
                       AcceptanceFailure::internal : AcceptanceFailure::imageInvalid;
        }
        const auto& image = std::get<storage::ValidatedImage>(result);
        return ValidatedUploadImage(view, image.format(), image.digest());
    } catch (...) {
        return AcceptanceFailure::internal;
    }
}

std::optional<domain::RecognitionId> OpenSslRecognitionIdGenerator::next() noexcept {
    std::array<std::uint8_t, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        return std::nullopt;
    }
    try {
        return domain::RecognitionId(domain::Uuid::v4(bytes));
    } catch (...) {
        return std::nullopt;
    }
}

domain::UtcTimePoint SystemAcceptanceClock::nowUtc() {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return domain::UtcTimePoint(milliseconds.count());
}

RecognitionAcceptanceService::RecognitionAcceptanceService(
    domain::IDeviceRepository& devices,
    domain::IRecognitionRepository& recognitions,
    domain::IImageStorage& images,
    domain::IMqttPublisher& mqtt,
    queue::RecognitionTaskQueue& queue,
    IRecognitionIdGenerator& recognitionIds,
    IAcceptanceClock& clock,
    std::shared_ptr<logging::JsonLinesLogger> logger)
    : devices_(devices),
      recognitions_(recognitions),
      images_(images),
      mqtt_(mqtt),
      queue_(queue),
      recognitionIds_(recognitionIds),
      clock_(clock),
      logger_(std::move(logger)) {}

AuthenticationResult RecognitionAcceptanceService::authenticateToken(
    const std::string_view token) {
    auto digestResult = tokenDigest(token);
    if (const auto* failure = std::get_if<TokenDigestFailure>(&digestResult)) {
        return *failure == TokenDigestFailure::invalidShape ?
                   AcceptanceFailure::deviceUnauthorized :
                   AcceptanceFailure::internal;
    }
    const auto& digest = std::get<domain::Sha256Digest>(digestResult);
    try {
        auto result = devices_.findByHttpTokenHash(digest);
        if (const auto* failure = std::get_if<domain::RepositoryFailure>(&result)) {
            return mapRepositoryFailure(*failure);
        }
        auto record = std::get<std::optional<domain::DeviceRecord>>(std::move(result));
        if (!record || !constantTimeEqual(record->httpTokenHash(), digest)) {
            return AcceptanceFailure::deviceUnauthorized;
        }
        return DevicePrincipal(std::move(*record));
    } catch (...) {
        return AcceptanceFailure::internal;
    }
}

AuthorizationResult RecognitionAcceptanceService::authorizeDevice(
    const DevicePrincipal& principal,
    const domain::DeviceId& pathDeviceId) {
    if (!(principal.record().deviceId() == pathDeviceId)) {
        return AcceptanceFailure::deviceForbidden;
    }
    if (!principal.record().enabled()) {
        return AcceptanceFailure::deviceDisabled;
    }
    return AuthorizedDevice(pathDeviceId);
}

RecognitionAcceptanceService::InFlightGuard::InFlightGuard(
    RecognitionAcceptanceService& owner) noexcept
    : owner_(&owner) {}

RecognitionAcceptanceService::InFlightGuard::~InFlightGuard() {
    if (owner_ != nullptr) {
        owner_->leave();
    }
}

RecognitionAcceptanceService::InFlightGuard::InFlightGuard(InFlightGuard&& other) noexcept
    : owner_(other.owner_) {
    other.owner_ = nullptr;
}

std::optional<RecognitionAcceptanceService::InFlightGuard>
RecognitionAcceptanceService::tryEnter() {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    if (!accepting_) {
        return std::nullopt;
    }
    ++inFlight_;
    return InFlightGuard(*this);
}

void RecognitionAcceptanceService::leave() noexcept {
    try {
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            if (inFlight_ != 0U) {
                --inFlight_;
            }
        }
        lifecycleChanged_.notify_all();
    } catch (...) {
    }
}

void RecognitionAcceptanceService::logFailureNoThrow(
    const std::string_view event,
    const std::string_view code,
    const domain::Uuid& requestId,
    const domain::RecognitionId& recognitionId,
    const domain::DeviceId& deviceId) noexcept {
    if (!logger_) {
        return;
    }
    try {
        logger_->log(
            logging::LogLevel::warning,
            logging::LogEvent(
                "recognition_acceptance",
                std::string(event),
                std::string(code),
                requestId.toString(),
                recognitionId.toString(),
                deviceId.value()));
    } catch (...) {
    }
}

AcceptanceResult RecognitionAcceptanceService::acceptUpload(
    const AuthorizedDevice& device,
    const domain::CaptureId& captureId,
    const domain::UtcTimePoint capturedAtUtc,
    const ValidatedUploadImage& image,
    const domain::Uuid& requestId) {
    auto inFlight = tryEnter();
    if (!inFlight) {
        return AcceptanceFailure::serviceUnavailable;
    }

    try {
        auto existing = recognitions_.findByCapture(device.deviceId(), captureId);
        if (const auto* failure = std::get_if<domain::RepositoryFailure>(&existing)) {
            return mapRepositoryFailure(*failure);
        }
        auto existingRecord =
            std::get<std::optional<domain::RecognitionRecord>>(std::move(existing));
        if (existingRecord) {
            return existingResult(*existingRecord, captureId, image, capturedAtUtc);
        }

        auto reservation = queue_.tryReserve();
        if (!reservation) {
            return queue_.isAccepting() ? AcceptanceFailure::recognitionQueueFull :
                                         AcceptanceFailure::serviceUnavailable;
        }
        const auto startedAtUtc = clock_.nowUtc();

        for (std::size_t attempt = 0U; attempt < kMaximumRecognitionIdCandidates; ++attempt) {
            const auto recognitionId = recognitionIds_.next();
            if (!recognitionId) {
                return AcceptanceFailure::internal;
            }

            auto idLookup = recognitions_.findById(*recognitionId);
            if (const auto* failure = std::get_if<domain::RepositoryFailure>(&idLookup)) {
                return mapRepositoryFailure(*failure);
            }
            if (std::get<std::optional<domain::RecognitionRecord>>(idLookup)) {
                continue;
            }

            const domain::SaveImageCommand saveCommand(
                *recognitionId,
                capturedAtUtc,
                image.format(),
                image.bytes(),
                image.digest());
            auto savedResult = images_.saveAtomically(saveCommand);
            if (const auto* failure = std::get_if<domain::StorageFailure>(&savedResult)) {
                if (*failure == domain::StorageFailure::alreadyExists) {
                    continue;
                }
                return mapStorageFailure(*failure);
            }
            auto stored = std::get<domain::StoredImage>(std::move(savedResult));
            StoredImageCleanup imageCleanup(images_, stored.relativePath());

            auto insertResult = recognitions_.insertProcessing(domain::NewRecognition(
                *recognitionId,
                device.deviceId(),
                captureId,
                image.digest(),
                stored.relativePath(),
                stored.mime(),
                stored.sizeBytes(),
                capturedAtUtc,
                startedAtUtc));
            if (const auto* record = std::get_if<domain::RecognitionRecord>(&insertResult)) {
                imageCleanup.disarm();
                auto gate = reservation->commit(domain::RecognitionTask(*recognitionId));
                try {
                    const auto publish = mqtt_.publishManagement(record->snapshot());
                    if (!publish.wasAccepted()) {
                        logFailureNoThrow(
                            "processing_publish_failed",
                            "MQTT_PUBLISH_FAILED",
                            requestId,
                            *recognitionId,
                            device.deviceId());
                    }
                } catch (...) {
                    logFailureNoThrow(
                        "processing_publish_exception",
                        "MQTT_PUBLISH_FAILED",
                        requestId,
                        *recognitionId,
                        device.deviceId());
                }
                try {
                    gate.open();
                } catch (...) {
                    logFailureNoThrow(
                        "start_gate_open_failed",
                        "INTERNAL_ERROR",
                        requestId,
                        *recognitionId,
                        device.deviceId());
                }
                return AcceptanceSucceeded{
                    record->snapshot().recognitionId(), captureId, record->snapshot().status()};
            }

            const auto insertFailure = std::get<domain::RepositoryFailure>(insertResult);
            if (insertFailure != domain::RepositoryFailure::conflict) {
                return mapRepositoryFailure(insertFailure);
            }

            auto winner = recognitions_.findByCapture(device.deviceId(), captureId);
            if (const auto* failure = std::get_if<domain::RepositoryFailure>(&winner)) {
                return mapRepositoryFailure(*failure);
            }
            auto winnerRecord =
                std::get<std::optional<domain::RecognitionRecord>>(std::move(winner));
            if (winnerRecord) {
                return existingResult(*winnerRecord, captureId, image, capturedAtUtc);
            }

            auto collision = recognitions_.findById(*recognitionId);
            if (const auto* failure = std::get_if<domain::RepositoryFailure>(&collision)) {
                return mapRepositoryFailure(*failure);
            }
            if (std::get<std::optional<domain::RecognitionRecord>>(collision)) {
                continue;
            }
            return AcceptanceFailure::internal;
        }
        return AcceptanceFailure::internal;
    } catch (...) {
        return AcceptanceFailure::internal;
    }
}

void RecognitionAcceptanceService::stopAcceptingAndWait() {
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        accepting_ = false;
    }
    queue_.stopAccepting();
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    lifecycleChanged_.wait(lock, [this] { return inFlight_ == 0U; });
}

bool RecognitionAcceptanceService::isAccepting() const {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    return accepting_;
}

}  // namespace ocrservice::services::recognition::acceptance
