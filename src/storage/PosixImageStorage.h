#pragma once

#include <filesystem>
#include <memory>

#include "ImageValidator.h"
#include "Ports.h"
#include "PosixFileOps.h"

namespace ocrservice::storage {

class PosixImageStorage final : public domain::IImageStorage {
public:
    explicit PosixImageStorage(std::filesystem::path imageRoot);
    PosixImageStorage(
        std::filesystem::path imageRoot,
        std::shared_ptr<detail::PosixFileOps> fileOps);

    domain::StorageResult<domain::StoredImage> saveAtomically(
        const domain::SaveImageCommand& command) override;
    domain::StorageResult<domain::ImageFile> openForRead(
        const domain::RelativeImagePath& path) override;
    void removeBestEffort(const domain::RelativeImagePath& path) noexcept override;

private:
    std::filesystem::path imageRoot_;
    std::shared_ptr<detail::PosixFileOps> fileOps_;
    ImageValidator validator_;
};

}  // namespace ocrservice::storage
