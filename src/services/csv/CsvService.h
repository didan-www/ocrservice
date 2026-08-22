#pragma once

#include <memory>
#include <stdexcept>
#include <string_view>
#include <variant>

#include "Ports.h"

namespace ocrservice::services::csv {

enum class CsvFailure { databaseUnavailable, internal };

class ICsvSink {
public:
    virtual ~ICsvSink() = default;
    virtual void write(std::string_view bytes) = 0;
};

class CsvStreamError final : public std::runtime_error {
public:
    CsvStreamError();
};

class CsvExport final {
public:
    explicit CsvExport(std::unique_ptr<domain::IHistoryCursor> cursor);
    CsvExport(CsvExport&&) noexcept;
    CsvExport& operator=(CsvExport&&) noexcept;
    CsvExport(const CsvExport&) = delete;
    CsvExport& operator=(const CsvExport&) = delete;

    void writeTo(ICsvSink& sink);

private:
    std::unique_ptr<domain::IHistoryCursor> cursor_;
    bool started_ = false;
};

using CsvPrepareResult = std::variant<CsvExport, CsvFailure>;

class ICsvService {
public:
    virtual ~ICsvService() = default;
    virtual CsvPrepareResult prepare(const domain::HistoryFilter& filter) = 0;
};

class CsvService final : public ICsvService {
public:
    explicit CsvService(domain::IRecognitionRepository& recognitions);
    CsvPrepareResult prepare(const domain::HistoryFilter& filter) override;

private:
    domain::IRecognitionRepository& recognitions_;
};

}  // namespace ocrservice::services::csv
