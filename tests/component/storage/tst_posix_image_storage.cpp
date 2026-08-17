#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <fcntl.h>
#include <gtest/gtest.h>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <unistd.h>

#include "ImageValidator.h"
#include "PosixImageStorage.h"
#include "ProtocolTime.h"

namespace {

namespace fs = std::filesystem;
using namespace ocrservice;
using domain::ByteView;
using domain::ImageFormat;
using domain::RecognitionId;
using domain::SaveImageCommand;
using domain::StorageFailure;
using domain::StoredImage;
using storage::ImageValidator;
using storage::PosixImageStorage;
using storage::ValidatedImage;

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto pattern = (fs::temp_directory_path() / "ocrservice-storage-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* const created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("test temporary directory creation failed");
        }
        path_ = created;
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

class InjectingFileOps final : public storage::detail::PosixFileOps {
public:
    InjectingFileOps() : delegate_(storage::detail::makeSystemPosixFileOps()) {}

    int open(const char* path, const int flags, const mode_t mode) noexcept override {
        return delegate_->open(path, flags, mode);
    }

    int openAt(
        const int directoryFd,
        const char* path,
        const int flags,
        const mode_t mode) noexcept override {
        if ((flags & O_CREAT) != 0) {
            observedCreateFlags.store(flags, std::memory_order_relaxed);
        }
        return delegate_->openAt(directoryFd, path, flags, mode);
    }

    int makeDirectoryAt(
        const int directoryFd,
        const char* path,
        const mode_t mode) noexcept override {
        return delegate_->makeDirectoryAt(directoryFd, path, mode);
    }

    ssize_t write(const int fd, const void* data, const std::size_t size) noexcept override {
        writeCalls.fetch_add(1U, std::memory_order_relaxed);
        if (interruptNextWrite.exchange(false, std::memory_order_relaxed)) {
            errno = EINTR;
            return -1;
        }
        return delegate_->write(fd, data, std::min(size, maximumWriteSize));
    }

    ssize_t read(const int fd, void* data, const std::size_t size) noexcept override {
        readCalls.fetch_add(1U, std::memory_order_relaxed);
        if (interruptNextRead.exchange(false, std::memory_order_relaxed)) {
            errno = EINTR;
            return -1;
        }
        return delegate_->read(fd, data, size);
    }

    int sync(const int fd) noexcept override {
        const auto call = syncCalls.fetch_add(1U, std::memory_order_relaxed) + 1U;
        if (failSyncCall != 0U && call == failSyncCall) {
            errno = EIO;
            return -1;
        }
        return delegate_->sync(fd);
    }

    int close(const int fd) noexcept override {
        const auto call = closeCalls.fetch_add(1U, std::memory_order_relaxed) + 1U;
        const int result = delegate_->close(fd);
        if (failCloseCall != 0U && call == failCloseCall) {
            errno = EIO;
            return -1;
        }
        return result;
    }

    int renameAt(
        const int oldDirectoryFd,
        const char* oldPath,
        const int newDirectoryFd,
        const char* newPath) noexcept override {
        if (failRename) {
            errno = EIO;
            return -1;
        }
        return delegate_->renameAt(oldDirectoryFd, oldPath, newDirectoryFd, newPath);
    }

    int unlinkAt(const int directoryFd, const char* path, const int flags) noexcept override {
        if (failUnlink) {
            errno = EACCES;
            return -1;
        }
        return delegate_->unlinkAt(directoryFd, path, flags);
    }

    int fileStatus(const int fd, struct stat* const status) noexcept override {
        return delegate_->fileStatus(fd, status);
    }

