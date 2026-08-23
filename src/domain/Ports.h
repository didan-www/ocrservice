#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "Recognition.h"

namespace ocrservice::domain {

class ByteView final {
public:
    ByteView(const std::uint8_t* data, std::size_t size);
    const std::uint8_t* data() const noexcept;
    std::size_t size() const noexcept;

private:
    const std::uint8_t* data_;
    std::size_t size_;
};

class BgrImageView final {
public:
    BgrImageView(
        const std::uint8_t* data,
        std::size_t byteSize,
        std::size_t width,
        std::size_t height,
        std::size_t rowStride);

    const std::uint8_t* data() const noexcept;
    std::size_t byteSize() const noexcept;
    std::size_t width() const noexcept;
    std::size_t height() const noexcept;
    std::size_t rowStride() const noexcept;

private:
    const std::uint8_t* data_;
    std::size_t byteSize_;
    std::size_t width_;
    std::size_t height_;
    std::size_t rowStride_;
};

enum class ImageFormat { jpeg, png };
enum class ImageMime { jpeg, png };
ImageMime mimeFor(ImageFormat format);
std::string_view toString(ImageMime mime);

class SaveImageCommand final {
public:
    SaveImageCommand(
        RecognitionId recognitionId,
        UtcTimePoint capturedAtUtc,
        ImageFormat format,
        ByteView compressedBytes,
        Sha256Digest digest);

    const RecognitionId& recognitionId() const noexcept;
    UtcTimePoint capturedAtUtc() const noexcept;
    ImageFormat format() const noexcept;
    ByteView compressedBytes() const noexcept;
    const Sha256Digest& digest() const noexcept;

private:
    RecognitionId recognitionId_;
    UtcTimePoint capturedAtUtc_;
    ImageFormat format_;
    ByteView compressedBytes_;
    Sha256Digest digest_;
};

class StoredImage final {
public:
    StoredImage(
        RelativeImagePath relativePath,
        ImageMime mime,
        std::uint64_t sizeBytes,
        Sha256Digest digest);

    const RelativeImagePath& relativePath() const noexcept;
    ImageMime mime() const noexcept;
    std::uint64_t sizeBytes() const noexcept;
    const Sha256Digest& digest() const noexcept;

private:
    RelativeImagePath relativePath_;
    ImageMime mime_;
    std::uint64_t sizeBytes_;
    Sha256Digest digest_;
};

class ImageReader {
public:
    virtual ~ImageReader() = default;
    virtual std::size_t read(std::uint8_t* destination, std::size_t capacity) = 0;
};

class ImageFile final {
public:
    ImageFile(std::unique_ptr<ImageReader> reader, ImageMime mime, std::uint64_t sizeBytes);
    ~ImageFile();
    ImageFile(ImageFile&&) noexcept;
    ImageFile& operator=(ImageFile&&) noexcept;
    ImageFile(const ImageFile&) = delete;
    ImageFile& operator=(const ImageFile&) = delete;

    ImageMime mime() const noexcept;
    std::uint64_t sizeBytes() const noexcept;
    std::size_t read(std::uint8_t* destination, std::size_t capacity);

private:
    std::unique_ptr<ImageReader> reader_;
    ImageMime mime_;
    std::uint64_t sizeBytes_;
};

class NewRecognition final {
public:
    NewRecognition(
        RecognitionId recognitionId,
        DeviceId deviceId,
        CaptureId captureId,
        Sha256Digest imageSha256,
        RelativeImagePath relativeImagePath,
        ImageMime imageMime,
        std::uint64_t imageSizeBytes,
        UtcTimePoint capturedAtUtc,
        UtcTimePoint startedAtUtc);

    const RecognitionId& recognitionId() const noexcept;
    const DeviceId& deviceId() const noexcept;
    const CaptureId& captureId() const noexcept;
    const Sha256Digest& imageSha256() const noexcept;
    const RelativeImagePath& relativeImagePath() const noexcept;
    ImageMime imageMime() const noexcept;
    std::uint64_t imageSizeBytes() const noexcept;
    UtcTimePoint capturedAtUtc() const noexcept;
    UtcTimePoint startedAtUtc() const noexcept;

private:
    RecognitionId recognitionId_;
    DeviceId deviceId_;
    CaptureId captureId_;
    Sha256Digest imageSha256_;
    RelativeImagePath relativeImagePath_;
    ImageMime imageMime_;
    std::uint64_t imageSizeBytes_;
    UtcTimePoint capturedAtUtc_;
    UtcTimePoint startedAtUtc_;
};

struct FinalizeRecognition final {
    RecognitionId recognitionId;
    RecognitionOutcome outcome;
    UtcTimePoint completedAtUtc;
    std::uint64_t durationMs;
};

class RecognitionRecord final {
public:
    RecognitionRecord(
        RecognitionSnapshot snapshot,
        CaptureId captureId,
        Sha256Digest imageSha256,
        RelativeImagePath relativeImagePath,
        ImageMime imageMime,
        std::uint64_t imageSizeBytes);

