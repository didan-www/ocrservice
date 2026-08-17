#include "RequestId.h"

#include <array>
#include <cstdint>
#include <stdexcept>

#include <openssl/rand.h>

namespace ocrservice::http::middleware {

domain::Uuid RandomRequestIdGenerator::next() {
    std::array<std::uint8_t, 16> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("request ID random source failed");
    }
    return domain::Uuid::v4(bytes);
}

std::shared_ptr<IRequestIdGenerator> makeRandomRequestIdGenerator() {
    return std::make_shared<RandomRequestIdGenerator>();
}

}  // namespace ocrservice::http::middleware
