#pragma once

#include <string>
#include <string_view>

namespace ocrservice::logging {

class LogSanitizer final {
public:
    static std::string sanitize(std::string_view value);
};

}  // namespace ocrservice::logging
