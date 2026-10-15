#ifndef KVSTORAGE_FILE_H
#define KVSTORAGE_FILE_H

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ios>
#include <map>
#include <memory>
#include <string>
#include <utility>

class in_memory_fs {
public:
    std::size_t remaining_writes;

    std::map<std::string, std::string> in_disk;
    std::map<std::string, std::string> in_cache;

    in_memory_fs() : remaining_writes(0) {}
    in_memory_fs(std::size_t N) : remaining_writes(N) {
    }

    // returns reference to file content by name
    std::string& get(const std::string& filename) { return in_cache[filename]; }

    void fsync(const std::string& filename) { in_disk[filename] = in_cache[filename]; }

    void crash() { in_cache = in_disk; }

    void onWrite() {
        if (remaining_writes == 0) {
            // no crash
            return;
        }
        if (--remaining_writes == 0) {
            crash();
            throw std::ios_base::failure("Injected file write failure");
        }
    }
};

// File operations independent of the underlying storage.
class File {
public:
    File() noexcept = default;
    virtual ~File() noexcept = default;

    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&&) noexcept = default;
    File& operator=(File&&) noexcept = default;

    virtual void read(void* data, std::size_t size) = 0;
    virtual void write(const void* data, std::size_t size) = 0;
    virtual void seek(off_t offset) = 0;
    virtual off_t tell() const = 0;
    virtual off_t size() = 0;
    virtual void truncate(off_t size) = 0;
    // Returns 0 on success or -1 on failure, wi;th errno set.
    virtual int flush() noexcept = 0;
    virtual int fsync() noexcept = 0;
    virtual int close() noexcept = 0;

    virtual explicit operator bool() const noexcept = 0;
};

// Owns a C file handle and performs ordinary file operations.
class RegularFile : public File {
public:
    RegularFile() noexcept = default;
    explicit RegularFile(std::FILE* file) noexcept : _file(file) {}
    RegularFile(const char* path, const char* mode) : _file(std::fopen(path, mode)) {
        if (!_file) {
            throw std::ios_base::failure("Failed to open file");
        }
    }

    ~RegularFile() noexcept override { close(); }

    RegularFile(const RegularFile&) = delete;
    RegularFile& operator=(const RegularFile&) = delete;

    RegularFile(RegularFile&& other) noexcept : _file(std::exchange(other._file, nullptr)) {}

    RegularFile& operator=(RegularFile&& other) noexcept {
        if (this != &other) {
            close();
            _file = std::exchange(other._file, nullptr);
        }
        return *this;
    }

    explicit operator bool() const noexcept override { return _file != nullptr; }

    void read(void* data, std::size_t size) override {
        if (!_file || (size != 0 && std::fread(data, 1, size, _file) != size)) {
            throw std::ios_base::failure("Failed to read file");
        }
    }

    void write(const void* data, std::size_t size) override {
        if (!_file || (size != 0 && std::fwrite(data, 1, size, _file) != size)) {
            throw std::ios_base::failure("Failed to write file");
        }
    }

    void seek(off_t offset) override {
        if (_file) {
            std::clearerr(_file);
        }
        if (!_file || ::fseeko(_file, offset, SEEK_SET) != 0) {
            throw std::ios_base::failure("Failed to seek file");
        }
    }

    off_t tell() const override {
        const auto offset = _file ? ::ftello(_file) : -1;
        if (offset < 0) {
            throw std::ios_base::failure("Failed to determine file position");
        }
        return offset;
    }

    off_t size() override {
        const auto position = tell();
        if (::fseeko(_file, 0, SEEK_END) != 0) {
            throw std::ios_base::failure("Failed to determine file size");
        }
        const auto result = tell();
        seek(position);
        return result;
    }

    void truncate(off_t size) override {
        if (flush() != 0 || ::ftruncate(::fileno(_file), size) != 0) {
            throw std::ios_base::failure("Failed to truncate file");
        }
    }

    int flush() noexcept override {
        if (!_file) {
            errno = EBADF;
            return -1;
        }
        return std::fflush(_file);
    }

