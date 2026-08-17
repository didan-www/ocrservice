#include "PosixImageStorage.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/rand.h>
#include <spdlog/spdlog.h>
#include <unistd.h>

namespace ocrservice::storage {
namespace {

constexpr std::size_t kMaximumDownloadImageBytes = 20U * 1024U * 1024U;
constexpr std::int64_t kMillisecondsPerDay = 86400000;
constexpr int kTemporaryNameAttempts = 16;

class FileDescriptor final {
public:
    FileDescriptor(std::shared_ptr<detail::PosixFileOps> fileOps, const int fd) noexcept
        : fileOps_(std::move(fileOps)), fd_(fd) {}

    ~FileDescriptor() {
        if (fd_ >= 0) {
            (void)fileOps_->close(fd_);
        }
    }

    FileDescriptor(FileDescriptor&& other) noexcept
        : fileOps_(std::move(other.fileOps_)), fd_(other.release()) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                (void)fileOps_->close(fd_);
            }
            fileOps_ = std::move(other.fileOps_);
            fd_ = other.release();
        }
        return *this;
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    int get() const noexcept { return fd_; }

    int release() noexcept {
        const int released = fd_;
        fd_ = -1;
        return released;
    }

    bool closeChecked() noexcept {
        if (fd_ < 0) {
            return true;
        }
        const int closing = release();
        return fileOps_->close(closing) == 0;
    }

private:
    std::shared_ptr<detail::PosixFileOps> fileOps_;
    int fd_;
};

class PosixImageReader final : public domain::ImageReader {
public:
    PosixImageReader(std::shared_ptr<detail::PosixFileOps> fileOps, const int fd) noexcept
        : fileOps_(std::move(fileOps)), fd_(fileOps_, fd) {}

    std::size_t read(std::uint8_t* destination, const std::size_t capacity) override {
        if (capacity == 0U) {
            return 0U;
        }
        const auto maximumCallSize = static_cast<std::size_t>(
            std::numeric_limits<ssize_t>::max());
        const auto callSize = std::min(capacity, maximumCallSize);
        for (;;) {
            const auto result = fileOps_->read(fd_.get(), destination, callSize);
            if (result >= 0) {
                return static_cast<std::size_t>(result);
            }
            if (errno != EINTR) {
                throw std::runtime_error("image file read failed");
            }
        }
    }

private:
    std::shared_ptr<detail::PosixFileOps> fileOps_;
    FileDescriptor fd_;
};

void logCleanupFailure() noexcept {
    try {
        spdlog::warn("module=image_storage code=IMAGE_CLEANUP_FAILED");
    } catch (...) {
    }
}

