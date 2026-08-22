#pragma once

#include <variant>

#include "Ports.h"

namespace ocrservice::services::history {

enum class HistoryFailure {
    recognitionNotFound,
    imageNotFound,
    databaseUnavailable,
    internal
};

using DetailResult = std::variant<domain::RecognitionRecord, HistoryFailure>;
using PageResult =
    std::variant<domain::PageResult<domain::RecognitionRecord>, HistoryFailure>;
using ImageResult = std::variant<domain::ImageFile, HistoryFailure>;

class IHistoryService {
public:
    virtual ~IHistoryService() = default;
    virtual DetailResult detail(const domain::RecognitionId& recognitionId) = 0;
    virtual PageResult query(
        const domain::HistoryFilter& filter,
        const domain::PageRequest& page) = 0;
    virtual ImageResult openImage(const domain::RecognitionId& recognitionId) = 0;
};

class HistoryService final : public IHistoryService {
public:
    HistoryService(
        domain::IRecognitionRepository& recognitions,
        domain::IImageStorage& images);

    DetailResult detail(const domain::RecognitionId& recognitionId) override;
    PageResult query(
        const domain::HistoryFilter& filter,
        const domain::PageRequest& page) override;
    ImageResult openImage(const domain::RecognitionId& recognitionId) override;

private:
    domain::IRecognitionRepository& recognitions_;
    domain::IImageStorage& images_;
};

}  // namespace ocrservice::services::history
