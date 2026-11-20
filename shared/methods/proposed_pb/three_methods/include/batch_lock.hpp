#pragma once

#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace metro {

// The OS releases this lock if the process exits or is interrupted.
class BatchLock {
public:
    explicit BatchLock(const std::filesystem::path& file) {
#ifdef _WIN32
        handle_ = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot lock output directory. Check access or use another --output directory.");
#else
        fd_ = open(file.c_str(), O_WRONLY | O_CREAT, 0666);
        if (fd_ < 0 || flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            if (fd_ >= 0) close(fd_);
            throw std::runtime_error("Cannot lock output directory. Check access or use another --output directory.");
        }
#endif
    }

    ~BatchLock() {
#ifdef _WIN32
        CloseHandle(handle_);
#else
        close(fd_);
#endif
    }

    BatchLock(const BatchLock&) = delete;
    BatchLock& operator=(const BatchLock&) = delete;

private:
#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};
}
