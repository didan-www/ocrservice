#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <onnxruntime_cxx_api.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "ModelAlgorithms.h"
#include "OnnxPlateRecognizer.h"

namespace {

using ocrservice::domain::BgrImageView;
using ocrservice::domain::ModelFailureCode;
using ocrservice::domain::RecognitionOutcome;
using ocrservice::model::ModelInitializationError;
using ocrservice::model::OnnxPlateRecognizer;

const std::filesystem::path& modelDirectory() {
    static const std::filesystem::path directory(OCRSERVICE_TEST_MODEL_DIR);
    return directory;
}

const std::filesystem::path& imageDirectory() {
    static const std::filesystem::path directory(OCRSERVICE_TEST_IMAGE_DIR);
    return directory;
}

OnnxPlateRecognizer& sharedRecognizer() {
    static OnnxPlateRecognizer recognizer(
        modelDirectory() / "yolov8_plate.onnx",
        modelDirectory() / "lprnet.onnx");
    return recognizer;
}

BgrImageView viewOf(const cv::Mat& image) {
    const auto byteSize = (static_cast<std::size_t>(image.rows) - 1U) * image.step +
                          static_cast<std::size_t>(image.cols) * 3U;
    return BgrImageView(
        image.data,
        byteSize,
        static_cast<std::size_t>(image.cols),
        static_cast<std::size_t>(image.rows),
        image.step);
}

void expectSameOutcome(const RecognitionOutcome& left, const RecognitionOutcome& right) {
    ASSERT_EQ(left.isSuccess(), right.isSuccess());
    if (left.isSuccess()) {
        EXPECT_EQ(left.plateNumber(), right.plateNumber());
    } else {
        EXPECT_EQ(left.failureCode(), right.failureCode());
    }
}

void expectStableInitializationFailure(
    const std::filesystem::path& yolo,
    const std::filesystem::path& lpr,
    const float confidence = 0.25F,
    const float nmsIou = 0.45F) {
    try {
        const OnnxPlateRecognizer recognizer(yolo, lpr, confidence, nmsIou);
        (void)recognizer;
        FAIL() << "expected model initialization to fail";
    } catch (const ModelInitializationError& error) {
        EXPECT_EQ(std::string(error.what()), "model initialization failed");
    } catch (...) {
        FAIL() << "initialization leaked a non-domain exception";
    }
}

std::string runFixedLprCrop(const BgrImageView image) {
    Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "ocrservice-model-test");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads(1);
    Ort::Session session(
        environment,
        (modelDirectory() / "lprnet.onnx").c_str(),
        options);
    auto input = ocrservice::model::detail::makeLprInput(image);
    constexpr std::array<std::int64_t, 4> inputShape = {1, 3, 24, 94};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    auto inputTensor = Ort::Value::CreateTensor<float>(
        memory,
        input.data(),
        input.size(),
        inputShape.data(),
        inputShape.size());
    constexpr std::array<const char*, 1> inputNames = {"input"};
    constexpr std::array<const char*, 1> outputNames = {"output"};
    auto outputs = session.Run(
        Ort::RunOptions{nullptr},
        inputNames.data(),
        &inputTensor,
        1U,
        outputNames.data(),
        1U);
    constexpr std::size_t outputSize =
        ocrservice::model::detail::kLprClassCount *
        ocrservice::model::detail::kLprTimeSteps;
    if (outputs.size() != 1U || !outputs.front().IsTensor() ||
        outputs.front().GetTensorTypeAndShapeInfo().GetElementCount() != outputSize) {
        throw std::runtime_error("fixed LPR crop output contract changed");
    }
    const auto decoded = ocrservice::model::detail::decodeLprCtc(
        outputs.front().GetTensorData<float>(),
        ocrservice::model::detail::kLprClassCount,
        ocrservice::model::detail::kLprTimeSteps);
    if (!decoded) {
        throw std::runtime_error("fixed LPR crop did not decode");
    }
    return *decoded;
}

TEST(OnnxPlateRecognizerTest, LoadsExactPackagedModelsAndRunsStartupSmoke) {
    EXPECT_NO_THROW((void)sharedRecognizer());
}

TEST(OnnxPlateRecognizerTest, RejectsWrongFilenameWithOnlyStableErrorText) {
    expectStableInitializationFailure(
        modelDirectory() / "not-the-yolo-model.onnx",
        modelDirectory() / "lprnet.onnx");
}