    // Flush the stdio buffer, then synchronize the file with storage.
    // Returns 0 on success or -1 on failure, with errno set.
    int fsync() noexcept override {
        if (flush() != 0) {
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
    int close() noexcept override {
        auto* file = std::exchange(_file, nullptr);
        return file ? std::fclose(file) : 0;
    }

private:
    std::FILE* _file = nullptr;
};

// Shared cache and durable data; the Nth write restores the cache and throws.
class FailingFile : public File {
public:
    FailingFile(const char* path, const char* mode, in_memory_fs& fs)
        : fs(fs), _path(path ? path : "") {
        if (!path || !mode || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) {
            throw std::ios_base::failure("Invalid file path or mode");
        }
        const bool update = std::strchr(mode, '+') != nullptr;
        _readable = mode[0] == 'r' || update;
        _writable = mode[0] != 'r' || update;
        _append = mode[0] == 'a';
        auto& content = fs.get(_path);
        if (mode[0] == 'w') {
            content.clear();
        }
        _position = _append ? content.size() : 0;
    }

    explicit operator bool() const noexcept override { return _open; }

    void read(void* data, std::size_t size) override {
        checkOpen();
        auto& content = fs.get(_path);
        if (!_readable || _position > content.size() || size > content.size() - _position) {
            throw std::ios_base::failure("Failed to read in-memory file");
        }
        if (size != 0) {
            std::memcpy(data, content.data() + _position, size);
        }
        _position += size;
    }

    void write(const void* data, std::size_t size) override {
        checkOpen();
        if (!_writable) {
            throw std::ios_base::failure("Failed to write in-memory file");
        }
        auto& content = fs.get(_path);
        if (_append) {
            _position = content.size();
        }
        if (_position > content.max_size() || size > content.max_size() - _position) {
            throw std::ios_base::failure("In-memory file is too large");
        }
        if (size != 0) {
            content.resize(std::max(content.size(), _position + size), '\0');
            std::memcpy(content.data() + _position, data, size);
        }
        _position += size;
        fs.onWrite();
    }

    void seek(off_t offset) override {
        checkOpen();
        if (offset < 0) {
            throw std::ios_base::failure("Invalid in-memory file position");
        }
        _position = static_cast<std::size_t>(offset);
    }

    off_t tell() const override {
        checkOpen();
        return static_cast<off_t>(_position);
    }

    off_t size() override {
        checkOpen();
        return static_cast<off_t>(fs.get(_path).size());
    }

    void truncate(off_t size) override {
        checkOpen();
        if (!_writable || size < 0) {
            throw std::ios_base::failure("Failed to truncate in-memory file");
        }
        fs.get(_path).resize(static_cast<std::size_t>(size), '\0');
    }

    int flush() noexcept override {
        if (!_open) {
            errno = EBADF;
            return -1;
        }
        return 0;
    }

    int fsync() noexcept override {
        if (!_open) {
            errno = EBADF;
            return -1;
        }
        try {
            fs.fsync(_path);
            return 0;
        } catch (...) {
            errno = EIO;
            return -1;
        }
    }

    int close() noexcept override {
        _open = false;
        return 0;
    }

private:
    void checkOpen() const {
        if (!_open) {
            throw std::ios_base::failure("In-memory file is closed");
        }
    }

    in_memory_fs& fs;
    std::string _path;
    std::size_t _position = 0;
    bool _open = true;
    bool _readable = false;
    bool _writable = false;
    bool _append = false;
};

class FileFactory {
public:
    FileFactory() = default;
    // The filesystem must outlive this factory and the files it creates.
    explicit FileFactory(in_memory_fs& fs) : _fs(&fs) {}

    std::unique_ptr<File> create(const std::string& path, const char* mode) const {
        if (_fs != nullptr) {
            return std::make_unique<FailingFile>(path.c_str(), mode, *_fs);
        }
        return std::make_unique<RegularFile>(path.c_str(), mode);
    }

    bool exists(const std::string& path) const {
        return _fs != nullptr ? _fs->in_cache.contains(path) : std::filesystem::exists(path);
    }

    bool isRegularFile(const std::string& path) const {
        return _fs != nullptr ? exists(path) : std::filesystem::is_regular_file(path);
    }

    void rename(const std::string& from, const std::string& to) const {
        if (_fs == nullptr) {
            std::filesystem::rename(from, to);
            return;
        }
        if (from == to) {
            return;
        }
        auto& fs = *_fs;
        fs.in_cache[to] = std::move(fs.in_cache.at(from));
        fs.in_cache.erase(from);
        if (fs.in_disk.contains(from)) {
            fs.in_disk[to] = std::move(fs.in_disk.at(from));
            fs.in_disk.erase(from);
        } else {
            fs.in_disk.erase(to);
        }
    }

    void remove(const std::string& path) const {
        if (_fs != nullptr) {
            _fs->in_cache.erase(path);
            _fs->in_disk.erase(path);
        } else {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    }

private:
    in_memory_fs* _fs = nullptr;
};

#endif  // KVSTORAGE_FILE_H
