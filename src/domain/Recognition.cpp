#include "Recognition.h"

#include <utility>

#include "Utf8.h"

namespace ocrservice::domain {

std::string_view toString(const RecognitionStatus value) {
    switch (value) {
        case RecognitionStatus::processing:
            return "PROCESSING";
        case RecognitionStatus::succeeded:
            return "SUCCEEDED";
        case RecognitionStatus::failed:
            return "FAILED";
    }
    throw DomainError("invalid recognition status");
}

std::string_view toString(const GateAction value) {
    switch (value) {
        case GateAction::open:
            return "OPEN";
        case GateAction::keepClosed:
            return "KEEP_CLOSED";
    }
    throw DomainError("invalid gate action");
}

std::string_view toString(const AccessListType value) {
    switch (value) {
        case AccessListType::white:
            return "WHITE";
        case AccessListType::black:
            return "BLACK";
    }
    throw DomainError("invalid access-list type");
}

std::string_view toString(const ModelFailureCode value) {
    switch (value) {
        case ModelFailureCode::plateNotFound:
            return "PLATE_NOT_FOUND";
        case ModelFailureCode::plateRecognitionFailed:
            return "PLATE_RECOGNITION_FAILED";
        case ModelFailureCode::modelInferenceError:
            return "MODEL_INFERENCE_ERROR";
    }
    throw DomainError("invalid model failure code");
}

std::string_view toString(const RecognitionFailureCode value) {
    switch (value) {
        case RecognitionFailureCode::plateNotFound:
            return "PLATE_NOT_FOUND";
        case RecognitionFailureCode::plateRecognitionFailed:
            return "PLATE_RECOGNITION_FAILED";
        case RecognitionFailureCode::modelInferenceError:
            return "MODEL_INFERENCE_ERROR";
        case RecognitionFailureCode::serverRestarted:
            return "SERVER_RESTARTED";
    }
    throw DomainError("invalid recognition failure code");
}

RecognitionFailureCode toRecognitionFailureCode(const ModelFailureCode value) {
    switch (value) {
        case ModelFailureCode::plateNotFound:
            return RecognitionFailureCode::plateNotFound;
        case ModelFailureCode::plateRecognitionFailed:
            return RecognitionFailureCode::plateRecognitionFailed;
        case ModelFailureCode::modelInferenceError:
            return RecognitionFailureCode::modelInferenceError;
    }
    throw DomainError("invalid model failure code");
}

std::optional<GateAction> gateActionFor(const RecognitionStatus status) {
    switch (status) {
        case RecognitionStatus::processing:
            return std::nullopt;
        case RecognitionStatus::succeeded:
            return GateAction::open;
        case RecognitionStatus::failed:
            return GateAction::keepClosed;
    }
    throw DomainError("invalid recognition status");
}

RecognitionOutcome::RecognitionOutcome(std::variant<PlateNumber, ModelFailureCode> value)
    : value_(std::move(value)) {}
RecognitionOutcome RecognitionOutcome::succeeded(PlateNumber plateNumber) {
    return RecognitionOutcome(std::move(plateNumber));
}
RecognitionOutcome RecognitionOutcome::failed(const ModelFailureCode failureCode) {
    (void)toString(failureCode);
    return RecognitionOutcome(failureCode);
}
bool RecognitionOutcome::isSuccess() const noexcept {
    return std::holds_alternative<PlateNumber>(value_);
}
const PlateNumber& RecognitionOutcome::plateNumber() const {
    if (!isSuccess()) {
        throw DomainError("failed recognition outcome has no plate number");
    }
    return std::get<PlateNumber>(value_);
}
ModelFailureCode RecognitionOutcome::failureCode() const {
    if (isSuccess()) {
        throw DomainError("successful recognition outcome has no failure code");
    }
    return std::get<ModelFailureCode>(value_);
}

RecognitionSnapshot::RecognitionSnapshot(
    RecognitionId recognitionId,
    const std::uint64_t revision,
    DeviceId deviceId,
    const RecognitionStatus status,
    std::optional<PlateNumber> plateNumber,
    std::optional<RecognitionFailureCode> errorCode,
    std::optional<std::string> errorMessage,
    const UtcTimePoint capturedAt,
    const UtcTimePoint startedAt,
    std::optional<UtcTimePoint> completedAt,
    std::optional<std::uint64_t> durationMs)
    : recognitionId_(std::move(recognitionId)),
      revision_(revision),
      deviceId_(std::move(deviceId)),
      status_(status),
      plateNumber_(std::move(plateNumber)),
      errorCode_(errorCode),
      errorMessage_(std::move(errorMessage)),
      capturedAt_(capturedAt),
      startedAt_(startedAt),
      completedAt_(completedAt),
      durationMs_(durationMs) {
    (void)toString(status_);
    if (errorCode_) {
        (void)toString(*errorCode_);
    }
    requirePositiveJsonSafe(revision_, "revision");
    if (status_ == RecognitionStatus::processing) {
        if (revision_ != 1U || plateNumber_ || errorCode_ || errorMessage_ || completedAt_ ||
            durationMs_) {
            throw DomainError("PROCESSING snapshot has an invalid field combination");
        }
        return;
    }
    if (revision_ < 2U || !completedAt_ || !durationMs_ || *completedAt_ < startedAt_) {
        throw DomainError("final snapshot has invalid revision or completion fields");
    }
    requireNonNegativeJsonSafe(*durationMs_, "durationMs");
    if (status_ == RecognitionStatus::succeeded) {
        if (!plateNumber_ || errorCode_ || errorMessage_) {
            throw DomainError("SUCCEEDED snapshot has an invalid field combination");
        }
        return;
    }
    if (plateNumber_ || !errorCode_ || !errorMessage_ || errorMessage_->empty()) {
        throw DomainError("FAILED snapshot has an invalid field combination");
    }
    try {
        if (text::countCodePoints(*errorMessage_) > 512U) {
            throw DomainError("errorMessage exceeds 512 Unicode code points");
        }
    } catch (const text::Utf8Error& error) {
        throw DomainError(error.what());
    }
}

const RecognitionId& RecognitionSnapshot::recognitionId() const noexcept { return recognitionId_; }
std::uint64_t RecognitionSnapshot::revision() const noexcept { return revision_; }
const DeviceId& RecognitionSnapshot::deviceId() const noexcept { return deviceId_; }
RecognitionStatus RecognitionSnapshot::status() const noexcept { return status_; }
const std::optional<PlateNumber>& RecognitionSnapshot::plateNumber() const noexcept {
    return plateNumber_;
}
const std::optional<RecognitionFailureCode>& RecognitionSnapshot::errorCode() const noexcept {
    return errorCode_;
}
const std::optional<std::string>& RecognitionSnapshot::errorMessage() const noexcept {
    return errorMessage_;
}
UtcTimePoint RecognitionSnapshot::capturedAt() const noexcept { return capturedAt_; }
UtcTimePoint RecognitionSnapshot::startedAt() const noexcept { return startedAt_; }
const std::optional<UtcTimePoint>& RecognitionSnapshot::completedAt() const noexcept {
    return completedAt_;
}
const std::optional<std::uint64_t>& RecognitionSnapshot::durationMs() const noexcept {
    return durationMs_;
}

HistoryFilter::HistoryFilter(
    const UtcTimePoint startInclusiveUtc,
    const UtcTimePoint endExclusiveUtc,
    std::optional<DeviceId> deviceId)
    : startInclusiveUtc_(startInclusiveUtc),
      endExclusiveUtc_(endExclusiveUtc),
      deviceId_(std::move(deviceId)) {
    if (endExclusiveUtc_ <= startInclusiveUtc_) {
        throw DomainError("history end time must be after start time");
    }
}
UtcTimePoint HistoryFilter::startInclusiveUtc() const noexcept { return startInclusiveUtc_; }
UtcTimePoint HistoryFilter::endExclusiveUtc() const noexcept { return endExclusiveUtc_; }
const std::optional<DeviceId>& HistoryFilter::deviceId() const noexcept { return deviceId_; }

AccessListFilter::AccessListFilter(const AccessListType listType, PlateKeyword keyword)
    : listType_(listType), keyword_(std::move(keyword)) {
    (void)toString(listType_);
}
AccessListType AccessListFilter::listType() const noexcept { return listType_; }
const PlateKeyword& AccessListFilter::keyword() const noexcept { return keyword_; }

PageRequest::PageRequest(const std::uint64_t page) : page_(page) {
    requirePositiveJsonSafe(page_, "page");
    if (page_ > maximumPage()) {
        throw DomainError("page exceeds the supported maximum");
    }
}
std::uint64_t PageRequest::page() const noexcept { return page_; }

RecognitionTask::RecognitionTask(RecognitionId recognitionId)
    : recognitionId_(std::move(recognitionId)) {}
const RecognitionId& RecognitionTask::recognitionId() const noexcept { return recognitionId_; }

}  // namespace ocrservice::domain