TEST(OnnxPlateRecognizerTest, RejectsWrongLprFilenameWithOnlyStableErrorText) {
    expectStableInitializationFailure(
        modelDirectory() / "yolov8_plate.onnx",
        modelDirectory() / "not-the-lpr-model.onnx");
}

TEST(OnnxPlateRecognizerTest, RejectsWrongHashWithOnlyStableErrorText) {
    const auto temporaryDirectory =
        std::filesystem::temp_directory_path() / "ocrservice-task007-wrong-model";
    std::error_code ignored;
    std::filesystem::remove_all(temporaryDirectory, ignored);
    ASSERT_TRUE(std::filesystem::create_directories(temporaryDirectory));
    const auto wrongModel = temporaryDirectory / "yolov8_plate.onnx";
    {
        std::ofstream output(wrongModel, std::ios::binary);
        ASSERT_TRUE(output.good());
        output << "not an ONNX model";
        ASSERT_TRUE(output.good());
    }

    expectStableInitializationFailure(wrongModel, modelDirectory() / "lprnet.onnx");

    std::filesystem::remove_all(temporaryDirectory, ignored);
}

TEST(OnnxPlateRecognizerTest, RejectsWrongLprHashWithOnlyStableErrorText) {
    const auto temporaryDirectory =
        std::filesystem::temp_directory_path() / "ocrservice-task007-wrong-lpr-model";
    std::error_code ignored;
    std::filesystem::remove_all(temporaryDirectory, ignored);
    ASSERT_TRUE(std::filesystem::create_directories(temporaryDirectory));
    const auto wrongModel = temporaryDirectory / "lprnet.onnx";
    {
        std::ofstream output(wrongModel, std::ios::binary);
        ASSERT_TRUE(output.good());
        output << "not an ONNX model";
        ASSERT_TRUE(output.good());
    }

    expectStableInitializationFailure(
        modelDirectory() / "yolov8_plate.onnx", wrongModel);

    std::filesystem::remove_all(temporaryDirectory, ignored);
}

TEST(OnnxPlateRecognizerTest, RejectsInvalidThresholdWithOnlyStableErrorText) {
    expectStableInitializationFailure(
        modelDirectory() / "yolov8_plate.onnx",
        modelDirectory() / "lprnet.onnx",
        1.1F,
        0.45F);
}

TEST(OnnxPlateRecognizerTest, MapsFixedBlankImageToPlateNotFound) {
    const cv::Mat image = cv::imread(
        (imageDirectory() / "blank.ppm").string(), cv::IMREAD_COLOR);
    ASSERT_FALSE(image.empty());

    const auto outcome = sharedRecognizer().recognize(viewOf(image));

    ASSERT_FALSE(outcome.isSuccess());
    EXPECT_EQ(outcome.failureCode(), ModelFailureCode::plateNotFound);
}

TEST(OnnxPlateRecognizerTest, MapsPreprocessingExceptionToStableInferenceFailure) {
    std::uint8_t dummy = 0U;
    const std::size_t width =
        static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U;
    const std::size_t rowStride = width * 3U;
    const BgrImageView oversizedImage(
        &dummy, rowStride, width, 1U, rowStride);

    const auto outcome = sharedRecognizer().recognize(oversizedImage);

    ASSERT_FALSE(outcome.isSuccess());
    EXPECT_EQ(outcome.failureCode(), ModelFailureCode::modelInferenceError);
}

TEST(OnnxPlateRecognizerTest, ProducesDeterministicDomainResultForTeachingPlateFixture) {
    const cv::Mat image = cv::imread(
        (imageDirectory() / "teaching_plate.ppm").string(), cv::IMREAD_COLOR);
    ASSERT_FALSE(image.empty());

    const auto first = sharedRecognizer().recognize(viewOf(image));
    const auto second = sharedRecognizer().recognize(viewOf(image));

    expectSameOutcome(first, second);
    if (!first.isSuccess()) {
        EXPECT_NE(first.failureCode(), ModelFailureCode::modelInferenceError);
    }
}

TEST(OnnxPlateRecognizerTest, FixedLprCropProducesKnownCharacterSequence) {
    const cv::Mat image = cv::imread(
        (imageDirectory() / "teaching_plate.ppm").string(), cv::IMREAD_COLOR);
    ASSERT_FALSE(image.empty());

    EXPECT_EQ(runFixedLprCrop(viewOf(image)), u8"\u7696A1A");
}

}  // namespace
