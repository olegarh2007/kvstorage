//
// Created by oleg on 10/3/26.
//

#include "../include/wal.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace {

void checkSize(std::string_view str) {
    if (str.size() > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("WAL string is too large");
    }
}

void writeString(File& out, std::string_view str) {
    const std::uint64_t size = str.size();
    out.write(&size, sizeof(size));
    if (!str.empty()) {
        out.write(str.data(), str.size());
    }
}

// False means that the final record ends before this string is complete.
bool readString(File& in, std::uint64_t& remaining, std::string& str) {
    if (remaining < sizeof(std::uint64_t)) {
        return false;
    }
    std::uint64_t size = 0;
    in.read(&size, sizeof(size));
    remaining -= sizeof(size);
    if (size > remaining) {
        return false;
    }
    if (size > str.max_size()) {
        throw std::runtime_error("Invalid WAL string size");
    }
    str.resize(static_cast<std::size_t>(size));
    if (!str.empty()) {
        in.read(str.data(), str.size());
    }
    remaining -= size;
    return true;
}

}  // namespace

Wal::Wal(std::string path,
         std::map<std::string, std::pair<std::string, bool>>& map,
         FileFactory factory)
    : _path(std::move(path)), _fileFactory(std::move(factory)) {
    if (_fileFactory.exists(_path)) {
        auto in = _fileFactory.create(_path, "rb+");
        const auto fileSize = in->size();
        in->seek(0);
        auto remaining = static_cast<std::uint64_t>(fileSize);
        std::uint64_t completeBytes = 0;
        while (remaining != 0) {
            std::uint8_t code = 0;
            in->read(&code, sizeof(code));
            --remaining;
            if (code != static_cast<std::uint8_t>(Operation::Put) &&
                code != static_cast<std::uint8_t>(Operation::Delete)) {
                throw std::runtime_error("Invalid WAL operation");
            }
            std::string key;
            if (!readString(*in, remaining, key)) {
                break;
            }
            if (code == static_cast<std::uint8_t>(Operation::Put)) {
                std::string value;
                if (!readString(*in, remaining, value)) {
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
            in->truncate(static_cast<off_t>(completeBytes));
        }
    }

    _out = _fileFactory.create(_path, "ab");
}

void Wal::clear() {
    if (_out->close() != 0) {
        throw std::runtime_error("Failed to close WAL: " + _path);
    }
    _out = _fileFactory.create(_path, "wb");
    if (_out->fsync() != 0) {
        throw std::runtime_error("Failed to sync cleared WAL: " + _path);
    }
    _size = 0;
}

void Wal::append(std::string_view key, std::string_view value) {
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
    _out->write(&code, sizeof(code));
    writeString(*_out, key);
    if (operation == Operation::Put) {
        writeString(*_out, value);
    }

    if (_out->flush() != 0) {
        throw std::runtime_error("Failed to flush WAL: " + _path);
    }
    ++_size;

    _out->fsync();
}
