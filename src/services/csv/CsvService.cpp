#include "CsvService.h"

#include <optional>
#include <utility>

#include "CsvSerialization.h"

namespace ocrservice::services::csv {

CsvStreamError::CsvStreamError() : std::runtime_error("CSV stream failed") {}

CsvExport::CsvExport(std::unique_ptr<domain::IHistoryCursor> cursor)
    : cursor_(std::move(cursor)) {
    if (!cursor_) {
        throw std::invalid_argument("CSV cursor must not be null");
    }
}

CsvExport::CsvExport(CsvExport&&) noexcept = default;
CsvExport& CsvExport::operator=(CsvExport&&) noexcept = default;

void CsvExport::writeTo(ICsvSink& sink) {
    if (started_ || !cursor_) {
        throw CsvStreamError();
    }
    started_ = true;
    sink.write(serialization::csv::header());
    while (true) {
        auto next = cursor_->next();
        if (std::holds_alternative<domain::RepositoryFailure>(next)) {
            throw CsvStreamError();
        }
        auto record = std::get<std::optional<domain::RecognitionRecord>>(std::move(next));
        if (!record) {
            cursor_.reset();
            return;
        }
        sink.write(serialization::csv::record(*record));
    }
}

CsvService::CsvService(domain::IRecognitionRepository& recognitions)
    : recognitions_(recognitions) {}

CsvPrepareResult CsvService::prepare(const domain::HistoryFilter& filter) {
    auto opened = recognitions_.openHistoryCursor(filter);
    if (std::holds_alternative<domain::RepositoryFailure>(opened)) {
        return std::get<domain::RepositoryFailure>(opened) ==
                       domain::RepositoryFailure::unavailable ?
                   CsvFailure::databaseUnavailable : CsvFailure::internal;
    }
    return CsvExport(
        std::get<std::unique_ptr<domain::IHistoryCursor>>(std::move(opened)));
}

}  // namespace ocrservice::services::csv
