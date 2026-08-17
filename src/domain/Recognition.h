#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "Identifiers.h"
#include "ProtocolTime.h"

namespace ocrservice::domain {

using UtcTimePoint = serialization::time::UtcTimePoint;

enum class RecognitionStatus { processing, succeeded, failed };
enum class GateAction { open, keepClosed };
enum class AccessListType { white, black };
enum class ModelFailureCode { plateNotFound, plateRecognitionFailed, modelInferenceError };
enum class RecognitionFailureCode {
    plateNotFound,
    plateRecognitionFailed,
    modelInferenceError,
    serverRestarted
};

std::string_view toString(RecognitionStatus value);
std::string_view toString(GateAction value);
std::string_view toString(AccessListType value);
std::string_view toString(ModelFailureCode value);
std::string_view toString(RecognitionFailureCode value);
RecognitionFailureCode toRecognitionFailureCode(ModelFailureCode value);
std::optional<GateAction> gateActionFor(RecognitionStatus status);

class RecognitionOutcome final {
public:
    static RecognitionOutcome succeeded(PlateNumber plateNumber);
    static RecognitionOutcome failed(ModelFailureCode failureCode);

    bool isSuccess() const noexcept;
    const PlateNumber& plateNumber() const;
    ModelFailureCode failureCode() const;

private:
    explicit RecognitionOutcome(std::variant<PlateNumber, ModelFailureCode> value);
    std::variant<PlateNumber, ModelFailureCode> value_;
};

class RecognitionSnapshot final {
public:
    RecognitionSnapshot(
        RecognitionId recognitionId,
        std::uint64_t revision,
        DeviceId deviceId,
        RecognitionStatus status,
        std::optional<PlateNumber> plateNumber,
        std::optional<RecognitionFailureCode> errorCode,
        std::optional<std::string> errorMessage,
        UtcTimePoint capturedAt,
        UtcTimePoint startedAt,
        std::optional<UtcTimePoint> completedAt,
        std::optional<std::uint64_t> durationMs);

    static constexpr std::uint64_t schemaVersion() noexcept {
        return 1U;
    }

    const RecognitionId& recognitionId() const noexcept;
    std::uint64_t revision() const noexcept;
    const DeviceId& deviceId() const noexcept;
    RecognitionStatus status() const noexcept;
    const std::optional<PlateNumber>& plateNumber() const noexcept;
    const std::optional<RecognitionFailureCode>& errorCode() const noexcept;
    const std::optional<std::string>& errorMessage() const noexcept;
    UtcTimePoint capturedAt() const noexcept;
    UtcTimePoint startedAt() const noexcept;
    const std::optional<UtcTimePoint>& completedAt() const noexcept;
    const std::optional<std::uint64_t>& durationMs() const noexcept;

private:
    RecognitionId recognitionId_;
    std::uint64_t revision_;
    DeviceId deviceId_;
    RecognitionStatus status_;
    std::optional<PlateNumber> plateNumber_;
    std::optional<RecognitionFailureCode> errorCode_;
    std::optional<std::string> errorMessage_;
    UtcTimePoint capturedAt_;
    UtcTimePoint startedAt_;
    std::optional<UtcTimePoint> completedAt_;
    std::optional<std::uint64_t> durationMs_;
};

class HistoryFilter final {
public:
    HistoryFilter(
        UtcTimePoint startInclusiveUtc,
        UtcTimePoint endExclusiveUtc,
        std::optional<DeviceId> deviceId = std::nullopt);

    UtcTimePoint startInclusiveUtc() const noexcept;
    UtcTimePoint endExclusiveUtc() const noexcept;
    const std::optional<DeviceId>& deviceId() const noexcept;

private:
    UtcTimePoint startInclusiveUtc_;
    UtcTimePoint endExclusiveUtc_;
    std::optional<DeviceId> deviceId_;
};

class AccessListFilter final {
public:
    AccessListFilter(AccessListType listType, PlateKeyword keyword);
    AccessListType listType() const noexcept;
    const PlateKeyword& keyword() const noexcept;

private:
    AccessListType listType_;
    PlateKeyword keyword_;
};

class PageRequest final {
public:
    static constexpr std::uint64_t pageSize() noexcept {
        return 100U;
    }

    explicit PageRequest(std::uint64_t page);
    std::uint64_t page() const noexcept;

private:
    std::uint64_t page_;
};

class RecognitionTask final {
public:
    explicit RecognitionTask(RecognitionId recognitionId);
    const RecognitionId& recognitionId() const noexcept;

private:
    RecognitionId recognitionId_;
};

}  // namespace ocrservice::domain
