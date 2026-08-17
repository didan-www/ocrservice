#include "ImageValidator.h"

#include <array>
#include <limits>
#include <utility>

#include <openssl/evp.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

namespace ocrservice::storage {
namespace {

bool startsWithJpegMagic(const domain::ByteView bytes) noexcept {
    return bytes.size() >= 3U && bytes.data()[0] == 0xFFU && bytes.data()[1] == 0xD8U &&
           bytes.data()[2] == 0xFFU;
}

bool startsWithPngMagic(const domain::ByteView bytes) noexcept {
    constexpr std::array<std::uint8_t, 8> kPngSignature = {
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    if (bytes.size() < kPngSignature.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < kPngSignature.size(); ++index) {
        if (bytes.data()[index] != kPngSignature[index]) {
            return false;
        }
    }
    return true;
}

domain::StorageResult<domain::Sha256Digest> sha256(const domain::ByteView bytes) noexcept {
    std::array<std::uint8_t, 32> digest{};
    unsigned int digestLength = 0U;
    if (EVP_Digest(
            bytes.data(),
            bytes.size(),
            digest.data(),
            &digestLength,
            EVP_sha256(),
            nullptr) != 1 ||
        digestLength != static_cast<unsigned int>(digest.size())) {
        return domain::StorageFailure::internal;
    }
    return domain::Sha256Digest(digest);
}

}  // namespace

ValidatedImage::ValidatedImage(
    const domain::ImageFormat format,
    const std::size_t width,
    const std::size_t height,
    const std::size_t sizeBytes,
    domain::Sha256Digest digest) noexcept
    : format_(format),
      width_(width),
      height_(height),
      sizeBytes_(sizeBytes),
      digest_(std::move(digest)) {}

domain::ImageFormat ValidatedImage::format() const noexcept { return format_; }
std::size_t ValidatedImage::width() const noexcept { return width_; }
std::size_t ValidatedImage::height() const noexcept { return height_; }
std::size_t ValidatedImage::sizeBytes() const noexcept { return sizeBytes_; }
const domain::Sha256Digest& ValidatedImage::digest() const noexcept { return digest_; }

domain::StorageResult<ValidatedImage> ImageValidator::validate(
    const domain::ByteView bytes) const noexcept {
    if (bytes.size() > kMaximumCompressedImageBytes ||
        bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return domain::StorageFailure::invalidImage;
    }

    domain::ImageFormat format = domain::ImageFormat::jpeg;
    if (startsWithJpegMagic(bytes)) {
        format = domain::ImageFormat::jpeg;
    } else if (startsWithPngMagic(bytes)) {
        format = domain::ImageFormat::png;
    } else {
        return domain::StorageFailure::invalidImage;
    }

    try {
        const cv::Mat encoded(
            1,
            static_cast<int>(bytes.size()),
            CV_8UC1,
            const_cast<std::uint8_t*>(bytes.data()));
        const cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_COLOR);
        if (decoded.empty() || decoded.type() != CV_8UC3 || decoded.cols <= 0 ||
            decoded.rows <= 0) {
            return domain::StorageFailure::invalidImage;
        }
        const auto width = static_cast<std::size_t>(decoded.cols);
        const auto height = static_cast<std::size_t>(decoded.rows);
        if (!detail::imageDimensionsWithinLimits(width, height)) {
            return domain::StorageFailure::invalidImage;
        }
        auto digestResult = sha256(bytes);
        if (const auto* failure = std::get_if<domain::StorageFailure>(&digestResult)) {
            return *failure;
        }
        return ValidatedImage(
            format,
            width,
            height,
            bytes.size(),
            std::get<domain::Sha256Digest>(std::move(digestResult)));
    } catch (const cv::Exception& error) {
        if (error.code == cv::Error::StsNoMem) {
            return domain::StorageFailure::internal;
        }
        return domain::StorageFailure::invalidImage;
    } catch (...) {
        return domain::StorageFailure::internal;
    }
}

namespace detail {

bool imageDimensionsWithinLimits(const std::size_t width, const std::size_t height) noexcept {
    if (width == 0U || height == 0U || width > kMaximumImageDimension ||
        height > kMaximumImageDimension) {
        return false;
    }
    return width <= kMaximumImagePixels / height;
}

}  // namespace detail
}  // namespace ocrservice::storage
