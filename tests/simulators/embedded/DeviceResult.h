#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>

namespace ocrservice::tests::embedded {

enum class GateAction { open, keepClosed };

struct DeviceResult final {
    std::string recognitionId;
    std::uint64_t revision = 0;
    std::string deviceId;
    std::string status;
    GateAction gateAction = GateAction::keepClosed;
};

DeviceResult parseDeviceResult(const std::string& payload);

class ResultDeduplicator final {
public:
    bool shouldExecute(const DeviceResult& result);
    std::size_t executionCount() const noexcept;

private:
    std::set<std::pair<std::string, std::uint64_t>> seen_;
};

const char* toString(GateAction action) noexcept;

}  // namespace ocrservice::tests::embedded
