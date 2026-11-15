#ifndef KVSTORAGE_FILE_H
#define KVSTORAGE_FILE_H

#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <ios>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <unordered_map>
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
    // Read at offset without changing the current position; returns bytes read or -1.
    virtual ssize_t pread(void* data, std::size_t size, off_t offset) = 0;
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
    // Persist directory entries independently of file contents.
    virtual void syncDirectory(const std::string& path) const = 0;
};

class MemFS : public FileSystem {
    // Nested operations (rename -> remove, fsync -> onSteps -> crash) share this lock.
    mutable std::recursive_mutex _mutex;
    // Shared cache and durable data; crash after the Nth step.
    // Each written byte is one step; open, close, fsync and syncDirectory take one step each.
    class FailingFile : public File {
    public:
        FailingFile(const char* path, const char* mode, const MemFS& fs)
            : fs(fs) {
            const std::lock_guard lock(fs._mutex);
            if (!path || !mode || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) {
                throw std::ios_base::failure("Invalid file path or mode");
            }
            const bool update = std::strchr(mode, '+') != nullptr;
            _readable = mode[0] == 'r' || update;
            _writable = mode[0] != 'r' || update;
            _append = mode[0] == 'a';
            const std::string filePath(path);
            if (!fs.exists(filePath)) {
                if (mode[0] == 'r') {
                    throw std::ios_base::failure("In-memory file does not exist");
                }
                fs.directories_in_cache[parentPath(filePath)].push_back({filePath, std::to_string(fs.next_id++)});
            }
            _fileId = fs.fileId(filePath);
            auto& content = fs.in_cache[_fileId];
            if (mode[0] == 'w') {
                content.clear();
            }
            _position = _append ? content.size() : 0;
            fs.onSteps(1);
        }

        explicit operator bool() const noexcept override {
            const std::lock_guard lock(fs._mutex);
            return _open;
        }

        void read(void* data, std::size_t size) override {
            const std::lock_guard lock(fs._mutex);
            checkOpen();
            auto& content = fs.in_cache[_fileId];
            if (!_readable || _position > content.size() || size > content.size() - _position) {
                throw std::ios_base::failure("Failed to read in-memory file");
            }
            if (size != 0) {
                std::memcpy(data, content.data() + _position, size);
            }
            _position += size;
        }

        ssize_t pread(void* data, std::size_t size, off_t offset) override {
            const std::lock_guard lock(fs._mutex);
            if (!_open || !_readable) {
                errno = EBADF;
                return -1;
            }
            if (offset < 0 || size > static_cast<std::size_t>(SSIZE_MAX)) {
                errno = EINVAL;
                return -1;
            }
            const auto& content = fs.in_cache[_fileId];
            const auto start = static_cast<std::size_t>(offset);
            const auto count = start >= content.size() ? 0 : std::min(size, content.size() - start);
            if (count != 0) {
                std::memcpy(data, content.data() + start, count);
            }
            return static_cast<ssize_t>(count);
        }

        void write(const void* data, std::size_t size) override {
            const std::lock_guard lock(fs._mutex);
            checkOpen();
            if (!_writable) {
                throw std::ios_base::failure("Failed to write in-memory file");
            }
            auto& content = fs.in_cache[_fileId];
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
            const std::lock_guard lock(fs._mutex);
            checkOpen();
            if (offset < 0) {
                throw std::ios_base::failure("Invalid in-memory file position");
            }
            _position = static_cast<std::size_t>(offset);
        }

        off_t tell() const override {
            const std::lock_guard lock(fs._mutex);
            checkOpen();
            return static_cast<off_t>(_position);
        }

        off_t size() override {
            const std::lock_guard lock(fs._mutex);
            checkOpen();
            return static_cast<off_t>(fs.in_cache[_fileId].size());
        }

        void truncate(off_t size) override {
            const std::lock_guard lock(fs._mutex);
            checkOpen();
            if (!_writable || size < 0) {
                throw std::ios_base::failure("Failed to truncate in-memory file");
            }
            fs.in_cache[_fileId].resize(static_cast<std::size_t>(size), '\0');
        }

        int flush() noexcept override {
            const std::lock_guard lock(fs._mutex);
            if (!_open) {
                errno = EBADF;
                return -1;
            }
            return 0;
        }

        int fsync() noexcept override {
            const std::lock_guard lock(fs._mutex);
            if (!_open) {
                errno = EBADF;
                return -1;
            }
            try {
                fs.syncData(_fileId);
                return 0;
            } catch (...) {
                errno = EIO;
                return -1;
            }
        }

        int close() noexcept override {
            const std::lock_guard lock(fs._mutex);
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
        std::string _fileId;
        std::size_t _position = 0;
        bool _open = true;
        bool _readable = false;
        bool _writable = false;
        bool _append = false;
    };

public:
    MemFS() = default;

