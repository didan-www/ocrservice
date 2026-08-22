#pragma once

#include <string>
#include <string_view>

#include "Ports.h"

namespace ocrservice::serialization::csv {

std::string escapeField(std::string_view value);
std::string header();
std::string record(const domain::RecognitionRecord& value);

}  // namespace ocrservice::serialization::csv
