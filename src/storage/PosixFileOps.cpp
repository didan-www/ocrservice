#include "PosixFileOps.h"

#include <cstdio>

#include <fcntl.h>
#include <unistd.h>

namespace ocrservice::storage::detail {
namespace {

class SystemPosixFileOps final : public PosixFileOps {
public:
    int open(const char* path, const int flags, const mode_t mode) noexcept override {
        return ::open(path, flags, mode);
    }

    int openAt(
        const int directoryFd,
        const char* path,
        const int flags,
        const mode_t mode) noexcept override {
        return ::openat(directoryFd, path, flags, mode);
    }

    int makeDirectoryAt(
        const int directoryFd,
        const char* path,
        const mode_t mode) noexcept override {
        return ::mkdirat(directoryFd, path, mode);
    }

    ssize_t write(const int fd, const void* data, const std::size_t size) noexcept override {
        return ::write(fd, data, size);
    }

    ssize_t read(const int fd, void* data, const std::size_t size) noexcept override {
        return ::read(fd, data, size);
    }

    int sync(const int fd) noexcept override { return ::fsync(fd); }
    int close(const int fd) noexcept override { return ::close(fd); }

    int renameAt(
        const int oldDirectoryFd,
        const char* oldPath,
        const int newDirectoryFd,
        const char* newPath) noexcept override {
        return ::renameat(oldDirectoryFd, oldPath, newDirectoryFd, newPath);
    }

    int unlinkAt(const int directoryFd, const char* path, const int flags) noexcept override {
        return ::unlinkat(directoryFd, path, flags);
    }

    int fileStatus(const int fd, struct stat* const status) noexcept override {
        return ::fstat(fd, status);
    }
};

}  // namespace

std::shared_ptr<PosixFileOps> makeSystemPosixFileOps() {
    return std::make_shared<SystemPosixFileOps>();
}

}  // namespace ocrservice::storage::detail