    // Reset the crash countdown; zero disables failure injection.
    void setSteps(std::size_t steps) noexcept {
        const std::lock_guard lock(_mutex);
        remaining_steps = steps;
    }

private:
    // Zero disables failure injection.
    mutable std::size_t remaining_steps = 0;
    mutable std::map<std::string, std::string> in_disk;
    mutable std::map<std::string, std::string> in_cache;

    struct DirectoryEntry {
        std::string path;
        std::string id;
    };
    mutable std::unordered_map<std::string, std::vector<DirectoryEntry>> directories_in_disk;
    mutable std::unordered_map<std::string, std::vector<DirectoryEntry>> directories_in_cache;
    mutable std::size_t next_id = 0;

public:
    static std::string directoryPath(const std::string& path) {
        auto normalized = std::filesystem::path(path).lexically_normal();
        if (normalized != normalized.root_path() && normalized.filename().empty()) {
            normalized = normalized.parent_path();
        }
        return normalized.empty() ? "." : normalized.string();
    }

    static std::string parentPath(const std::string& path) {
        return directoryPath(std::filesystem::path(path).parent_path().string());
    }

    std::string fileId(const std::string& path) const {
        const std::lock_guard lock(_mutex);
        const auto directory = directories_in_cache.find(parentPath(path));
        if (directory != directories_in_cache.end()) {
            for (const auto& entry : directory->second) {
                if (entry.path == path) return entry.id;
            }
        }
        throw std::ios_base::failure("In-memory file does not exist");
    }

    std::string get(const std::string& filename) const {
        const std::lock_guard lock(_mutex);
        return in_cache[fileId(filename)];
    }

    void fsync(const std::string& filename) const {
        const std::lock_guard lock(_mutex);
        syncData(fileId(filename));
    }

    void syncData(const std::string& filename) const {
        const std::lock_guard lock(_mutex);
        in_disk[filename] = in_cache[filename];
        onSteps(1);
    }

    void crash() const {
        const std::lock_guard lock(_mutex);
        in_cache = in_disk;
        directories_in_cache = directories_in_disk;
    }

    void syncDirectory(const std::string& path) const override {
        const std::lock_guard lock(_mutex);
        const auto directory = directoryPath(path);
        directories_in_disk[directory] = directories_in_cache[directory];
        onSteps(1);
    }

    void onSteps(std::size_t steps) const {
        const std::lock_guard lock(_mutex);
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
        const std::lock_guard lock(_mutex);
        return std::make_unique<FailingFile>(path.c_str(), mode, *this);
    }

    bool exists(const std::string& path) const override {
        const std::lock_guard lock(_mutex);
        const auto directory = directories_in_cache.find(parentPath(path));
        if (directory == directories_in_cache.end()) return false;
        return std::any_of(directory->second.begin(), directory->second.end(),
                           [&](const auto& entry) { return entry.path == path; });
    }

    bool isRegularFile(const std::string& path) const override {
        const std::lock_guard lock(_mutex);
        return exists(path);
    }

    std::vector<std::string> list(const std::string& dir) const override {
        const std::lock_guard lock(_mutex);
        const auto directory = directories_in_cache.find(directoryPath(dir));
        std::vector<std::string> files;
        if (directory != directories_in_cache.end()) {
            for (const auto& entry : directory->second) files.push_back(entry.path);
        }
        return files;
    }

    void rename(const std::string& from, const std::string& to) const override {
        const std::lock_guard lock(_mutex);
        const auto id = fileId(from);
        if (from == to) return;
        remove(to);
        remove(from);
        directories_in_cache[parentPath(to)].push_back({to, id});
    }

    void remove(const std::string& path) const override {
        const std::lock_guard lock(_mutex);
        auto directory = directories_in_cache.find(parentPath(path));
        if (directory == directories_in_cache.end()) return;
        std::erase_if(directory->second, [&](const auto& entry) { return entry.path == path; });
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

        ssize_t pread(void* data, std::size_t size, off_t offset) override {
            if (!_file) {
                errno = EBADF;
                return -1;
            }
            if (std::fflush(_file) != 0) {
                return -1;
            }
            return ::pread(::fileno(_file), data, size, offset);
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

    void syncDirectory(const std::string& path) const override {
        const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd == -1) throw std::ios_base::failure("Failed to open directory");
        int result;
        do {
            result = ::fsync(fd);
        } while (result == -1 && errno == EINTR);
        const int error = errno;
        const int closed = ::close(fd);
        if (result == -1 || closed == -1) {
            if (result == -1) errno = error;
            throw std::ios_base::failure("Failed to sync directory");
        }
    }

    void remove(const std::string& path) const override {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

#endif  // KVSTORAGE_FILE_H
