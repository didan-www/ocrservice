#pragma once

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace ocrservice::storage {

constexpr std::size_t kMaximumCompressedImageBytes = 10U * 1024U * 1024U;
constexpr std::size_t kMaximumImageDimension = 8192U;
constexpr std::size_t kMaximumImagePixels = 40000000U;

class ValidatedImage final {
public:
    ValidatedImage(
        domain::ImageFormat format,
        std::size_t width,
        std::size_t height,
        std::size_t sizeBytes,
        domain::Sha256Digest digest) noexcept;

    domain::ImageFormat format() const noexcept;
    std::size_t width() const noexcept;
    std::size_t height() const noexcept;
    std::size_t sizeBytes() const noexcept;
    const domain::Sha256Digest& digest() const noexcept;

private:
    domain::ImageFormat format_;
    std::size_t width_;
    std::size_t height_;
    std::size_t sizeBytes_;
    domain::Sha256Digest digest_;
};

class ImageValidator final {
public:
    domain::StorageResult<ValidatedImage> validate(domain::ByteView bytes) const noexcept;
};

namespace detail {
bool imageDimensionsWithinLimits(std::size_t width, std::size_t height) noexcept;
}  // namespace detail

}  // namespace ocrservice::storage
