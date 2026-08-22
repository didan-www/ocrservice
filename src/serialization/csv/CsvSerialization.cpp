#include "CsvSerialization.h"

#include <array>
#include <optional>
#include <string>

#include "ProtocolTime.h"

namespace ocrservice::serialization::csv {
namespace {

constexpr std::string_view kBom = "\xEF\xBB\xBF";
constexpr std::string_view kHeader =
    "记录ID,设备,车牌号,状态,错误码,错误信息,拍摄时间,开始时间,完成时间,耗时(ms)";

std::string optionalPlate(const domain::RecognitionSnapshot& snapshot) {
    return snapshot.plateNumber() ? snapshot.plateNumber()->value() : std::string{};
}

std::string optionalErrorCode(const domain::RecognitionSnapshot& snapshot) {
    return snapshot.errorCode() ? std::string(domain::toString(*snapshot.errorCode())) :
                                  std::string{};
}

std::string optionalErrorMessage(const domain::RecognitionSnapshot& snapshot) {
    return snapshot.errorMessage().value_or(std::string{});
}

std::string optionalTime(const std::optional<domain::UtcTimePoint>& value) {
    return value ? time::formatProtocolTime(*value) : std::string{};
}

std::string optionalDuration(const std::optional<std::uint64_t>& value) {
    return value ? std::to_string(*value) : std::string{};
}

}  // namespace

std::string escapeField(const std::string_view value) {
    if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(value);
    }
    std::string escaped;
    escaped.reserve(value.size() + 2U);
    escaped.push_back('"');
    for (const char character : value) {
        if (character == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

std::string header() {
    std::string result;
    result.reserve(kBom.size() + kHeader.size() + 2U);
    result.append(kBom);
    result.append(kHeader);
    result.append("\r\n");
    return result;
}

std::string record(const domain::RecognitionRecord& value) {
    const auto& snapshot = value.snapshot();
    const std::array<std::string, 10> fields = {
        snapshot.recognitionId().toString(),
        snapshot.deviceId().value(),
        optionalPlate(snapshot),
        std::string(domain::toString(snapshot.status())),
        optionalErrorCode(snapshot),
        optionalErrorMessage(snapshot),
        time::formatProtocolTime(snapshot.capturedAt()),
        time::formatProtocolTime(snapshot.startedAt()),
        optionalTime(snapshot.completedAt()),
        optionalDuration(snapshot.durationMs())};
    std::string result;
    for (std::size_t index = 0U; index < fields.size(); ++index) {
        if (index != 0U) {
            result.push_back(',');
        }
        result.append(escapeField(fields[index]));
    }
    result.append("\r\n");
    return result;
}

}  // namespace ocrservice::serialization::csv