    std::size_t maximumWriteSize = std::numeric_limits<std::size_t>::max();
    std::size_t failSyncCall = 0U;
    std::size_t failCloseCall = 0U;
    bool failRename = false;
    bool failUnlink = false;
    std::atomic<bool> interruptNextWrite{false};
    std::atomic<bool> interruptNextRead{false};
    std::atomic<std::size_t> writeCalls{0U};
    std::atomic<std::size_t> readCalls{0U};
    std::atomic<std::size_t> syncCalls{0U};
    std::atomic<std::size_t> closeCalls{0U};
    std::atomic<int> observedCreateFlags{0};

private:
    std::shared_ptr<storage::detail::PosixFileOps> delegate_;
};

std::vector<std::uint8_t> encode(const std::string& extension) {
    cv::Mat image(5, 7, CV_8UC3, cv::Scalar(22, 44, 88));
    std::vector<unsigned char> encoded;
    if (!cv::imencode(extension, image, encoded)) {
        throw std::runtime_error("test image encoding failed");
    }
    return {encoded.begin(), encoded.end()};
}

RecognitionId recognitionId(const std::uint64_t sequence) {
    std::array<std::uint8_t, 16> bytes{};
    for (std::size_t index = 0U; index < sizeof(sequence); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        bytes[15U - index] = static_cast<std::uint8_t>((sequence >> shift) & 0xFFU);
    }
    return RecognitionId(domain::Uuid::v4(bytes));
}

ValidatedImage validate(const std::vector<std::uint8_t>& bytes) {
    auto result = ImageValidator().validate(ByteView(bytes.data(), bytes.size()));
    if (!std::holds_alternative<ValidatedImage>(result)) {
        throw std::runtime_error("test image validation failed");
    }
    return std::get<ValidatedImage>(std::move(result));
}

domain::StorageResult<StoredImage> save(
    PosixImageStorage& storage,
    const std::vector<std::uint8_t>& bytes,
    const RecognitionId& id,
    const domain::UtcTimePoint capturedAt,
    const ImageFormat format) {
    const auto image = validate(bytes);
    return storage.saveAtomically(
        SaveImageCommand(
            id,
            capturedAt,
            format,
            ByteView(bytes.data(), bytes.size()),
            image.digest()));
}

domain::UtcTimePoint capturedAt() {
    return serialization::time::parseProtocolTime("2026-08-15T00:30:00.000+08:00");
}

std::vector<std::uint8_t> readAll(domain::ImageFile& file) {
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 31> buffer{};
    for (;;) {
        const auto count = file.read(buffer.data(), buffer.size());
        if (count == 0U) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
    }
    return bytes;
}

std::size_t regularFileCount(const fs::path& root) {
    std::size_t count = 0U;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

void expectFailure(
    const domain::StorageResult<StoredImage>& result,
    const StorageFailure expected) {
    ASSERT_TRUE(std::holds_alternative<StorageFailure>(result));
    EXPECT_EQ(std::get<StorageFailure>(result), expected);
}

TEST(PosixImageStorageTest, SavesByUtcDateAndActualFormatThenStreamsExactBytes) {
    TemporaryDirectory root;
    PosixImageStorage storage(root.path());
    const auto jpeg = encode(".jpg");
    const auto id = recognitionId(1U);
    auto savedResult = save(storage, jpeg, id, capturedAt(), ImageFormat::jpeg);
    ASSERT_TRUE(std::holds_alternative<StoredImage>(savedResult));
    const auto& saved = std::get<StoredImage>(savedResult);
    EXPECT_EQ(
        saved.relativePath().value(),
        "2026/08/14/" + id.toString() + ".jpg");
    EXPECT_EQ(saved.mime(), domain::ImageMime::jpeg);
    EXPECT_EQ(saved.sizeBytes(), jpeg.size());
    EXPECT_EQ(regularFileCount(root.path()), 1U);

    auto openedResult = storage.openForRead(saved.relativePath());
    ASSERT_TRUE(std::holds_alternative<domain::ImageFile>(openedResult));
    auto& opened = std::get<domain::ImageFile>(openedResult);
    EXPECT_EQ(opened.mime(), domain::ImageMime::jpeg);
    EXPECT_EQ(opened.sizeBytes(), jpeg.size());
    EXPECT_EQ(readAll(opened), jpeg);
}

TEST(PosixImageStorageTest, UsesDecodedPngFormatAndRejectsMismatchedCommandFormat) {
    TemporaryDirectory root;
    PosixImageStorage storage(root.path());
    const auto png = encode(".png");
    const auto validId = recognitionId(2U);
    auto valid = save(storage, png, validId, capturedAt(), ImageFormat::png);
    ASSERT_TRUE(std::holds_alternative<StoredImage>(valid));
    EXPECT_EQ(
        std::get<StoredImage>(valid).relativePath().value(),
        "2026/08/14/" + validId.toString() + ".png");

    auto mismatched = save(
        storage, png, recognitionId(3U), capturedAt(), ImageFormat::jpeg);
    expectFailure(mismatched, StorageFailure::invalidImage);
    EXPECT_EQ(regularFileCount(root.path()), 1U);
}

TEST(PosixImageStorageTest, RejectsDigestMismatchWithoutCreatingAFile) {
    TemporaryDirectory root;
    PosixImageStorage storage(root.path());
    const auto jpeg = encode(".jpg");
    const auto image = validate(jpeg);
    auto wrongDigestBytes = image.digest().bytes();
    wrongDigestBytes[0] ^= 0xFFU;
    const auto result = storage.saveAtomically(
        SaveImageCommand(
            recognitionId(3U),
            capturedAt(),
            ImageFormat::jpeg,
            ByteView(jpeg.data(), jpeg.size()),
            domain::Sha256Digest(wrongDigestBytes)));
    expectFailure(result, StorageFailure::invalidImage);
    EXPECT_EQ(regularFileCount(root.path()), 0U);
}

TEST(PosixImageStorageTest, CompletesShortWritesAndUsesExclusiveTemporaryCreation) {
    TemporaryDirectory root;
    auto fileOps = std::make_shared<InjectingFileOps>();
    fileOps->maximumWriteSize = 7U;
    PosixImageStorage storage(root.path(), fileOps);
    const auto jpeg = encode(".jpg");
    auto result = save(storage, jpeg, recognitionId(4U), capturedAt(), ImageFormat::jpeg);
    ASSERT_TRUE(std::holds_alternative<StoredImage>(result));
    EXPECT_GT(fileOps->writeCalls.load(), 1U);
    const int flags = fileOps->observedCreateFlags.load();
    EXPECT_NE(flags & O_CREAT, 0);
    EXPECT_NE(flags & O_EXCL, 0);
    EXPECT_EQ(regularFileCount(root.path()), 1U);
}

TEST(PosixImageStorageTest, RetriesInterruptedWriteAndStreamingRead) {
    TemporaryDirectory root;
    auto fileOps = std::make_shared<InjectingFileOps>();
    fileOps->interruptNextWrite.store(true);
    PosixImageStorage storage(root.path(), fileOps);
    const auto jpeg = encode(".jpg");
    auto savedResult = save(
        storage, jpeg, recognitionId(5U), capturedAt(), ImageFormat::jpeg);
    ASSERT_TRUE(std::holds_alternative<StoredImage>(savedResult));
    EXPECT_GE(fileOps->writeCalls.load(), 2U);

    fileOps->interruptNextRead.store(true);
    auto openedResult =
        storage.openForRead(std::get<StoredImage>(savedResult).relativePath());
    ASSERT_TRUE(std::holds_alternative<domain::ImageFile>(openedResult));
    auto& opened = std::get<domain::ImageFile>(openedResult);
    EXPECT_EQ(readAll(opened), jpeg);
    EXPECT_GE(fileOps->readCalls.load(), 2U);
}

TEST(PosixImageStorageTest, RenameAndFsyncFailuresLeaveNoOfficialOrTemporaryFile) {
    const auto jpeg = encode(".jpg");
    for (const std::size_t failSyncCall : {0U, 1U, 2U}) {
        TemporaryDirectory root;
        auto fileOps = std::make_shared<InjectingFileOps>();
        fileOps->failRename = failSyncCall == 0U;
        fileOps->failSyncCall = failSyncCall;
        PosixImageStorage storage(root.path(), fileOps);
        auto result = save(
            storage,
            jpeg,
            recognitionId(10U + failSyncCall),
            capturedAt(),
            ImageFormat::jpeg);
        expectFailure(result, StorageFailure::writeFailed);
        EXPECT_EQ(regularFileCount(root.path()), 0U) << failSyncCall;
    }
}

TEST(PosixImageStorageTest, TemporaryCloseFailureIsMappedAndCleaned) {
    TemporaryDirectory root;
    auto fileOps = std::make_shared<InjectingFileOps>();
    fileOps->failCloseCall = 1U;
    PosixImageStorage storage(root.path(), fileOps);
    const auto jpeg = encode(".jpg");
    auto result = save(storage, jpeg, recognitionId(13U), capturedAt(), ImageFormat::jpeg);
    expectFailure(result, StorageFailure::writeFailed);
    EXPECT_EQ(regularFileCount(root.path()), 0U);
}

TEST(PosixImageStorageTest, BestEffortRemovalIsDurableMissingSafeAndNeverThrows) {
    TemporaryDirectory root;
    const auto jpeg = encode(".jpg");
    PosixImageStorage storage(root.path());
    auto savedResult = save(storage, jpeg, recognitionId(20U), capturedAt(), ImageFormat::jpeg);
    ASSERT_TRUE(std::holds_alternative<StoredImage>(savedResult));
    const auto path = std::get<StoredImage>(savedResult).relativePath();
    EXPECT_NO_THROW(storage.removeBestEffort(path));
    EXPECT_EQ(regularFileCount(root.path()), 0U);
    EXPECT_NO_THROW(storage.removeBestEffort(path));
    auto missing = storage.openForRead(path);
    ASSERT_TRUE(std::holds_alternative<StorageFailure>(missing));
    EXPECT_EQ(std::get<StorageFailure>(missing), StorageFailure::notFound);
}

TEST(PosixImageStorageTest, CleanupFailureDoesNotThrowOrExposeAnException) {
    TemporaryDirectory root;
    const auto jpeg = encode(".jpg");
    PosixImageStorage initial(root.path());
    auto savedResult = save(initial, jpeg, recognitionId(21U), capturedAt(), ImageFormat::jpeg);
    ASSERT_TRUE(std::holds_alternative<StoredImage>(savedResult));
    const auto path = std::get<StoredImage>(savedResult).relativePath();

    auto fileOps = std::make_shared<InjectingFileOps>();
    fileOps->failUnlink = true;
    PosixImageStorage failing(root.path(), fileOps);
    EXPECT_NO_THROW(failing.removeBestEffort(path));
    EXPECT_EQ(regularFileCount(root.path()), 1U);
}

TEST(PosixImageStorageTest, RejectsSymlinkTraversalOutsideImageRoot) {
    TemporaryDirectory root;
    TemporaryDirectory outside;
    fs::create_directories(outside.path() / "08" / "14");
    const auto id = recognitionId(30U);
    const auto outsideFile = outside.path() / "08" / "14" / (id.toString() + ".jpg");
    const auto jpeg = encode(".jpg");
    {
        std::ofstream output(outsideFile, std::ios::binary);
        output.write(
            reinterpret_cast<const char*>(jpeg.data()),
            static_cast<std::streamsize>(jpeg.size()));
    }
    fs::create_directory_symlink(outside.path(), root.path() / "2026");
    const auto path = domain::RelativeImagePath::parseGenerated(
        "2026/08/14/" + id.toString() + ".jpg");

    PosixImageStorage storage(root.path());
    auto opened = storage.openForRead(path);
    ASSERT_TRUE(std::holds_alternative<StorageFailure>(opened));
    EXPECT_EQ(std::get<StorageFailure>(opened), StorageFailure::readFailed);
    EXPECT_NO_THROW(storage.removeBestEffort(path));
    EXPECT_TRUE(fs::exists(outsideFile));
}

TEST(PosixImageStorageTest, ConcurrentSavesInSharedDateDirectoryRemainIndependent) {
    constexpr std::size_t kThreadCount = 16U;
    TemporaryDirectory root;
    PosixImageStorage storage(root.path());
    const auto jpeg = encode(".jpg");
    std::atomic<std::size_t> successes{0U};
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);
    for (std::size_t index = 0U; index < kThreadCount; ++index) {
        threads.emplace_back([&, index] {
            const auto result = save(
                storage,
                jpeg,
                recognitionId(100U + index),
                capturedAt(),
                ImageFormat::jpeg);
            if (std::holds_alternative<StoredImage>(result)) {
                successes.fetch_add(1U, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(successes.load(), kThreadCount);
    EXPECT_EQ(regularFileCount(root.path()), kThreadCount);
}

}  // namespace
