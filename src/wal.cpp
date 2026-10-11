//
// Created by oleg on 10/3/26.
//

#include "../include/wal.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

void checkSize(std::string_view str) {
    if (str.size() > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("WAL string is too large");
    }
}

void writeString(std::ostream& out, std::string_view str) {
    const std::uint64_t size = str.size();
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    if (!str.empty()) {
        out.write(str.data(), static_cast<std::streamsize>(size));
    }
    if (!out) {
        throw std::runtime_error("Failed to write WAL string");
    }
}

// False means that the final record ends before this string is complete.
bool readString(std::istream& in, std::uint64_t& remaining, std::string& str) {
    if (remaining < sizeof(std::uint64_t)) {
        return false;
    }
    std::uint64_t size = 0;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    if (!in) {
        throw std::runtime_error("Failed to read WAL string size");
    }
    remaining -= sizeof(size);
    if (size > remaining) {
        return false;
    }
    if (size > std::string().max_size() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("Invalid WAL string size");
    }
    str.resize(static_cast<std::size_t>(size));
    in.read(str.data(), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error("Failed to read WAL record");
    }
    remaining -= size;
    return true;
}

}  // namespace

Wal::Wal(std::string path, std::map<std::string, std::pair<std::string, bool>>& map)
    : _path(std::move(path)) {
    if (std::filesystem::exists(_path)) {
        std::ifstream in(_path, std::ios::binary | std::ios::ate);
        if (!in) {
            throw std::runtime_error("Failed to open WAL for reading: " + _path);
        }
        const auto fileSize = in.tellg();
        if (fileSize < 0) {
            throw std::runtime_error("Failed to determine WAL size: " + _path);
        }
        in.seekg(0);
        if (!in) {
            throw std::runtime_error("Failed to seek WAL: " + _path);
        }
        auto remaining = static_cast<std::uint64_t>(fileSize);
        std::uint64_t completeBytes = 0;
        while (remaining != 0) {
            std::uint8_t code = 0;
            in.read(reinterpret_cast<char*>(&code), sizeof(code));
            --remaining;
            if (!in) {
                throw std::runtime_error("Failed to read WAL operation");
            }
            if (code != static_cast<std::uint8_t>(Operation::Put) &&
                code != static_cast<std::uint8_t>(Operation::Delete)) {
                throw std::runtime_error("Invalid WAL operation");
            }
            std::string key;
            if (!readString(in, remaining, key)) {
                break;
            }
            if (code == static_cast<std::uint8_t>(Operation::Put)) {
                std::string value;
                if (!readString(in, remaining, value)) {
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
            in.close();
            std::filesystem::resize_file(_path, completeBytes);
        }
    }

    _out.open(_path, std::ios::binary | std::ios::app);
    if (!_out) {
        throw std::runtime_error("Failed to open WAL for writing: " + _path);
    }
}

void Wal::clear() {
    _out.close();
    if (!_out) {
        throw std::runtime_error("Failed to close WAL: " + _path);
    }
    _out.open(_path, std::ios::binary | std::ios::trunc);
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
    _out.write(reinterpret_cast<const char*>(&code), sizeof(code));
    if (!_out) {
        throw std::runtime_error("Failed to write WAL operation");
    }
    writeString(_out, key);
    if (operation == Operation::Put) {
        writeString(_out, value);
    }

    _out.flush();
    if (!_out) {
        throw std::runtime_error("Failed to flush WAL: " + _path);
    }
    ++_size;
}
