#include "ModelAlgorithms.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace ocrservice::model::detail {
namespace {

cv::Mat bgrMat(const domain::BgrImageView image) {
    if (image.width() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        image.height() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("model image dimensions are unsupported");
    }
    return cv::Mat(
        static_cast<int>(image.height()),
        static_cast<int>(image.width()),
        CV_8UC3,
        const_cast<std::uint8_t*>(image.data()),
        image.rowStride());
}

std::vector<float> toNchwUnitRange(const cv::Mat& image) {
    const auto planeSize = static_cast<std::size_t>(image.rows) *
                           static_cast<std::size_t>(image.cols);
    std::vector<float> result(planeSize * 3U);
    for (int row = 0; row < image.rows; ++row) {
        for (int column = 0; column < image.cols; ++column) {
            const auto pixel = image.at<cv::Vec3b>(row, column);
            const auto offset = static_cast<std::size_t>(row) *
                                    static_cast<std::size_t>(image.cols) +
                                static_cast<std::size_t>(column);
            for (int channel = 0; channel < 3; ++channel) {
                result[static_cast<std::size_t>(channel) * planeSize + offset] =
                    static_cast<float>(pixel[channel]) / 255.0F;
            }
        }
    }
    return result;
}

float intersectionOverUnion(const Detection& left, const Detection& right) noexcept {
    const float intersectionWidth =
        std::max(0.0F, std::min(left.x2, right.x2) - std::max(left.x1, right.x1));
    const float intersectionHeight =
        std::max(0.0F, std::min(left.y2, right.y2) - std::max(left.y1, right.y1));
    const float intersection = intersectionWidth * intersectionHeight;
    const float leftArea = (left.x2 - left.x1) * (left.y2 - left.y1);
    const float rightArea = (right.x2 - right.x1) * (right.y2 - right.y1);
    const float unionArea = leftArea + rightArea - intersection;
    return unionArea > 0.0F ? intersection / unionArea : 0.0F;
}

}  // namespace

std::vector<float> makeYoloInput(
    const domain::BgrImageView image,
    LetterboxTransform& transform) {
    const double horizontalScale =
        static_cast<double>(kYoloInputSize) / static_cast<double>(image.width());
    const double verticalScale =
        static_cast<double>(kYoloInputSize) / static_cast<double>(image.height());
    const double scale = std::min(horizontalScale, verticalScale);
    const auto resizedWidth = static_cast<std::size_t>(std::llround(
        static_cast<double>(image.width()) * scale));
    const auto resizedHeight = static_cast<std::size_t>(std::llround(
        static_cast<double>(image.height()) * scale));
    if (resizedWidth == 0U || resizedHeight == 0U || resizedWidth > kYoloInputSize ||
        resizedHeight > kYoloInputSize) {
        throw std::invalid_argument("model letterbox dimensions are invalid");
    }
    const auto left = (kYoloInputSize - resizedWidth) / 2U;
    const auto top = (kYoloInputSize - resizedHeight) / 2U;
    transform = LetterboxTransform{
        static_cast<float>(scale),
        static_cast<float>(left),
        static_cast<float>(top),
        resizedWidth,
        resizedHeight};

    cv::Mat rgb;
    cv::cvtColor(bgrMat(image), rgb, cv::COLOR_BGR2RGB);
    cv::Mat resized;
    cv::resize(
        rgb,
        resized,
        cv::Size(static_cast<int>(resizedWidth), static_cast<int>(resizedHeight)),
        0.0,
        0.0,
        cv::INTER_LINEAR);
    cv::Mat canvas(
        static_cast<int>(kYoloInputSize),
        static_cast<int>(kYoloInputSize),
        CV_8UC3,
        cv::Scalar(114, 114, 114));
    resized.copyTo(canvas(cv::Rect(
        static_cast<int>(left),
        static_cast<int>(top),
        static_cast<int>(resizedWidth),
        static_cast<int>(resizedHeight))));
    return toNchwUnitRange(canvas);
}