    const RecognitionSnapshot& snapshot() const noexcept;
    const CaptureId& captureId() const noexcept;
    const Sha256Digest& imageSha256() const noexcept;
    const RelativeImagePath& relativeImagePath() const noexcept;
    ImageMime imageMime() const noexcept;
    std::uint64_t imageSizeBytes() const noexcept;

private:
    RecognitionSnapshot snapshot_;
    CaptureId captureId_;
    Sha256Digest imageSha256_;
    RelativeImagePath relativeImagePath_;
    ImageMime imageMime_;
    std::uint64_t imageSizeBytes_;
};

class AdminUserRecord final {
public:
    AdminUserRecord(
        std::uint64_t id,
        std::string username,
        std::string displayName,
        std::string passwordHash,
        bool enabled);

    std::uint64_t id() const noexcept;
    const std::string& username() const noexcept;
    const std::string& displayName() const noexcept;
    const std::string& passwordHash() const noexcept;
    bool enabled() const noexcept;

private:
    std::uint64_t id_;
    std::string username_;
    std::string displayName_;
    std::string passwordHash_;
    bool enabled_;
};

class DeviceRecord final {
public:
    DeviceRecord(
        DeviceId deviceId,
        std::string deviceName,
        Sha256Digest httpTokenHash,
        std::string mqttUsername,
        bool enabled);

    const DeviceId& deviceId() const noexcept;
    const std::string& deviceName() const noexcept;
    const Sha256Digest& httpTokenHash() const noexcept;
    const std::string& mqttUsername() const noexcept;
    bool enabled() const noexcept;

private:
    DeviceId deviceId_;
    std::string deviceName_;
    Sha256Digest httpTokenHash_;
    std::string mqttUsername_;
    bool enabled_;
};

class AccessListRecord final {
public:
    AccessListRecord(
        std::uint64_t id,
        AccessListType listType,
        PlateNumber plateNumber,
        std::string remark,
        std::string createdBy,
        UtcTimePoint createdAt);

    std::uint64_t id() const noexcept;
    AccessListType listType() const noexcept;
    const PlateNumber& plateNumber() const noexcept;
    const std::string& remark() const noexcept;
    const std::string& createdBy() const noexcept;
    UtcTimePoint createdAt() const noexcept;

private:
    std::uint64_t id_;
    AccessListType listType_;
    PlateNumber plateNumber_;
    std::string remark_;
    std::string createdBy_;
    UtcTimePoint createdAt_;
};

class NewAccessListRecord final {
public:
    NewAccessListRecord(
        AccessListType listType,
        PlateNumber plateNumber,
        std::string remark,
        std::uint64_t createdByUserId,
        std::string createdBy,
        UtcTimePoint createdAt);

    AccessListType listType() const noexcept;
    const PlateNumber& plateNumber() const noexcept;
    const std::string& remark() const noexcept;
    std::uint64_t createdByUserId() const noexcept;
    const std::string& createdBy() const noexcept;
    UtcTimePoint createdAt() const noexcept;

private:
    AccessListType listType_;
    PlateNumber plateNumber_;
    std::string remark_;
    std::uint64_t createdByUserId_;
    std::string createdBy_;
    UtcTimePoint createdAt_;
};

template <typename T>
class PageResult final {
public:
    PageResult(std::vector<T> items, PageRequest request, const std::uint64_t total)
        : items_(std::move(items)), request_(request), total_(total) {
        requireNonNegativeJsonSafe(total_, "total");
        if (items_.size() > PageRequest::pageSize() || items_.size() > total_) {
            throw DomainError("page result has inconsistent item counts");
        }
    }