std::int64_t floorDivide(const std::int64_t value, const std::int64_t divisor) noexcept {
    const auto quotient = value / divisor;
    const auto remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

void civilFromDays(
    std::int64_t days,
    int& year,
    unsigned int& month,
    unsigned int& day) noexcept {
    days += 719468;
    const auto era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned int>(days - era * 146097);
    const auto yearOfEra =
        (dayOfEra - dayOfEra / 1460U + dayOfEra / 36524U - dayOfEra / 146096U) / 365U;
    year = static_cast<int>(yearOfEra) + static_cast<int>(era) * 400;
    const auto dayOfYear =
        dayOfEra - (365U * yearOfEra + yearOfEra / 4U - yearOfEra / 100U);
    const auto monthPrime = (5U * dayOfYear + 2U) / 153U;
    day = dayOfYear - (153U * monthPrime + 2U) / 5U + 1U;
    month = monthPrime < 10U ? monthPrime + 3U : monthPrime - 9U;
    year += month <= 2U ? 1 : 0;
}

std::string utcDate(const domain::UtcTimePoint timePoint) {
    const auto days = floorDivide(timePoint.unixMilliseconds(), kMillisecondsPerDay);
    int year = 0;
    unsigned int month = 0U;
    unsigned int day = 0U;
    civilFromDays(days, year, month, day);
    if (year < 0 || year > 9999) {
        throw std::runtime_error("image date is outside the supported range");
    }
    std::array<char, 11> output{};
    const int written = std::snprintf(
        output.data(), output.size(), "%04d/%02u/%02u", year, month, day);
    if (written != 10) {
        throw std::runtime_error("image date could not be formatted");
    }
    return std::string(output.data(), static_cast<std::size_t>(written));
}

std::string extensionFor(const domain::ImageFormat format) {
    switch (format) {
        case domain::ImageFormat::jpeg:
            return ".jpg";
        case domain::ImageFormat::png:
            return ".png";
    }
    throw std::runtime_error("image format is invalid");
}

domain::ImageMime mimeFromPath(const std::string_view path) {
    if (path.size() >= 4U && path.substr(path.size() - 4U) == ".jpg") {
        return domain::ImageMime::jpeg;
    }
    if (path.size() >= 4U && path.substr(path.size() - 4U) == ".png") {
        return domain::ImageMime::png;
    }
    throw std::runtime_error("image path extension is invalid");
}

std::vector<std::string> splitPath(const std::string_view path) {
    std::vector<std::string> segments;
    std::size_t start = 0U;
    while (start < path.size()) {
        const auto separator = path.find('/', start);
        const auto end = separator == std::string_view::npos ? path.size() : separator;
        segments.emplace_back(path.substr(start, end - start));
        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1U;
    }
    return segments;
}

FileDescriptor openRoot(
    const std::filesystem::path& imageRoot,
    const std::shared_ptr<detail::PosixFileOps>& fileOps) {
    const int fd = fileOps->open(
        imageRoot.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW, 0);
    return FileDescriptor(fileOps, fd);
}

FileDescriptor openOrCreateDirectory(
    const std::shared_ptr<detail::PosixFileOps>& fileOps,
    const int parentFd,
    const std::string& name) {
    if (fileOps->makeDirectoryAt(parentFd, name.c_str(), 0750) != 0 && errno != EEXIST) {
        return FileDescriptor(fileOps, -1);
    }
    return FileDescriptor(
        fileOps,
        fileOps->openAt(
            parentFd,
            name.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
            0));
}

FileDescriptor openDirectory(
    const std::shared_ptr<detail::PosixFileOps>& fileOps,
    const int parentFd,
    const std::string& name) {
    return FileDescriptor(
        fileOps,
        fileOps->openAt(
            parentFd,
            name.c_str(),
            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW,
            0));
}

std::string randomTemporaryName(const std::string& recognitionId) {
    std::array<unsigned char, 16> randomBytes{};
    if (RAND_bytes(randomBytes.data(), static_cast<int>(randomBytes.size())) != 1) {
        throw std::runtime_error("temporary image name could not be generated");
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string name;
    name.reserve(1U + recognitionId.size() + 1U + randomBytes.size() * 2U + 4U);
    name.push_back('.');
    name.append(recognitionId);
    name.push_back('.');
    for (const auto value : randomBytes) {
        name.push_back(kHex[value >> 4U]);
        name.push_back(kHex[value & 0x0FU]);
    }
    name.append(".tmp");
    return name;
}

bool writeAll(
    const std::shared_ptr<detail::PosixFileOps>& fileOps,
    const int fd,
    const domain::ByteView bytes) noexcept {
    std::size_t written = 0U;
    while (written < bytes.size()) {
        const auto result = fileOps->write(
            fd, bytes.data() + written, bytes.size() - written);
        if (result > 0) {
            written += static_cast<std::size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

void unlinkForCleanup(
    const std::shared_ptr<detail::PosixFileOps>& fileOps,
    const int directoryFd,
    const std::string& name) noexcept {
    if (fileOps->unlinkAt(directoryFd, name.c_str(), 0) != 0 && errno != ENOENT) {
        logCleanupFailure();
    }
}

domain::StorageFailure openFailure(const int error) noexcept {
    return error == ENOENT ? domain::StorageFailure::notFound
                           : domain::StorageFailure::readFailed;
}

}  // namespace

PosixImageStorage::PosixImageStorage(std::filesystem::path imageRoot)
    : PosixImageStorage(std::move(imageRoot), detail::makeSystemPosixFileOps()) {}

PosixImageStorage::PosixImageStorage(
    std::filesystem::path imageRoot,
    std::shared_ptr<detail::PosixFileOps> fileOps)
    : imageRoot_(std::move(imageRoot)), fileOps_(std::move(fileOps)) {
    if (imageRoot_.empty() || !fileOps_) {
        throw std::invalid_argument("image storage configuration is invalid");
    }
}

domain::StorageResult<domain::StoredImage> PosixImageStorage::saveAtomically(
    const domain::SaveImageCommand& command) {
    try {
        auto validation = validator_.validate(command.compressedBytes());
        if (const auto* failure = std::get_if<domain::StorageFailure>(&validation)) {
            return *failure;
        }
        const auto& image = std::get<ValidatedImage>(validation);
        if (image.format() != command.format() || !(image.digest() == command.digest())) {
            return domain::StorageFailure::invalidImage;
        }

        const auto date = utcDate(command.capturedAtUtc());
        const auto recognitionId = command.recognitionId().toString();
        const auto extension = extensionFor(image.format());
        const auto relativePath = domain::RelativeImagePath::parseGenerated(
            date + "/" + recognitionId + extension);
        const auto dateSegments = splitPath(date);

        auto root = openRoot(imageRoot_, fileOps_);
        if (root.get() < 0) {
            return domain::StorageFailure::writeFailed;
        }
        auto year = openOrCreateDirectory(fileOps_, root.get(), dateSegments[0]);
        if (year.get() < 0) {
            return domain::StorageFailure::writeFailed;
        }
        auto month = openOrCreateDirectory(fileOps_, year.get(), dateSegments[1]);
        if (month.get() < 0) {
            return domain::StorageFailure::writeFailed;
        }
        auto day = openOrCreateDirectory(fileOps_, month.get(), dateSegments[2]);
        if (day.get() < 0) {
            return domain::StorageFailure::writeFailed;
        }

        std::string temporaryName;
        FileDescriptor temporary(fileOps_, -1);
        for (int attempt = 0; attempt < kTemporaryNameAttempts; ++attempt) {
            temporaryName = randomTemporaryName(recognitionId);
            temporary = FileDescriptor(
                fileOps_,
                fileOps_->openAt(
                    day.get(),
                    temporaryName.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0640));
            if (temporary.get() >= 0 || errno != EEXIST) {
                break;
            }
        }
        if (temporary.get() < 0) {
            return domain::StorageFailure::writeFailed;
        }

        const bool writeSucceeded =
            writeAll(fileOps_, temporary.get(), command.compressedBytes());
        const bool fileSyncSucceeded =
            writeSucceeded && fileOps_->sync(temporary.get()) == 0;
        const bool closeSucceeded = temporary.closeChecked();
        if (!writeSucceeded || !fileSyncSucceeded || !closeSucceeded) {
            unlinkForCleanup(fileOps_, day.get(), temporaryName);
            return domain::StorageFailure::writeFailed;
        }

        const auto finalName = recognitionId + extension;
        if (fileOps_->renameAt(
                day.get(), temporaryName.c_str(), day.get(), finalName.c_str()) != 0) {
            unlinkForCleanup(fileOps_, day.get(), temporaryName);
            return domain::StorageFailure::writeFailed;
        }
        if (fileOps_->sync(day.get()) != 0) {
            unlinkForCleanup(fileOps_, day.get(), finalName);
            (void)fileOps_->sync(day.get());
            return domain::StorageFailure::writeFailed;
        }

        return domain::StoredImage(
            relativePath,
            domain::mimeFor(image.format()),
            static_cast<std::uint64_t>(image.sizeBytes()),
            image.digest());
    } catch (...) {
        return domain::StorageFailure::internal;
    }
}

domain::StorageResult<domain::ImageFile> PosixImageStorage::openForRead(
    const domain::RelativeImagePath& path) {
    try {
        const auto mime = mimeFromPath(path.value());
        const auto segments = splitPath(path.value());
        if (segments.size() < 2U) {
            return domain::StorageFailure::readFailed;
        }

        auto directory = openRoot(imageRoot_, fileOps_);
        if (directory.get() < 0) {
            return openFailure(errno);
        }
        for (std::size_t index = 0U; index + 1U < segments.size(); ++index) {
            auto next = openDirectory(fileOps_, directory.get(), segments[index]);
            if (next.get() < 0) {
                return openFailure(errno);
            }
            directory = std::move(next);
        }

        FileDescriptor file(
            fileOps_,
            fileOps_->openAt(
                directory.get(),
                segments.back().c_str(),
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW,
                0));
        if (file.get() < 0) {
            return openFailure(errno);
        }
        struct stat status {};
        if (fileOps_->fileStatus(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
            status.st_size <= 0 ||
            static_cast<std::uintmax_t>(status.st_size) > kMaximumDownloadImageBytes) {
            return domain::StorageFailure::readFailed;
        }
        const auto sizeBytes = static_cast<std::uint64_t>(status.st_size);
        return domain::ImageFile(
            std::make_unique<PosixImageReader>(fileOps_, file.release()), mime, sizeBytes);
    } catch (...) {
        return domain::StorageFailure::internal;
    }
}

void PosixImageStorage::removeBestEffort(const domain::RelativeImagePath& path) noexcept {
    try {
        const auto segments = splitPath(path.value());
        if (segments.size() < 2U) {
            logCleanupFailure();
            return;
        }
        auto directory = openRoot(imageRoot_, fileOps_);
        if (directory.get() < 0) {
            if (errno != ENOENT) {
                logCleanupFailure();
            }
            return;
        }
        for (std::size_t index = 0U; index + 1U < segments.size(); ++index) {
            auto next = openDirectory(fileOps_, directory.get(), segments[index]);
            if (next.get() < 0) {
                if (errno != ENOENT) {
                    logCleanupFailure();
                }
                return;
            }
            directory = std::move(next);
        }
        if (fileOps_->unlinkAt(directory.get(), segments.back().c_str(), 0) != 0) {
            if (errno != ENOENT) {
                logCleanupFailure();
            }
            return;
        }
        if (fileOps_->sync(directory.get()) != 0) {
            logCleanupFailure();
        }
    } catch (...) {
        logCleanupFailure();
    }
}

}  // namespace ocrservice::storage
