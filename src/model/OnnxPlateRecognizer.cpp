#include "OnnxPlateRecognizer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <onnxruntime_cxx_api.h>
#include <openssl/evp.h>
#include <opencv2/core.hpp>

#include "ModelAlgorithms.h"

namespace ocrservice::model {
namespace {

constexpr std::string_view kYoloFileName = "yolov8_plate.onnx";
constexpr std::string_view kLprFileName = "lprnet.onnx";
constexpr std::string_view kYoloSha256 =
    "bfa426b74d4b619207cca55b296ce3603fb784755a205791af0d0dab4fcf6c04";
constexpr std::string_view kLprSha256 =
    "c78e54070d0e2b8a6f8d548feb52b75321d5f464bcaadb679ec7ee31433cffa4";
constexpr std::array<std::int64_t, 4> kYoloInputShape = {1, 3, 640, 640};
constexpr std::array<std::int64_t, 3> kYoloOutputShape = {1, 5, 8400};
constexpr std::array<std::int64_t, 4> kLprInputShape = {1, 3, 24, 94};
constexpr std::array<std::int64_t, 3> kLprOutputShape = {1, 68, 18};

bool hasExpectedHash(
    const std::filesystem::path& path,
    const std::string_view expectedHash) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return false;
    }
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 &&
            EVP_DigestUpdate(
                context.get(), buffer.data(), static_cast<std::size_t>(count)) != 1) {
            return false;
        }
    }
    if (!input.eof()) {
        return false;
    }
    std::array<unsigned char, 32> digest{};
    unsigned int digestSize = 0U;
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &digestSize) != 1 ||
        digestSize != static_cast<unsigned int>(digest.size())) {
        return false;
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::array<char, 64> actualHash{};
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        actualHash[index * 2U] = kHex[digest[index] >> 4U];
        actualHash[index * 2U + 1U] = kHex[digest[index] & 0x0FU];
    }
    return std::string_view(actualHash.data(), actualHash.size()) == expectedHash;
}

template <std::size_t Rank>
bool hasTensorContract(
    const Ort::Session& session,
    const bool input,
    const std::string_view expectedName,
    const std::array<std::int64_t, Rank>& expectedShape) {
    Ort::AllocatorWithDefaultOptions allocator;
    const auto name = input ? session.GetInputNameAllocated(0U, allocator)
                            : session.GetOutputNameAllocated(0U, allocator);
    if (!name || std::string_view(name.get()) != expectedName) {
        return false;
    }
    const auto typeInfo = input ? session.GetInputTypeInfo(0U)
                                : session.GetOutputTypeInfo(0U);
    const auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
    if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
        return false;
    }
    const auto shape = tensorInfo.GetShape();
    return shape.size() == expectedShape.size() &&
           std::equal(shape.begin(), shape.end(), expectedShape.begin());
}

template <std::size_t InputRank, std::size_t OutputRank>
bool validateSession(
    const Ort::Session& session,
    const std::string_view inputName,
    const std::array<std::int64_t, InputRank>& inputShape,
    const std::string_view outputName,
    const std::array<std::int64_t, OutputRank>& outputShape) {
    return session.GetInputCount() == 1U && session.GetOutputCount() == 1U &&
           hasTensorContract(session, true, inputName, inputShape) &&
           hasTensorContract(session, false, outputName, outputShape);
}

std::vector<float> runSession(
    Ort::Session& session,
    const char* const inputName,
    const char* const outputName,
    std::vector<float>& input,
    const std::int64_t* const inputShape,
    const std::size_t inputRank,
    const std::size_t expectedOutputSize) {
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputTensor = Ort::Value::CreateTensor<float>(
        memory,
        input.data(),
        input.size(),
        inputShape,
        inputRank);
    const std::array<const char*, 1> inputNames = {inputName};
    const std::array<const char*, 1> outputNames = {outputName};
    auto outputs = session.Run(
        Ort::RunOptions{nullptr},
        inputNames.data(),
        &inputTensor,
        1U,
        outputNames.data(),
        1U);
    if (outputs.size() != 1U || !outputs[0].IsTensor()) {
        throw std::runtime_error("model inference output is invalid");
    }
    const auto tensorInfo = outputs[0].GetTensorTypeAndShapeInfo();
    if (tensorInfo.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
        tensorInfo.GetElementCount() != expectedOutputSize) {
        throw std::runtime_error("model inference output contract changed");
    }
    const float* const values = outputs[0].GetTensorData<float>();
    if (!detail::allFinite(values, expectedOutputSize)) {
        throw std::runtime_error("model inference output is non-finite");
    }
    return std::vector<float>(values, values + expectedOutputSize);
}

domain::BgrImageView cropView(
    const domain::BgrImageView source,
    const detail::Detection& detection,
    cv::Mat& crop) {
    const int imageWidth = static_cast<int>(source.width());
    const int imageHeight = static_cast<int>(source.height());
    const int left = std::clamp(static_cast<int>(std::floor(detection.x1)), 0, imageWidth);
    const int top = std::clamp(static_cast<int>(std::floor(detection.y1)), 0, imageHeight);
    const int right = std::clamp(static_cast<int>(std::ceil(detection.x2)), 0, imageWidth);
    const int bottom = std::clamp(static_cast<int>(std::ceil(detection.y2)), 0, imageHeight);
    if (right <= left || bottom <= top) {
        throw std::runtime_error("model detection crop is empty");
    }
    cv::Mat image(
        imageHeight,
        imageWidth,
        CV_8UC3,
        const_cast<std::uint8_t*>(source.data()),
        source.rowStride());
    crop = image(cv::Rect(left, top, right - left, bottom - top));
    const auto byteSize = (static_cast<std::size_t>(crop.rows) - 1U) * crop.step +
                          static_cast<std::size_t>(crop.cols) * 3U;
    return domain::BgrImageView(
        crop.data,
        byteSize,
        static_cast<std::size_t>(crop.cols),
        static_cast<std::size_t>(crop.rows),
        crop.step);
}

}  // namespace

