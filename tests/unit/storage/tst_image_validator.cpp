#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <openssl/sha.h>

#include "ImageValidator.h"

namespace {

using ocrservice::domain::ByteView;
using ocrservice::domain::ImageFormat;
using ocrservice::domain::Sha256Digest;
using ocrservice::domain::StorageFailure;
using ocrservice::storage::ImageValidator;
using ocrservice::storage::ValidatedImage;

std::vector<std::uint8_t> encode(const std::string& extension) {
    cv::Mat image(3, 4, CV_8UC3, cv::Scalar(12, 34, 56));
    std::vector<unsigned char> encoded;
    if (!cv::imencode(extension, image, encoded)) {
        throw std::runtime_error("test image encoding failed");
    }
    return {encoded.begin(), encoded.end()};
}

const ValidatedImage& requireImage(
    const ocrservice::domain::StorageResult<ValidatedImage>& result) {
    EXPECT_TRUE(std::holds_alternative<ValidatedImage>(result));
    return std::get<ValidatedImage>(result);
}

void expectInvalid(const std::vector<std::uint8_t>& bytes) {
    const auto result = ImageValidator().validate(ByteView(bytes.data(), bytes.size()));
    ASSERT_TRUE(std::holds_alternative<StorageFailure>(result));
    EXPECT_EQ(std::get<StorageFailure>(result), StorageFailure::invalidImage);
}

Sha256Digest independentlyHash(const std::vector<std::uint8_t>& bytes) {
    std::array<std::uint8_t, SHA256_DIGEST_LENGTH> digest{};
    if (SHA256(bytes.data(), bytes.size(), digest.data()) == nullptr) {
        throw std::runtime_error("test SHA-256 failed");
    }
    return Sha256Digest(digest);
}

TEST(ImageValidatorTest, AcceptsRealJpegAndPngAndHashesOriginalCompressedBytes) {
    const auto jpeg = encode(".jpg");
    const auto png = encode(".png");
    const auto jpegResult = ImageValidator().validate(ByteView(jpeg.data(), jpeg.size()));
    const auto pngResult = ImageValidator().validate(ByteView(png.data(), png.size()));

    const auto& validatedJpeg = requireImage(jpegResult);
    EXPECT_EQ(validatedJpeg.format(), ImageFormat::jpeg);
    EXPECT_EQ(validatedJpeg.width(), 4U);
    EXPECT_EQ(validatedJpeg.height(), 3U);
    EXPECT_EQ(validatedJpeg.sizeBytes(), jpeg.size());
    EXPECT_EQ(validatedJpeg.digest(), independentlyHash(jpeg));

    const auto& validatedPng = requireImage(pngResult);
    EXPECT_EQ(validatedPng.format(), ImageFormat::png);
    EXPECT_EQ(validatedPng.width(), 4U);
    EXPECT_EQ(validatedPng.height(), 3U);
    EXPECT_EQ(validatedPng.digest(), independentlyHash(png));
}

TEST(ImageValidatorTest, RejectsUnsupportedMagicAndUndecodableClaimedMagic) {
    expectInvalid(encode(".bmp"));
    expectInvalid({0xFFU, 0xD8U, 0xFFU, 0x00U, 0x01U});
    expectInvalid({0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU, 0x00U});
}

TEST(ImageValidatorTest, AcceptsExactlyTenMebibytesAndRejectsOneByteMore) {
    auto exact = encode(".jpg");
    exact.resize(ocrservice::storage::kMaximumCompressedImageBytes, 0U);
    const auto accepted = ImageValidator().validate(ByteView(exact.data(), exact.size()));
    ASSERT_TRUE(std::holds_alternative<ValidatedImage>(accepted));
    EXPECT_EQ(std::get<ValidatedImage>(accepted).sizeBytes(), exact.size());

    exact.push_back(0U);
    expectInvalid(exact);
}

TEST(ImageValidatorTest, EnforcesDimensionAndPixelBoundariesWithoutOverflow) {
    using ocrservice::storage::detail::imageDimensionsWithinLimits;
    EXPECT_TRUE(imageDimensionsWithinLimits(8192U, 1U));
    EXPECT_TRUE(imageDimensionsWithinLimits(1U, 8192U));
    EXPECT_FALSE(imageDimensionsWithinLimits(8193U, 1U));
    EXPECT_FALSE(imageDimensionsWithinLimits(1U, 8193U));
    EXPECT_TRUE(imageDimensionsWithinLimits(8000U, 5000U));
    EXPECT_FALSE(imageDimensionsWithinLimits(8000U, 5001U));
    EXPECT_FALSE(imageDimensionsWithinLimits(0U, 1U));
    EXPECT_FALSE(imageDimensionsWithinLimits(1U, 0U));
    EXPECT_FALSE(imageDimensionsWithinLimits(
        std::numeric_limits<std::size_t>::max(),
        std::numeric_limits<std::size_t>::max()));
}

TEST(ImageValidatorTest, DeterminesFormatFromBytesInsteadOfExternalMimeClaims) {
    const auto png = encode(".png");
    const auto result = ImageValidator().validate(ByteView(png.data(), png.size()));
    ASSERT_TRUE(std::holds_alternative<ValidatedImage>(result));
    EXPECT_EQ(std::get<ValidatedImage>(result).format(), ImageFormat::png);
}

}  // namespace
