#include "AuthTypes.h"

#include <chrono>

namespace ocrservice::services::auth {

domain::UtcTimePoint SystemAuthClock::nowUtc() {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return domain::UtcTimePoint(milliseconds.count());
}

}  // namespace ocrservice::services::auth
