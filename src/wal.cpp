//
// Created by oleg on 10/3/26.
//

#include "../include/wal.h"

#include <unistd.h>

#include <cerrno>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

void checkSize(std::string_view str) {
    if (str.size() > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("WAL string is too large");
    }
}

void writeString(std::FILE* out, std::string_view str) {
    const std::uint64_t size = str.size();
    if (std::fwrite(&size, sizeof(size), 1, out) != 1 ||
        (!str.empty() && std::fwrite(str.data(), 1, str.size(), out) != str.size())) {
        throw std::runtime_error("Failed to write WAL string");
    }
}

// False means that the final record ends before this string is complete.
bool readString(std::FILE* in, std::uint64_t& remaining, std::string& str) {
    if (remaining < sizeof(std::uint64_t)) {
        return false;
    }
    std::uint64_t size = 0;
    if (std::fread(&size, sizeof(size), 1, in) != 1) {
        throw std::runtime_error("Failed to read WAL string size");
    }
    remaining -= sizeof(size);
    if (size > remaining) {
        return false;
    }
    if (size > str.max_size()) {
        throw std::runtime_error("Invalid WAL string size");
    }
    str.resize(static_cast<std::size_t>(size));
    if (!str.empty() && std::fread(str.data(), 1, str.size(), in) != str.size()) {
        throw std::runtime_error("Failed to read WAL record");
    }
    remaining -= size;
    return true;
}

}  // namespace

Wal::Wal(std::string path, std::map<std::string, std::pair<std::string, bool>>& map)
    : _path(std::move(path)) {
    File in(std::fopen(_path.c_str(), "rb+"));
    if (!in && errno != ENOENT) {
        throw std::runtime_error("Failed to open WAL for reading: " + _path);
    }
    if (in) {
        if (::fseeko(in.get(), 0, SEEK_END) != 0) {
            throw std::runtime_error("Failed to seek WAL: " + _path);
        }
        const auto fileSize = ::ftello(in.get());
        if (fileSize < 0) {
            throw std::runtime_error("Failed to determine WAL size: " + _path);
        }
        if (::fseeko(in.get(), 0, SEEK_SET) != 0) {
            throw std::runtime_error("Failed to seek WAL: " + _path);
        }
        auto remaining = static_cast<std::uint64_t>(fileSize);
        std::uint64_t completeBytes = 0;
        while (remaining != 0) {
            std::uint8_t code = 0;
            if (std::fread(&code, sizeof(code), 1, in.get()) != 1) {
                throw std::runtime_error("Failed to read WAL operation");
            }
            --remaining;
            if (code != static_cast<std::uint8_t>(Operation::Put) &&
                code != static_cast<std::uint8_t>(Operation::Delete)) {
                throw std::runtime_error("Invalid WAL operation");
            }
            std::string key;
            if (!readString(in.get(), remaining, key)) {
                break;
            }
            if (code == static_cast<std::uint8_t>(Operation::Put)) {
                std::string value;
                if (!readString(in.get(), remaining, value)) {
                    break;
                }
                map.insert_or_assign(std::move(key), std::make_pair(std::move(value), true));
            } else {
                map.insert_or_assign(std::move(key), std::make_pair(std::string{}, false));
            }
            ++_size;
            completeBytes = static_cast<std::uint64_t>(fileSize) - remaining;
        }
        if (completeBytes < static_cast<std::uint64_t>(fileSize)) {
            // Remove the incomplete tail before any new records are appended.
            if (::ftruncate(::fileno(in.get()), static_cast<off_t>(completeBytes)) != 0) {
                throw std::runtime_error("Failed to truncate WAL: " + _path);
            }
        }
    }

    _out = File(std::fopen(_path.c_str(), "ab"));
    if (!_out) {
        throw std::runtime_error("Failed to open WAL for writing: " + _path);
    }
}

void Wal::clear() {
    if (_out.close() != 0) {
        throw std::runtime_error("Failed to close WAL: " + _path);
    }
    _out = File(std::fopen(_path.c_str(), "wb"));
    if (!_out) {
        throw std::runtime_error("Failed to clear WAL: " + _path);
    }
    _size = 0;
}

void Wal::put(std::string_view key, std::string_view value) {
    write(Operation::Put, key, value);
}

void Wal::remove(std::string_view key) {
    write(Operation::Delete, key, {});
}

std::size_t Wal::size() const noexcept {
    return _size;
}

void Wal::write(Operation operation, std::string_view key, std::string_view value) {
    checkSize(key);
    if (operation == Operation::Put) {
        checkSize(value);
    }

    const auto code = static_cast<std::uint8_t>(operation);
    if (!_out || std::fwrite(&code, sizeof(code), 1, _out.get()) != 1) {
        throw std::runtime_error("Failed to write WAL operation");
    }
    writeString(_out.get(), key);
    if (operation == Operation::Put) {
        writeString(_out.get(), value);
    }

    if (std::fflush(_out.get()) != 0) {
        throw std::runtime_error("Failed to flush WAL: " + _path);
    }
    ++_size;

    _out.fsync();
}