    const std::vector<T>& items() const noexcept { return items_; }
    const PageRequest& request() const noexcept { return request_; }
    std::uint64_t total() const noexcept { return total_; }

private:
    std::vector<T> items_;
    PageRequest request_;
    std::uint64_t total_;
};

class HistoryCursorResult final {
public:
    HistoryCursorResult(std::uint64_t recordsVisited, bool fullyConsumed);
    std::uint64_t recordsVisited() const noexcept;
    bool fullyConsumed() const noexcept;

private:
    std::uint64_t recordsVisited_;
    bool fullyConsumed_;
};

using HistoryVisitor = std::function<bool(const RecognitionRecord&)>;

enum class RepositoryFailure { unavailable, conflict, notFound, stateConflict, internal };
enum class StorageFailure {
    notFound,
    alreadyExists,
    invalidImage,
    writeFailed,
    readFailed,
    internal
};
enum class PublishFailure { notConnected, brokerRejected, transportError, stopping };

template <typename T, typename Error>
using PortResult = std::variant<T, Error>;

template <typename T>
using RepositoryResult = PortResult<T, RepositoryFailure>;
template <typename T>
using StorageResult = PortResult<T, StorageFailure>;

class IHistoryCursor {
public:
    virtual ~IHistoryCursor() = default;
    virtual RepositoryResult<std::optional<RecognitionRecord>> next() = 0;
};

class PublishAttempt final {
public:
    static PublishAttempt accepted() noexcept;
    static PublishAttempt rejected(PublishFailure failure);

    bool wasAccepted() const noexcept;
    const std::optional<PublishFailure>& failure() const noexcept;

private:
    explicit PublishAttempt(std::optional<PublishFailure> failure) noexcept;
    std::optional<PublishFailure> failure_;
};

class IRecognitionRepository {
public:
    virtual ~IRecognitionRepository() = default;
    virtual RepositoryResult<std::optional<RecognitionRecord>> findByCapture(
        const DeviceId& deviceId,
        const CaptureId& captureId) = 0;
    virtual RepositoryResult<std::optional<RecognitionRecord>> findById(
        const RecognitionId& recognitionId) = 0;
    virtual RepositoryResult<RecognitionRecord> insertProcessing(
        const NewRecognition& recognition) = 0;
    virtual RepositoryResult<RecognitionRecord> finalize(
        const FinalizeRecognition& recognition) = 0;
    virtual RepositoryResult<std::vector<RecognitionRecord>> failInterruptedOnStartup(
        UtcTimePoint completedAtUtc) = 0;
    virtual RepositoryResult<PageResult<RecognitionRecord>> queryHistory(
        const HistoryFilter& filter,
        const PageRequest& page) = 0;
    virtual RepositoryResult<std::unique_ptr<IHistoryCursor>> openHistoryCursor(
        const HistoryFilter& filter) = 0;
    virtual RepositoryResult<HistoryCursorResult> visitHistory(
        const HistoryFilter& filter,
        const HistoryVisitor& visitor) = 0;
};

class IAdminUserRepository {
public:
    virtual ~IAdminUserRepository() = default;
    virtual RepositoryResult<std::optional<AdminUserRecord>> findByUsername(
        std::string_view username) = 0;
};

class IDeviceRepository {
public:
    virtual ~IDeviceRepository() = default;
    virtual RepositoryResult<std::optional<DeviceRecord>> findByHttpTokenHash(
        const Sha256Digest& tokenHash) = 0;
};

class AccessListConflict final {
public:
    explicit AccessListConflict(AccessListType existingListType);
    AccessListType existingListType() const noexcept;

private:
    AccessListType existingListType_;
};

using AccessListInsertResult =
    std::variant<AccessListRecord, AccessListConflict, RepositoryFailure>;

class IAccessListRepository {
public:
    virtual ~IAccessListRepository() = default;
    virtual RepositoryResult<PageResult<AccessListRecord>> query(
        const AccessListFilter& filter,
        const PageRequest& page) = 0;
    virtual RepositoryResult<std::optional<AccessListRecord>> lookup(
        const PlateNumber& plateNumber) = 0;
    // AccessListType in the result identifies the existing conflicting list.
    virtual AccessListInsertResult insert(const NewAccessListRecord& record) = 0;
    virtual RepositoryResult<bool> remove(std::uint64_t id) = 0;
};

class IImageStorage {
public:
    virtual ~IImageStorage() = default;
    virtual StorageResult<StoredImage> saveAtomically(const SaveImageCommand& command) = 0;
    virtual StorageResult<ImageFile> openForRead(const RelativeImagePath& path) = 0;
    virtual void removeBestEffort(const RelativeImagePath& path) noexcept = 0;
};

class IPlateRecognizer {
public:
    virtual ~IPlateRecognizer() = default;
    virtual RecognitionOutcome recognize(BgrImageView bgrImage) = 0;
};

class IMqttPublisher {
public:
    virtual ~IMqttPublisher() = default;
    virtual PublishAttempt publishManagement(const RecognitionSnapshot& snapshot) = 0;
    virtual PublishAttempt publishDeviceFinal(
        const RecognitionSnapshot& snapshot,
        GateAction gateAction) = 0;
};

}  // namespace ocrservice::domain
