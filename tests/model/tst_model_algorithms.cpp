#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "ModelAlgorithms.h"

namespace {

using ocrservice::domain::BgrImageView;
using ocrservice::model::detail::Detection;
using ocrservice::model::detail::LetterboxTransform;

TEST(ModelPreprocessingTest, LetterboxesWithRgbUnitRangeNchwAndCenteredPadding) {
    constexpr std::size_t width = 4U;
    constexpr std::size_t height = 2U;
    constexpr std::size_t stride = 14U;
    std::vector<std::uint8_t> pixels(stride * height, 0xEEU);
    for (std::size_t row = 0U; row < height; ++row) {
        for (std::size_t column = 0U; column < width; ++column) {
            const std::size_t offset = row * stride + column * 3U;
            pixels[offset] = 10U;
            pixels[offset + 1U] = 20U;
            pixels[offset + 2U] = 30U;
        }
    }
    const BgrImageView image(
        pixels.data(), (height - 1U) * stride + width * 3U, width, height, stride);

    LetterboxTransform transform{};
    const auto input = ocrservice::model::detail::makeYoloInput(image, transform);

    constexpr std::size_t side = ocrservice::model::detail::kYoloInputSize;
    constexpr std::size_t planeSize = side * side;
    ASSERT_EQ(input.size(), 3U * planeSize);
    EXPECT_FLOAT_EQ(transform.scale, 160.0F);
    EXPECT_FLOAT_EQ(transform.padX, 0.0F);
    EXPECT_FLOAT_EQ(transform.padY, 160.0F);
    EXPECT_EQ(transform.resizedWidth, 640U);
    EXPECT_EQ(transform.resizedHeight, 320U);

    const std::size_t paddingOffset = 0U;
    const std::size_t imageOffset = 160U * side;
    EXPECT_FLOAT_EQ(input[paddingOffset], 114.0F / 255.0F);
    EXPECT_FLOAT_EQ(input[planeSize + paddingOffset], 114.0F / 255.0F);
    EXPECT_FLOAT_EQ(input[2U * planeSize + paddingOffset], 114.0F / 255.0F);
    EXPECT_FLOAT_EQ(input[imageOffset], 30.0F / 255.0F);
    EXPECT_FLOAT_EQ(input[planeSize + imageOffset], 20.0F / 255.0F);
    EXPECT_FLOAT_EQ(input[2U * planeSize + imageOffset], 10.0F / 255.0F);
}

TEST(ModelPreprocessingTest, ResizesLprBgrAndNormalizesEachNchwPlane) {
    const std::array<std::uint8_t, 3> pixels = {0U, 127U, 255U};
    const BgrImageView image(pixels.data(), pixels.size(), 1U, 1U, 3U);

    const auto input = ocrservice::model::detail::makeLprInput(image);

    constexpr std::size_t planeSize =
        ocrservice::model::detail::kLprInputWidth *
        ocrservice::model::detail::kLprInputHeight;
    ASSERT_EQ(input.size(), 3U * planeSize);
    EXPECT_FLOAT_EQ(input[0U], (0.0F - 127.5F) / 128.0F);
    EXPECT_FLOAT_EQ(input[planeSize], (127.0F - 127.5F) / 128.0F);
    EXPECT_FLOAT_EQ(input[2U * planeSize], (255.0F - 127.5F) / 128.0F);
    EXPECT_FLOAT_EQ(input[planeSize - 1U], input[0U]);
}

TEST(YoloPostprocessingTest, UsesRawConfidenceWithoutApplyingSigmoid) {
    std::array<float, 5> output = {50.0F, 50.0F, 20.0F, 10.0F, 0.0F};
    const LetterboxTransform transform{1.0F, 0.0F, 0.0F, 100U, 100U};

    EXPECT_TRUE(ocrservice::model::detail::postprocessYolo(
                    output.data(), 1U, transform, 100U, 100U, 0.25F, 0.45F)
                    .empty());

    output[4U] = 0.25F;
    const auto accepted = ocrservice::model::detail::postprocessYolo(
        output.data(), 1U, transform, 100U, 100U, 0.25F, 0.45F);
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_FLOAT_EQ(accepted.front().score, 0.25F);
}

TEST(YoloPostprocessingTest, RestoresClipsSortsAndSuppressesOverlappingBoxes) {
    constexpr std::size_t anchors = 3U;
    std::array<float, 5U * anchors> output{};
    const auto setAnchor = [&output](
                               const std::size_t anchor,
                               const float centerX,
                               const float centerY,
                               const float width,
                               const float height,
                               const float score) {
        output[anchor] = centerX;
        output[anchors + anchor] = centerY;
        output[2U * anchors + anchor] = width;
        output[3U * anchors + anchor] = height;
        output[4U * anchors + anchor] = score;
    };
    setAnchor(0U, 90.0F, 70.0F, 80.0F, 40.0F, 0.8F);
    setAnchor(1U, 92.0F, 72.0F, 80.0F, 40.0F, 0.7F);
    setAnchor(2U, 210.0F, 120.0F, 80.0F, 80.0F, 0.9F);
    const LetterboxTransform transform{2.0F, 10.0F, 20.0F, 200U, 100U};

    const auto detections = ocrservice::model::detail::postprocessYolo(
        output.data(), anchors, transform, 100U, 50U, 0.25F, 0.45F);

    ASSERT_EQ(detections.size(), 2U);
    EXPECT_FLOAT_EQ(detections[0U].score, 0.9F);
    EXPECT_FLOAT_EQ(detections[0U].x1, 80.0F);
    EXPECT_FLOAT_EQ(detections[0U].y1, 30.0F);
    EXPECT_FLOAT_EQ(detections[0U].x2, 100.0F);
    EXPECT_FLOAT_EQ(detections[0U].y2, 50.0F);
    EXPECT_FLOAT_EQ(detections[1U].score, 0.8F);
    EXPECT_FLOAT_EQ(detections[1U].x1, 20.0F);
    EXPECT_FLOAT_EQ(detections[1U].y1, 15.0F);
    EXPECT_FLOAT_EQ(detections[1U].x2, 60.0F);
    EXPECT_FLOAT_EQ(detections[1U].y2, 35.0F);
}

TEST(YoloPostprocessingTest, KeepsEqualScoresInAnchorOrderAndDropsInvalidBoxes) {
    constexpr std::size_t anchors = 5U;
    std::array<float, 5U * anchors> output{};
    const auto setAnchor = [&output](
                               const std::size_t anchor,
                               const float centerX,
                               const float centerY,
                               const float width,
                               const float height,
                               const float score) {
        output[anchor] = centerX;
        output[anchors + anchor] = centerY;
        output[2U * anchors + anchor] = width;
        output[3U * anchors + anchor] = height;
        output[4U * anchors + anchor] = score;
    };
    setAnchor(0U, 20.0F, 20.0F, 10.0F, 10.0F, 0.8F);
    setAnchor(1U, 80.0F, 80.0F, 10.0F, 10.0F, 0.8F);
    setAnchor(2U, 40.0F, 40.0F, -1.0F, 10.0F, 0.9F);
    setAnchor(
        3U,
        std::numeric_limits<float>::quiet_NaN(),
        40.0F,
        10.0F,
        10.0F,
        0.9F);
    setAnchor(4U, 150.0F, 50.0F, 20.0F, 20.0F, 0.95F);
    const LetterboxTransform transform{1.0F, 0.0F, 0.0F, 100U, 100U};

    const auto detections = ocrservice::model::detail::postprocessYolo(
        output.data(), anchors, transform, 100U, 100U, 0.25F, 0.45F);

    ASSERT_EQ(detections.size(), 2U);
    EXPECT_FLOAT_EQ(detections[0U].x1, 15.0F);
    EXPECT_FLOAT_EQ(detections[1U].x1, 75.0F);
}

TEST(YoloPostprocessingTest, RejectsInvalidArguments) {
    const std::array<float, 5> output{};
    const LetterboxTransform transform{1.0F, 0.0F, 0.0F, 1U, 1U};
    EXPECT_THROW(
        ocrservice::model::detail::postprocessYolo(
            nullptr, 1U, transform, 1U, 1U, 0.25F, 0.45F),
        std::invalid_argument);
    EXPECT_THROW(
        ocrservice::model::detail::postprocessYolo(
            output.data(), 1U, transform, 1U, 1U, 1.01F, 0.45F),
        std::invalid_argument);
}

TEST(LprPostprocessingTest, DecodesKnownPlateIndicesAndAppliesCtcRules) {
    constexpr std::size_t timeSteps = 11U;
    std::vector<float> output(
        ocrservice::model::detail::kLprClassCount * timeSteps, -10.0F);
    const std::array<std::size_t, timeSteps> sequence = {
        26U, 26U, 67U, 59U, 39U, 31U, 37U, 56U, 56U, 67U, 47U};
    for (std::size_t time = 0U; time < timeSteps; ++time) {
        output[sequence[time] * timeSteps + time] = 10.0F;
    }

    const auto decoded = ocrservice::model::detail::decodeLprCtc(
        output.data(), ocrservice::model::detail::kLprClassCount, timeSteps);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, u8"\u9655U806RG");
}

