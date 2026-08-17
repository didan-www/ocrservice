#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Ports.h"

namespace ocrservice::model::detail {

constexpr std::size_t kYoloInputSize = 640U;
constexpr std::size_t kYoloAnchorCount = 8400U;
constexpr std::size_t kLprInputWidth = 94U;
constexpr std::size_t kLprInputHeight = 24U;
constexpr std::size_t kLprClassCount = 68U;
constexpr std::size_t kLprTimeSteps = 18U;

struct LetterboxTransform final {
    float scale;
    float padX;
    float padY;
    std::size_t resizedWidth;
    std::size_t resizedHeight;
};

struct Detection final {
    float x1;
    float y1;
    float x2;
    float y2;
    float score;
};

std::vector<float> makeYoloInput(
    domain::BgrImageView image,
    LetterboxTransform& transform);

std::vector<Detection> postprocessYolo(
    const float* channelMajorOutput,
    std::size_t anchorCount,
    const LetterboxTransform& transform,
    std::size_t originalWidth,
    std::size_t originalHeight,
    float confidenceThreshold,
    float nmsIouThreshold);

std::vector<float> makeLprInput(domain::BgrImageView image);

std::optional<std::string> decodeLprCtc(
    const float* classMajorOutput,
    std::size_t classCount,
    std::size_t timeSteps);

const std::array<std::string_view, kLprClassCount>& lprCharacters() noexcept;
bool allFinite(const float* values, std::size_t count) noexcept;

}  // namespace ocrservice::model::detail
