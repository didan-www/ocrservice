#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>

#include "Ports.h"

namespace ocrservice::model {

class ModelInitializationError final : public std::runtime_error {
public:
    ModelInitializationError();
};

class OnnxPlateRecognizer final : public domain::IPlateRecognizer {
public:
    OnnxPlateRecognizer(
        std::filesystem::path yoloModel,
        std::filesystem::path lprModel,
        float yoloConfidence = 0.25F,
        float yoloNmsIou = 0.45F);
    ~OnnxPlateRecognizer() override;

    OnnxPlateRecognizer(const OnnxPlateRecognizer&) = delete;
    OnnxPlateRecognizer& operator=(const OnnxPlateRecognizer&) = delete;
    OnnxPlateRecognizer(OnnxPlateRecognizer&&) = delete;
    OnnxPlateRecognizer& operator=(OnnxPlateRecognizer&&) = delete;

    domain::RecognitionOutcome recognize(domain::BgrImageView bgrImage) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ocrservice::model
