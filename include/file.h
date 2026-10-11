#ifndef KVSTORAGE_FILE_H
#define KVSTORAGE_FILE_H

#include <cerrno>
#include <cstdio>
#include <utility>

#include <unistd.h>

// Owns a C file handle. get() borrows the handle without transferring ownership.
class File {
public:
    File() noexcept = default;
    explicit File(std::FILE* file) noexcept : _file(file) {}

    ~File() noexcept {
        close();
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    File(File&& other) noexcept : _file(std::exchange(other._file, nullptr)) {}

    File& operator=(File&& other) noexcept {
        if (this != &other) {
            close();
            _file = std::exchange(other._file, nullptr);
        }
        return *this;
    }

    std::FILE* get() const noexcept {
        return _file;
    }

    explicit operator bool() const noexcept {
        return _file != nullptr;
    }

    // Flush the stdio buffer, then synchronize the file with storage.
    // Returns 0 on success or -1 on failure, with errno set.
    int fsync() noexcept {
        if (!_file) {
            errno = EBADF;
            return -1;
        }
        if (std::fflush(_file) != 0) {
            return -1;
        }
        int result;
        do {
            result = ::fsync(::fileno(_file));
        } while (result == -1 && errno == EINTR);
        return result;
    }

    // Returns fclose's status; the handle is released even if closing fails.
    // Destruction ignores errors; call close() explicitly when they matter.
    int close() noexcept {
        auto* file = std::exchange(_file, nullptr);
        return file ? std::fclose(file) : 0;
    }

private:
    std::FILE* _file = nullptr;
};

#endif  // KVSTORAGE_FILE_H
