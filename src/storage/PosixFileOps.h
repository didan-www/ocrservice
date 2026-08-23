#pragma once

#include <cstddef>
#include <memory>

#include <sys/stat.h>
#include <sys/types.h>

namespace ocrservice::storage::detail {

class PosixFileOps {
public:
    virtual ~PosixFileOps() = default;

    virtual int open(const char* path, int flags, mode_t mode) noexcept = 0;
    virtual int openAt(int directoryFd, const char* path, int flags, mode_t mode) noexcept = 0;
    virtual int makeDirectoryAt(int directoryFd, const char* path, mode_t mode) noexcept = 0;
    virtual ssize_t write(int fd, const void* data, std::size_t size) noexcept = 0;
    virtual ssize_t read(int fd, void* data, std::size_t size) noexcept = 0;
    virtual int sync(int fd) noexcept = 0;
    virtual int close(int fd) noexcept = 0;
    virtual int renameNoReplaceAt(
        int oldDirectoryFd,
        const char* oldPath,
        int newDirectoryFd,
        const char* newPath) noexcept = 0;
    virtual int unlinkAt(int directoryFd, const char* path, int flags) noexcept = 0;
    virtual int fileStatus(int fd, struct stat* status) noexcept = 0;
};

std::shared_ptr<PosixFileOps> makeSystemPosixFileOps();

}  // namespace ocrservice::storage::detail
