#include "Ports.h"

#include <limits>
#include <utility>

#include "Utf8.h"

namespace ocrservice::domain {
namespace {

void requireUnicodeLength(
    const std::string_view value,
    const std::size_t minimum,
    const std::size_t maximum,
    const std::string_view fieldName) {
    try {
        const auto length = text::countCodePoints(value);
        if (length < minimum || length > maximum) {
            throw DomainError(std::string(fieldName) + " has an invalid length");
        }
    } catch (const text::Utf8Error& error) {
        throw DomainError(error.what());
    }
}

void requireAsciiLength(
    const std::string_view value,
    const std::size_t minimum,
    const std::size_t maximum,
    const std::string_view fieldName) {
    if (value.size() < minimum || value.size() > maximum) {
        throw DomainError(std::string(fieldName) + " has an invalid length");
    }
    for (const char rawCharacter : value) {
        const auto character = static_cast<unsigned char>(rawCharacter);
        if (character < 0x21U || character > 0x7EU) {
            throw DomainError(std::string(fieldName) + " must contain visible ASCII");
        }
    }
}

}  // namespace

ByteView::ByteView(const std::uint8_t* data, const std::size_t size) : data_(data), size_(size) {
    if (data_ == nullptr || size_ == 0U) {
        throw DomainError("byte view must not be empty");
    }
}
const std::uint8_t* ByteView::data() const noexcept { return data_; }
std::size_t ByteView::size() const noexcept { return size_; }

BgrImageView::BgrImageView(
    const std::uint8_t* data,
    const std::size_t byteSize,
    const std::size_t width,
    const std::size_t height,
    const std::size_t rowStride)
    : data_(data),
      byteSize_(byteSize),
      width_(width),
      height_(height),
      rowStride_(rowStride) {
    if (data_ == nullptr || width_ == 0U || height_ == 0U ||
        width_ > std::numeric_limits<std::size_t>::max() / 3U) {
        throw DomainError("BGR image view has invalid dimensions");
    }
    const auto rowBytes = width_ * 3U;
    if (rowStride_ < rowBytes ||
        (height_ > 1U && rowStride_ > (std::numeric_limits<std::size_t>::max() - rowBytes) /
                                          (height_ - 1U))) {
        throw DomainError("BGR image view has invalid row stride");
    }
    const auto requiredBytes = (height_ - 1U) * rowStride_ + rowBytes;
    if (byteSize_ < requiredBytes) {
        throw DomainError("BGR image view buffer is too small");
    }
}
const std::uint8_t* BgrImageView::data() const noexcept { return data_; }
std::size_t BgrImageView::byteSize() const noexcept { return byteSize_; }
std::size_t BgrImageView::width() const noexcept { return width_; }
std::size_t BgrImageView::height() const noexcept { return height_; }
std::size_t BgrImageView::rowStride() const noexcept { return rowStride_; }

ImageMime mimeFor(const ImageFormat format) {
    switch (format) {
        case ImageFormat::jpeg:
            return ImageMime::jpeg;
        case ImageFormat::png:
            return ImageMime::png;
    }
    throw DomainError("invalid image format");
}

std::string_view toString(const ImageMime mime) {
    switch (mime) {
        case ImageMime::jpeg:
            return "image/jpeg";
        case ImageMime::png:
            return "image/png";
    }
    throw DomainError("invalid image MIME");
}

SaveImageCommand::SaveImageCommand(
    RecognitionId recognitionId,
    const UtcTimePoint capturedAtUtc,
    const ImageFormat format,
    const ByteView compressedBytes,
    Sha256Digest digest)
    : recognitionId_(std::move(recognitionId)),
      capturedAtUtc_(capturedAtUtc),
      format_(format),
      compressedBytes_(compressedBytes),
      digest_(std::move(digest)) {
    (void)mimeFor(format_);
}
const RecognitionId& SaveImageCommand::recognitionId() const noexcept { return recognitionId_; }
UtcTimePoint SaveImageCommand::capturedAtUtc() const noexcept { return capturedAtUtc_; }
ImageFormat SaveImageCommand::format() const noexcept { return format_; }
ByteView SaveImageCommand::compressedBytes() const noexcept { return compressedBytes_; }
const Sha256Digest& SaveImageCommand::digest() const noexcept { return digest_; }

StoredImage::StoredImage(
    RelativeImagePath relativePath,
    const ImageMime mime,
    const std::uint64_t sizeBytes,
    Sha256Digest digest)
    : relativePath_(std::move(relativePath)),
      mime_(mime),
      sizeBytes_(sizeBytes),
      digest_(std::move(digest)) {
    (void)toString(mime_);
    requirePositiveJsonSafe(sizeBytes_, "imageSizeBytes");
}
const RelativeImagePath& StoredImage::relativePath() const noexcept { return relativePath_; }
ImageMime StoredImage::mime() const noexcept { return mime_; }
std::uint64_t StoredImage::sizeBytes() const noexcept { return sizeBytes_; }
const Sha256Digest& StoredImage::digest() const noexcept { return digest_; }

ImageFile::ImageFile(
    std::unique_ptr<ImageReader> reader,
    const ImageMime mime,
    const std::uint64_t sizeBytes)
    : reader_(std::move(reader)), mime_(mime), sizeBytes_(sizeBytes) {
    if (!reader_) {
        throw DomainError("image reader must not be null");
    }
    (void)toString(mime_);
    requireNonNegativeJsonSafe(sizeBytes_, "imageSizeBytes");
}
ImageFile::~ImageFile() = default;
ImageFile::ImageFile(ImageFile&&) noexcept = default;
ImageFile& ImageFile::operator=(ImageFile&&) noexcept = default;
ImageMime ImageFile::mime() const noexcept { return mime_; }
std::uint64_t ImageFile::sizeBytes() const noexcept { return sizeBytes_; }
std::size_t ImageFile::read(std::uint8_t* destination, const std::size_t capacity) {
    if (destination == nullptr && capacity != 0U) {
        throw DomainError("image read destination must not be null");
    }
    return reader_->read(destination, capacity);
}

NewRecognition::NewRecognition(
    RecognitionId recognitionId,
    DeviceId deviceId,
    CaptureId captureId,
    Sha256Digest imageSha256,
    RelativeImagePath relativeImagePath,
    const ImageMime imageMime,
    const std::uint64_t imageSizeBytes,
    const UtcTimePoint capturedAtUtc,
    const UtcTimePoint startedAtUtc)
    : recognitionId_(std::move(recognitionId)),
      deviceId_(std::move(deviceId)),
      captureId_(std::move(captureId)),
      imageSha256_(std::move(imageSha256)),
      relativeImagePath_(std::move(relativeImagePath)),
      imageMime_(imageMime),
      imageSizeBytes_(imageSizeBytes),
      capturedAtUtc_(capturedAtUtc),
      startedAtUtc_(startedAtUtc) {
    (void)toString(imageMime_);
    requirePositiveJsonSafe(imageSizeBytes_, "imageSizeBytes");
}
const RecognitionId& NewRecognition::recognitionId() const noexcept { return recognitionId_; }
const DeviceId& NewRecognition::deviceId() const noexcept { return deviceId_; }
const CaptureId& NewRecognition::captureId() const noexcept { return captureId_; }
const Sha256Digest& NewRecognition::imageSha256() const noexcept { return imageSha256_; }
const RelativeImagePath& NewRecognition::relativeImagePath() const noexcept {
    return relativeImagePath_;
}
ImageMime NewRecognition::imageMime() const noexcept { return imageMime_; }
std::uint64_t NewRecognition::imageSizeBytes() const noexcept { return imageSizeBytes_; }
UtcTimePoint NewRecognition::capturedAtUtc() const noexcept { return capturedAtUtc_; }
UtcTimePoint NewRecognition::startedAtUtc() const noexcept { return startedAtUtc_; }

RecognitionRecord::RecognitionRecord(
    RecognitionSnapshot snapshot,
    CaptureId captureId,
    Sha256Digest imageSha256,
    RelativeImagePath relativeImagePath,
    const ImageMime imageMime,
    const std::uint64_t imageSizeBytes)
    : snapshot_(std::move(snapshot)),
      captureId_(std::move(captureId)),
      imageSha256_(std::move(imageSha256)),
      relativeImagePath_(std::move(relativeImagePath)),
      imageMime_(imageMime),
      imageSizeBytes_(imageSizeBytes) {
    (void)toString(imageMime_);
    requirePositiveJsonSafe(imageSizeBytes_, "imageSizeBytes");
}
const RecognitionSnapshot& RecognitionRecord::snapshot() const noexcept { return snapshot_; }
const CaptureId& RecognitionRecord::captureId() const noexcept { return captureId_; }
const Sha256Digest& RecognitionRecord::imageSha256() const noexcept { return imageSha256_; }
const RelativeImagePath& RecognitionRecord::relativeImagePath() const noexcept {
    return relativeImagePath_;
}
ImageMime RecognitionRecord::imageMime() const noexcept { return imageMime_; }
std::uint64_t RecognitionRecord::imageSizeBytes() const noexcept { return imageSizeBytes_; }

PublishAttempt::PublishAttempt(std::optional<PublishFailure> failure) noexcept
    : failure_(failure) {}
PublishAttempt PublishAttempt::accepted() noexcept { return PublishAttempt(std::nullopt); }
PublishAttempt PublishAttempt::rejected(const PublishFailure failure) {
    switch (failure) {
        case PublishFailure::notConnected:
        case PublishFailure::brokerRejected:
        case PublishFailure::transportError:
        case PublishFailure::stopping:
            break;
        default:
            throw DomainError("invalid publish failure");
    }
    return PublishAttempt(failure);
}
bool PublishAttempt::wasAccepted() const noexcept { return !failure_.has_value(); }
const std::optional<PublishFailure>& PublishAttempt::failure() const noexcept { return failure_; }

AdminUserRecord::AdminUserRecord(
    const std::uint64_t id,
    std::string username,
    std::string displayName,
    std::string passwordHash,
    const bool enabled)
    : id_(id),
      username_(std::move(username)),
      displayName_(std::move(displayName)),
      passwordHash_(std::move(passwordHash)),
      enabled_(enabled) {
    requirePositiveJsonSafe(id_, "adminUserId");
    requireUnicodeLength(username_, 1U, 64U, "username");
    requireUnicodeLength(displayName_, 1U, 64U, "displayName");
    requireAsciiLength(passwordHash_, 1U, 100U, "passwordHash");
}
std::uint64_t AdminUserRecord::id() const noexcept { return id_; }
const std::string& AdminUserRecord::username() const noexcept { return username_; }
const std::string& AdminUserRecord::displayName() const noexcept { return displayName_; }
const std::string& AdminUserRecord::passwordHash() const noexcept { return passwordHash_; }
bool AdminUserRecord::enabled() const noexcept { return enabled_; }

DeviceRecord::DeviceRecord(
    DeviceId deviceId,
    std::string deviceName,
    Sha256Digest httpTokenHash,
    std::string mqttUsername,
    const bool enabled)
    : deviceId_(std::move(deviceId)),
      deviceName_(std::move(deviceName)),
      httpTokenHash_(std::move(httpTokenHash)),
      mqttUsername_(std::move(mqttUsername)),
      enabled_(enabled) {
    requireUnicodeLength(deviceName_, 1U, 100U, "deviceName");
    requireAsciiLength(mqttUsername_, 1U, 64U, "mqttUsername");
}
const DeviceId& DeviceRecord::deviceId() const noexcept { return deviceId_; }
const std::string& DeviceRecord::deviceName() const noexcept { return deviceName_; }
const Sha256Digest& DeviceRecord::httpTokenHash() const noexcept { return httpTokenHash_; }
const std::string& DeviceRecord::mqttUsername() const noexcept { return mqttUsername_; }
bool DeviceRecord::enabled() const noexcept { return enabled_; }

AccessListRecord::AccessListRecord(
    const std::uint64_t id,
    const AccessListType listType,
    PlateNumber plateNumber,
    std::string remark,
    std::string createdBy,
    const UtcTimePoint createdAt)
    : id_(id),
      listType_(listType),
      plateNumber_(std::move(plateNumber)),
      remark_(std::move(remark)),
      createdBy_(std::move(createdBy)),
      createdAt_(createdAt) {
    requirePositiveJsonSafe(id_, "accessListId");
    (void)toString(listType_);
    requireUnicodeLength(remark_, 0U, 200U, "remark");
    requireUnicodeLength(createdBy_, 1U, 64U, "createdBy");
}
std::uint64_t AccessListRecord::id() const noexcept { return id_; }
AccessListType AccessListRecord::listType() const noexcept { return listType_; }
const PlateNumber& AccessListRecord::plateNumber() const noexcept { return plateNumber_; }
const std::string& AccessListRecord::remark() const noexcept { return remark_; }
const std::string& AccessListRecord::createdBy() const noexcept { return createdBy_; }
UtcTimePoint AccessListRecord::createdAt() const noexcept { return createdAt_; }

NewAccessListRecord::NewAccessListRecord(
    const AccessListType listType,
    PlateNumber plateNumber,
    std::string remark,
    const std::uint64_t createdByUserId,
    std::string createdBy,
    const UtcTimePoint createdAt)
    : listType_(listType),
      plateNumber_(std::move(plateNumber)),
      remark_(std::move(remark)),
      createdByUserId_(createdByUserId),
      createdBy_(std::move(createdBy)),
      createdAt_(createdAt) {
    (void)toString(listType_);
    requirePositiveJsonSafe(createdByUserId_, "createdByUserId");
    requireUnicodeLength(remark_, 0U, 200U, "remark");
    requireUnicodeLength(createdBy_, 1U, 64U, "createdBy");
}
AccessListType NewAccessListRecord::listType() const noexcept { return listType_; }
const PlateNumber& NewAccessListRecord::plateNumber() const noexcept { return plateNumber_; }
const std::string& NewAccessListRecord::remark() const noexcept { return remark_; }
std::uint64_t NewAccessListRecord::createdByUserId() const noexcept { return createdByUserId_; }
const std::string& NewAccessListRecord::createdBy() const noexcept { return createdBy_; }
UtcTimePoint NewAccessListRecord::createdAt() const noexcept { return createdAt_; }

HistoryCursorResult::HistoryCursorResult(
    const std::uint64_t recordsVisited,
    const bool fullyConsumed)
    : recordsVisited_(recordsVisited), fullyConsumed_(fullyConsumed) {
    requireNonNegativeJsonSafe(recordsVisited_, "recordsVisited");
}
std::uint64_t HistoryCursorResult::recordsVisited() const noexcept { return recordsVisited_; }
bool HistoryCursorResult::fullyConsumed() const noexcept { return fullyConsumed_; }

AccessListConflict::AccessListConflict(const AccessListType existingListType)
    : existingListType_(existingListType) {
    (void)toString(existingListType_);
}
AccessListType AccessListConflict::existingListType() const noexcept {
    return existingListType_;
}

}  // namespace ocrservice::domain
