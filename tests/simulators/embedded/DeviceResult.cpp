#include "DeviceResult.h"

#include <limits>
#include <regex>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "Identifiers.h"
#include "ProtocolTime.h"
#include "Utf8.h"

namespace ocrservice::tests::embedded {
namespace {

using Json = nlohmann::json;

Json parseStrict(const std::string& payload) {
    if (payload.empty() || payload.size() > 2U * 1024U * 1024U) {
        throw std::runtime_error("device result payload size is invalid");
    }
    bool duplicate = false;
    std::vector<std::set<std::string>> objectKeys;
    const auto callback = [&duplicate, &objectKeys](
                              int depth,
                              Json::parse_event_t event,
                              Json& parsed) {
        const auto index = static_cast<std::size_t>(depth);
        if (event == Json::parse_event_t::object_start) {
            if (objectKeys.size() <= index) {
                objectKeys.resize(index + 1U);
            }
            objectKeys[index].clear();
        } else if (event == Json::parse_event_t::key) {
            if (objectKeys.size() <= index) {
                objectKeys.resize(index + 1U);
            }
            duplicate = !objectKeys[index].insert(parsed.get<std::string>()).second || duplicate;
        }
        return true;
    };
    auto result = Json::parse(payload, callback, true, false);
    if (duplicate) {
        throw std::runtime_error("device result contains duplicate fields");
    }
    return result;
}

std::set<std::string> keys(const Json& value) {
    std::set<std::string> result;
    if (!value.is_object()) {
        return result;
    }
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        result.insert(key);
    }
    return result;
}

std::string requiredString(const Json& value, const char* name) {
    if (!value.is_string()) {
        throw std::runtime_error(std::string(name) + " must be a string");
    }
    auto result = value.get<std::string>();
    if (result.empty()) {
        throw std::runtime_error(std::string(name) + " must not be empty");
    }
    return result;
}

bool isProtocolTime(const Json& value) {
    static const std::regex pattern(
        "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\\.[0-9]{3}\\+08:00$");
    if (!value.is_string() || !std::regex_match(value.get<std::string>(), pattern)) {
        return false;
    }
    try {
        (void)ocrservice::serialization::time::parseProtocolTime(value.get<std::string>());
        return true;
    } catch (const ocrservice::serialization::time::TimeError&) {
        return false;
    }
}

}  // namespace

DeviceResult parseDeviceResult(const std::string& payload) {
    const auto root = parseStrict(payload);
    const std::set<std::string> expected = {
        "schemaVersion", "recognitionId", "revision", "deviceId", "status",
        "plateNumber", "errorCode", "errorMessage", "capturedAt", "startedAt",
        "completedAt", "durationMs", "gateAction"};
    if (keys(root) != expected || !root.at("schemaVersion").is_number_integer() ||
        root.at("schemaVersion").get<int>() != 1 ||
        !root.at("revision").is_number_unsigned()) {
        throw std::runtime_error("device result has an invalid field set or version");
    }

    DeviceResult result;
    result.recognitionId = requiredString(root.at("recognitionId"), "recognitionId");
    result.revision = root.at("revision").get<std::uint64_t>();
    result.deviceId = requiredString(root.at("deviceId"), "deviceId");
    result.status = requiredString(root.at("status"), "status");
    const auto action = requiredString(root.at("gateAction"), "gateAction");
    try {
        (void)ocrservice::domain::RecognitionId::parse(result.recognitionId);
        (void)ocrservice::domain::DeviceId::parse(result.deviceId);
    } catch (const ocrservice::domain::DomainError&) {
        throw std::runtime_error("device result identifiers are invalid");
    }
    ocrservice::serialization::time::UtcTimePoint started(0);
    ocrservice::serialization::time::UtcTimePoint completed(0);
    try {
        started = ocrservice::serialization::time::parseProtocolTime(
            root.at("startedAt").get<std::string>());
        completed = ocrservice::serialization::time::parseProtocolTime(
            root.at("completedAt").get<std::string>());
    } catch (const ocrservice::serialization::time::TimeError&) {
        throw std::runtime_error("device result time is invalid");
    }
    if (result.revision < 2U ||
        result.revision > 9007199254740991ULL ||
        !isProtocolTime(root.at("capturedAt")) || !isProtocolTime(root.at("startedAt")) ||
        !isProtocolTime(root.at("completedAt")) ||
        !root.at("durationMs").is_number_unsigned() ||
        root.at("durationMs").get<std::uint64_t>() > 9007199254740991ULL ||
        completed < started) {
        throw std::runtime_error("device result has invalid common fields");
    }

    if (result.status == "SUCCEEDED" && action == "OPEN") {
        if (!root.at("plateNumber").is_string() ||
            root.at("plateNumber").get<std::string>().empty() ||
            !root.at("errorCode").is_null() || !root.at("errorMessage").is_null()) {
            throw std::runtime_error("successful device result is inconsistent");
        }
        try {
            const auto rawPlate = root.at("plateNumber").get<std::string>();
            const auto plate = ocrservice::domain::PlateNumber::parse(rawPlate);
            if (plate.value() != rawPlate) {
                throw std::runtime_error("successful device result plate is not normalized");
            }
        } catch (const ocrservice::domain::DomainError&) {
            throw std::runtime_error("successful device result plate is invalid");
        }
        result.gateAction = GateAction::open;
    } else if (result.status == "FAILED" && action == "KEEP_CLOSED") {
        if (!root.at("plateNumber").is_null() || !root.at("errorCode").is_string() ||
            root.at("errorCode").get<std::string>().empty() ||
            !root.at("errorMessage").is_string() ||
            root.at("errorMessage").get<std::string>().empty()) {
            throw std::runtime_error("failed device result is inconsistent");
        }
        try {
            const auto& errorCode = root.at("errorCode").get_ref<const std::string&>();
            const std::set<std::string> allowedCodes = {
                "PLATE_NOT_FOUND", "PLATE_RECOGNITION_FAILED", "MODEL_INFERENCE_ERROR",
                "SERVER_RESTARTED"};
            if (allowedCodes.count(errorCode) == 0U) {
                throw std::runtime_error("failed device result code is unknown");
            }
            if (ocrservice::text::countCodePoints(
                    errorCode) > 64U ||
                ocrservice::text::countCodePoints(
                    root.at("errorMessage").get<std::string>()) > 512U) {
                throw std::runtime_error("failed device result error text is too long");
            }
        } catch (const ocrservice::text::Utf8Error&) {
            throw std::runtime_error("failed device result error text is invalid UTF-8");
        }
        result.gateAction = GateAction::keepClosed;
    } else {
        throw std::runtime_error("device result action does not match final status");
    }
    return result;
}

bool ResultDeduplicator::shouldExecute(const DeviceResult& result) {
    return seen_.emplace(result.recognitionId, result.revision).second;
}

std::size_t ResultDeduplicator::executionCount() const noexcept { return seen_.size(); }

const char* toString(const GateAction action) noexcept {
    return action == GateAction::open ? "OPEN" : "KEEP_CLOSED";
}

}  // namespace ocrservice::tests::embedded
