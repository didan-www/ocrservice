#include "HistoryService.h"

#include <optional>
#include <utility>

namespace ocrservice::services::history {
namespace {

HistoryFailure mapRepositoryFailure(const domain::RepositoryFailure failure) noexcept {
    return failure == domain::RepositoryFailure::unavailable ?
               HistoryFailure::databaseUnavailable : HistoryFailure::internal;
}

}  // namespace

HistoryService::HistoryService(
    domain::IRecognitionRepository& recognitions,
    domain::IImageStorage& images)
    : recognitions_(recognitions), images_(images) {}

DetailResult HistoryService::detail(const domain::RecognitionId& recognitionId) {
    auto result = recognitions_.findById(recognitionId);
    if (std::holds_alternative<domain::RepositoryFailure>(result)) {
        return mapRepositoryFailure(std::get<domain::RepositoryFailure>(result));
    }
    auto record = std::get<std::optional<domain::RecognitionRecord>>(std::move(result));
    if (!record) {
        return HistoryFailure::recognitionNotFound;
    }
    return std::move(*record);
}

PageResult HistoryService::query(
    const domain::HistoryFilter& filter,
    const domain::PageRequest& page) {
    auto result = recognitions_.queryHistory(filter, page);
    if (std::holds_alternative<domain::RepositoryFailure>(result)) {
        return mapRepositoryFailure(std::get<domain::RepositoryFailure>(result));
    }
    return std::get<domain::PageResult<domain::RecognitionRecord>>(std::move(result));
}

ImageResult HistoryService::openImage(const domain::RecognitionId& recognitionId) {
    auto found = recognitions_.findById(recognitionId);
    if (std::holds_alternative<domain::RepositoryFailure>(found)) {
        return mapRepositoryFailure(std::get<domain::RepositoryFailure>(found));
    }
    auto record = std::get<std::optional<domain::RecognitionRecord>>(std::move(found));
    if (!record) {
        return HistoryFailure::imageNotFound;
    }
    auto opened = images_.openForRead(record->relativeImagePath());
    if (std::holds_alternative<domain::StorageFailure>(opened)) {
        return std::get<domain::StorageFailure>(opened) == domain::StorageFailure::notFound ?
                   HistoryFailure::imageNotFound : HistoryFailure::internal;
    }
    auto image = std::get<domain::ImageFile>(std::move(opened));
    if (image.mime() != record->imageMime() ||
        image.sizeBytes() != record->imageSizeBytes()) {
        return HistoryFailure::internal;
    }
    return image;
}

}  // namespace ocrservice::services::history