class OnnxPlateRecognizer::Impl final {
public:
    Impl(
        const std::filesystem::path& yoloModel,
        const std::filesystem::path& lprModel,
        const float confidence,
        const float nmsIou)
        : env_(ORT_LOGGING_LEVEL_WARNING, "ocrservice-model"),
          yoloSession_(nullptr),
          lprSession_(nullptr),
          confidence_(confidence),
          nmsIou_(nmsIou) {
        if (yoloModel.filename().string() != kYoloFileName ||
            lprModel.filename().string() != kLprFileName ||
            !hasExpectedHash(yoloModel, kYoloSha256) ||
            !hasExpectedHash(lprModel, kLprSha256) || !std::isfinite(confidence_) ||
            !std::isfinite(nmsIou_) || confidence_ < 0.0F || confidence_ > 1.0F ||
            nmsIou_ < 0.0F || nmsIou_ > 1.0F) {
            throw ModelInitializationError();
        }
        options_.SetIntraOpNumThreads(1);
        options_.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);
        yoloSession_ = Ort::Session(env_, yoloModel.c_str(), options_);
        lprSession_ = Ort::Session(env_, lprModel.c_str(), options_);
        if (!validateSession(
                yoloSession_, "images", kYoloInputShape, "output0", kYoloOutputShape) ||
            !validateSession(
                lprSession_, "input", kLprInputShape, "output", kLprOutputShape)) {
            throw ModelInitializationError();
        }
        Ort::AllocatorWithDefaultOptions allocator;
        const auto metadata = yoloSession_.GetModelMetadata();
        const auto names = metadata.LookupCustomMetadataMapAllocated("names", allocator);
        if (!names || std::string_view(names.get()) != "{0: '0'}") {
            throw ModelInitializationError();
        }
        std::vector<float> yoloZero(3U * detail::kYoloInputSize * detail::kYoloInputSize);
        std::vector<float> lprZero(
            3U * detail::kLprInputHeight * detail::kLprInputWidth);
        (void)runSession(
            yoloSession_,
            "images",
            "output0",
            yoloZero,
            kYoloInputShape.data(),
            kYoloInputShape.size(),
            5U * detail::kYoloAnchorCount);
        (void)runSession(
            lprSession_,
            "input",
            "output",
            lprZero,
            kLprInputShape.data(),
            kLprInputShape.size(),
            detail::kLprClassCount * detail::kLprTimeSteps);
    }

    domain::RecognitionOutcome recognize(const domain::BgrImageView image) {
        detail::LetterboxTransform transform{};
        auto yoloInput = detail::makeYoloInput(image, transform);
        const auto yoloOutput = runSession(
            yoloSession_,
            "images",
            "output0",
            yoloInput,
            kYoloInputShape.data(),
            kYoloInputShape.size(),
            5U * detail::kYoloAnchorCount);
        const auto detections = detail::postprocessYolo(
            yoloOutput.data(),
            detail::kYoloAnchorCount,
            transform,
            image.width(),
            image.height(),
            confidence_,
            nmsIou_);
        if (detections.empty()) {
            return domain::RecognitionOutcome::failed(domain::ModelFailureCode::plateNotFound);
        }

        cv::Mat crop;
        const auto lprImage = cropView(image, detections.front(), crop);
        auto lprInput = detail::makeLprInput(lprImage);
        const auto lprOutput = runSession(
            lprSession_,
            "input",
            "output",
            lprInput,
            kLprInputShape.data(),
            kLprInputShape.size(),
            detail::kLprClassCount * detail::kLprTimeSteps);
        const auto decoded = detail::decodeLprCtc(
            lprOutput.data(), detail::kLprClassCount, detail::kLprTimeSteps);
        if (!decoded) {
            return domain::RecognitionOutcome::failed(
                domain::ModelFailureCode::plateRecognitionFailed);
        }
        try {
            return domain::RecognitionOutcome::succeeded(domain::PlateNumber::parse(*decoded));
        } catch (const domain::DomainError&) {
            return domain::RecognitionOutcome::failed(
                domain::ModelFailureCode::plateRecognitionFailed);
        }
    }

private:
    Ort::Env env_;
    Ort::SessionOptions options_;
    Ort::Session yoloSession_;
    Ort::Session lprSession_;
    float confidence_;
    float nmsIou_;
};

ModelInitializationError::ModelInitializationError()
    : std::runtime_error("model initialization failed") {}

OnnxPlateRecognizer::OnnxPlateRecognizer(
    std::filesystem::path yoloModel,
    std::filesystem::path lprModel,
    const float yoloConfidence,
    const float yoloNmsIou) try
    : impl_(std::make_unique<Impl>(
          yoloModel, lprModel, yoloConfidence, yoloNmsIou)) {
} catch (...) {
    throw ModelInitializationError();
}

OnnxPlateRecognizer::~OnnxPlateRecognizer() = default;

domain::RecognitionOutcome OnnxPlateRecognizer::recognize(
    const domain::BgrImageView bgrImage) {
    try {
        return impl_->recognize(bgrImage);
    } catch (...) {
        return domain::RecognitionOutcome::failed(
            domain::ModelFailureCode::modelInferenceError);
    }
}

}  // namespace ocrservice::model
