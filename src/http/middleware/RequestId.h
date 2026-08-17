#pragma once

#include <memory>

#include "Identifiers.h"

namespace ocrservice::http::middleware {

class IRequestIdGenerator {
public:
    virtual ~IRequestIdGenerator() = default;
    virtual domain::Uuid next() = 0;
};

class RandomRequestIdGenerator final : public IRequestIdGenerator {
public:
    domain::Uuid next() override;
};

std::shared_ptr<IRequestIdGenerator> makeRandomRequestIdGenerator();

}  // namespace ocrservice::http::middleware