TEST(LprPostprocessingTest, HandlesBlankRepeatsFirstClassAndIo) {
    constexpr std::size_t timeSteps = 9U;
    std::vector<float> output(
        ocrservice::model::detail::kLprClassCount * timeSteps, -10.0F);
    const std::array<std::size_t, timeSteps> sequence = {
        0U, 0U, 67U, 0U, 65U, 65U, 67U, 66U, 66U};
    for (std::size_t time = 0U; time < timeSteps; ++time) {
        output[sequence[time] * timeSteps + time] = 10.0F;
    }
    const auto decoded = ocrservice::model::detail::decodeLprCtc(
        output.data(), ocrservice::model::detail::kLprClassCount, timeSteps);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, u8"\u4eac\u4eacIO");
}

TEST(LprPostprocessingTest, RejectsBlankInvalidShapeAndNonFiniteOutput) {
    constexpr std::size_t timeSteps = 2U;
    std::vector<float> output(
        ocrservice::model::detail::kLprClassCount * timeSteps, -10.0F);
    output[67U * timeSteps] = 10.0F;
    output[67U * timeSteps + 1U] = 10.0F;
    EXPECT_FALSE(ocrservice::model::detail::decodeLprCtc(
                     output.data(), ocrservice::model::detail::kLprClassCount, timeSteps)
                     .has_value());
    EXPECT_FALSE(ocrservice::model::detail::decodeLprCtc(
                     output.data(), ocrservice::model::detail::kLprClassCount - 1U, timeSteps)
                     .has_value());

    output[0U] = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(ocrservice::model::detail::decodeLprCtc(
                     output.data(), ocrservice::model::detail::kLprClassCount, timeSteps)
                     .has_value());
    EXPECT_FALSE(ocrservice::model::detail::allFinite(output.data(), output.size()));
    EXPECT_FALSE(ocrservice::model::detail::allFinite(nullptr, output.size()));
}

TEST(LprPostprocessingTest, UsesTheExactFixedSixtyEightEntryCharacterTable) {
    static constexpr std::array<std::string_view, 68> expected = {
        u8"\u4eac", u8"\u6caa", u8"\u6d25", u8"\u6e1d", u8"\u5180",
        u8"\u664b", u8"\u8499", u8"\u8fbd", u8"\u5409", u8"\u9ed1",
        u8"\u82cf", u8"\u6d59", u8"\u7696", u8"\u95fd", u8"\u8d63",
        u8"\u9c81", u8"\u8c6b", u8"\u9102", u8"\u6e58", u8"\u7ca4",
        u8"\u6842", u8"\u743c", u8"\u5ddd", u8"\u8d35", u8"\u4e91",
        u8"\u85cf", u8"\u9655", u8"\u7518", u8"\u9752", u8"\u5b81",
        u8"\u65b0", "0", "1", "2", "3", "4", "5", "6", "7", "8",
        "9", "A", "B", "C", "D", "E", "F", "G", "H", "J", "K", "L",
        "M", "N", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y",
        "Z", "I", "O", ""};

    EXPECT_EQ(ocrservice::model::detail::lprCharacters(), expected);
}

}  // namespace