std::vector<Detection> postprocessYolo(
    const float* const channelMajorOutput,
    const std::size_t anchorCount,
    const LetterboxTransform& transform,
    const std::size_t originalWidth,
    const std::size_t originalHeight,
    const float confidenceThreshold,
    const float nmsIouThreshold) {
    if (channelMajorOutput == nullptr || anchorCount == 0U || transform.scale <= 0.0F ||
        originalWidth == 0U || originalHeight == 0U ||
        !std::isfinite(confidenceThreshold) || !std::isfinite(nmsIouThreshold) ||
        confidenceThreshold < 0.0F || confidenceThreshold > 1.0F ||
        nmsIouThreshold < 0.0F || nmsIouThreshold > 1.0F) {
        throw std::invalid_argument("YOLO postprocessing arguments are invalid");
    }

    struct Candidate final {
        Detection detection;
        std::size_t anchor;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(anchorCount);
    const float maximumX = static_cast<float>(originalWidth);
    const float maximumY = static_cast<float>(originalHeight);
    for (std::size_t anchor = 0U; anchor < anchorCount; ++anchor) {
        const float centerX = channelMajorOutput[anchor];
        const float centerY = channelMajorOutput[anchorCount + anchor];
        const float width = channelMajorOutput[2U * anchorCount + anchor];
        const float height = channelMajorOutput[3U * anchorCount + anchor];
        const float score = channelMajorOutput[4U * anchorCount + anchor];
        if (!std::isfinite(centerX) || !std::isfinite(centerY) || !std::isfinite(width) ||
            !std::isfinite(height) || !std::isfinite(score) || score < confidenceThreshold ||
            width <= 0.0F || height <= 0.0F) {
            continue;
        }
        const float x1 = std::clamp(
            (centerX - width / 2.0F - transform.padX) / transform.scale,
            0.0F,
            maximumX);
        const float y1 = std::clamp(
            (centerY - height / 2.0F - transform.padY) / transform.scale,
            0.0F,
            maximumY);
        const float x2 = std::clamp(
            (centerX + width / 2.0F - transform.padX) / transform.scale,
            0.0F,
            maximumX);
        const float y2 = std::clamp(
            (centerY + height / 2.0F - transform.padY) / transform.scale,
            0.0F,
            maximumY);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }
        candidates.push_back(Candidate{Detection{x1, y1, x2, y2, score}, anchor});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        return left.detection.score == right.detection.score
                   ? left.anchor < right.anchor
                   : left.detection.score > right.detection.score;
    });

    std::vector<Detection> kept;
    kept.reserve(candidates.size());
    for (const auto& candidate : candidates) {
        bool suppressed = false;
        for (const auto& selected : kept) {
            if (intersectionOverUnion(candidate.detection, selected) > nmsIouThreshold) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) {
            kept.push_back(candidate.detection);
        }
    }
    return kept;
}

std::vector<float> makeLprInput(const domain::BgrImageView image) {
    cv::Mat resized;
    cv::resize(
        bgrMat(image),
        resized,
        cv::Size(static_cast<int>(kLprInputWidth), static_cast<int>(kLprInputHeight)),
        0.0,
        0.0,
        cv::INTER_LINEAR);
    const auto planeSize = kLprInputWidth * kLprInputHeight;
    std::vector<float> result(planeSize * 3U);
    for (int row = 0; row < resized.rows; ++row) {
        for (int column = 0; column < resized.cols; ++column) {
            const auto pixel = resized.at<cv::Vec3b>(row, column);
            const auto offset = static_cast<std::size_t>(row) * kLprInputWidth +
                                static_cast<std::size_t>(column);
            for (int channel = 0; channel < 3; ++channel) {
                result[static_cast<std::size_t>(channel) * planeSize + offset] =
                    (static_cast<float>(pixel[channel]) - 127.5F) / 128.0F;
            }
        }
    }
    return result;
}

std::optional<std::string> decodeLprCtc(
    const float* const classMajorOutput,
    const std::size_t classCount,
    const std::size_t timeSteps) {
    if (classMajorOutput == nullptr || classCount != kLprClassCount || timeSteps == 0U) {
        return std::nullopt;
    }
    std::string decoded;
    constexpr std::size_t blankClass = kLprClassCount - 1U;
    std::size_t previous = blankClass;
    for (std::size_t time = 0U; time < timeSteps; ++time) {
        std::size_t bestClass = 0U;
        float bestValue = classMajorOutput[time];
        if (!std::isfinite(bestValue)) {
            return std::nullopt;
        }
        for (std::size_t character = 1U; character < classCount; ++character) {
            const float value = classMajorOutput[character * timeSteps + time];
            if (!std::isfinite(value)) {
                return std::nullopt;
            }
            if (value > bestValue) {
                bestValue = value;
                bestClass = character;
            }
        }
        if (bestClass != previous && bestClass != blankClass) {
            decoded.append(lprCharacters()[bestClass]);
        }
        previous = bestClass;
    }
    if (decoded.empty()) {
        return std::nullopt;
    }
    return decoded;
}

const std::array<std::string_view, kLprClassCount>& lprCharacters() noexcept {
    static constexpr std::array<std::string_view, kLprClassCount> kCharacters = {
        u8"京", u8"沪", u8"津", u8"渝", u8"冀", u8"晋", u8"蒙", u8"辽",
        u8"吉", u8"黑", u8"苏", u8"浙", u8"皖", u8"闽", u8"赣", u8"鲁",
        u8"豫", u8"鄂", u8"湘", u8"粤", u8"桂", u8"琼", u8"川", u8"贵",
        u8"云", u8"藏", u8"陕", u8"甘", u8"青", u8"宁", u8"新", "0",
        "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B", "C",
        "D", "E", "F", "G", "H", "J", "K", "L", "M", "N", "P", "Q",
        "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "I", "O", ""};
    return kCharacters;
}

bool allFinite(const float* const values, const std::size_t count) noexcept {
    if (values == nullptr) {
        return false;
    }
    for (std::size_t index = 0U; index < count; ++index) {
        if (!std::isfinite(values[index])) {
            return false;
        }
    }
    return true;
}

}  // namespace ocrservice::model::detail
