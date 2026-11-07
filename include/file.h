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
#include <vector>

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

// File operations independent of the underlying filesystem.
class FileSystem {
public:
    virtual ~FileSystem() noexcept = default;

    virtual std::unique_ptr<File> open(const std::string& path, const char* mode) const = 0;
    virtual bool exists(const std::string& path) const = 0;
    virtual bool isRegularFile(const std::string& path) const = 0;
    // Returns paths of regular files directly in dir, without recursion.
    virtual std::vector<std::string> list(const std::string& dir) const = 0;
    virtual void rename(const std::string& from, const std::string& to) const = 0;
    virtual void remove(const std::string& path) const = 0;
};

class MemFS : public FileSystem {
    // Shared cache and durable data; crash after the Nth step.
    // Each written byte is one step; open, close and fsync take one step each.
    class FailingFile : public File {
    public:
        FailingFile(const char* path, const char* mode, const MemFS& fs)
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
            fs.onSteps(1);
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
            const auto written = fs.remaining_steps == 0 ? size : std::min(size, fs.remaining_steps);
            if (written != 0) {
                content.resize(std::max(content.size(), _position + written), '\0');
                std::memcpy(content.data() + _position, data, written);
            }
            _position += written;
            fs.onSteps(written);
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
            if (!_open) {
                return 0;
            }
            _open = false;
            try {
                fs.onSteps(1);
                return 0;
            } catch (...) {
                errno = EIO;
                return -1;
            }
        }

    private:
        void checkOpen() const {
            if (!_open) {
                throw std::ios_base::failure("In-memory file is closed");
            }
        }

        const MemFS& fs;
        std::string _path;
        std::size_t _position = 0;
        bool _open = true;
        bool _readable = false;
        bool _writable = false;
        bool _append = false;
    };

public:
    MemFS() = default;

    // Reset the crash countdown; zero disables failure injection.
    void setSteps(std::size_t steps) noexcept { remaining_steps = steps; }

    // Zero disables failure injection.
    mutable std::size_t remaining_steps = 0;
    mutable std::map<std::string, std::string> in_disk;
    mutable std::map<std::string, std::string> in_cache;

    std::string& get(const std::string& filename) const { return in_cache[filename]; }

    void fsync(const std::string& filename) const {
        in_disk[filename] = in_cache[filename];
        onSteps(1);
    }

    void crash() const { in_cache = in_disk; }

    void onSteps(std::size_t steps) const {
        if (remaining_steps == 0 || steps == 0) {
            return;
        }
        if (steps >= remaining_steps) {
            remaining_steps = 0;
            crash();
            throw std::ios_base::failure("Injected filesystem crash");
        }
        remaining_steps -= steps;
    }

    // This filesystem must outlive the files it creates.
    std::unique_ptr<File> open(const std::string& path, const char* mode) const override {
        return std::make_unique<FailingFile>(path.c_str(), mode, *this);
    }

    bool exists(const std::string& path) const override {
        return in_cache.contains(path);
    }

    bool isRegularFile(const std::string& path) const override {
        return exists(path);
    }

    std::vector<std::string> list(const std::string& dir) const override {
        auto directory = std::filesystem::path(dir).lexically_normal();
        if (directory.empty()) {
            directory = ".";
        }
        if (directory != directory.root_path() && directory.filename().empty()) {
            directory = directory.parent_path();
        }
        std::vector<std::string> files;
        for (const auto& [path, content] : in_cache) {
            auto parent = std::filesystem::path(path).lexically_normal().parent_path();
            if (parent.empty()) {
                parent = ".";
            }
            if (parent == directory) {
                files.push_back(path);
            }
        }
        return files;
    }

    void rename(const std::string& from, const std::string& to) const override {
        if (from == to) {
            return;
        }
        in_cache[to] = std::move(in_cache.at(from));
        in_cache.erase(from);
        if (in_disk.contains(from)) {
            in_disk[to] = std::move(in_disk.at(from));
            in_disk.erase(from);
        } else {
            in_disk.erase(to);
        }
    }

    void remove(const std::string& path) const override {
        in_cache.erase(path);
        in_disk.erase(path);
    }
};

class RegularFS : public FileSystem {
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

public:
    std::unique_ptr<File> open(const std::string& path, const char* mode) const override {
        return std::make_unique<RegularFile>(path.c_str(), mode);
    }

    bool exists(const std::string& path) const override {
        return std::filesystem::exists(path);
    }

    bool isRegularFile(const std::string& path) const override {
        return std::filesystem::is_regular_file(path);
    }

    std::vector<std::string> list(const std::string& dir) const override {
        std::vector<std::string> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (entry.is_regular_file()) {
                files.push_back(entry.path().string());
            }
        }
        return files;
    }

    void rename(const std::string& from, const std::string& to) const override {
        std::filesystem::rename(from, to);
    }

    void remove(const std::string& path) const override {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

#endif  // KVSTORAGE_FILE_H
