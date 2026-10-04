//
// Created by oleg on 10/3/26.
//

#include "../include/snapshot.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

void writeString(std::ostream& out, const std::string& str) {
    if (str.size() > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("Snapshot string is too large");
    }
    const std::uint64_t size = str.size();
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(str.data(), static_cast<std::streamsize>(size));
    if (!out) {
        throw std::runtime_error("Failed to write snapshot record");
    }
}

std::string readString(std::istream& in, std::uint64_t& remaining) {
    if (remaining < sizeof(std::uint64_t)) {
        throw std::runtime_error("Truncated snapshot string size");
    }
    std::uint64_t size = 0;
    in.read(reinterpret_cast<char*>(&size), sizeof(size));
    remaining -= sizeof(size);
    if (!in || size > remaining || size > std::string().max_size() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("Invalid snapshot string size");
    }
    std::string str(static_cast<std::size_t>(size), '\0');
    in.read(str.data(), static_cast<std::streamsize>(size));
    if (!in) {
        throw std::runtime_error("Failed to read snapshot record");
    }
    remaining -= size;
    return str;
}

}  // namespace

void Snapshot::save(const std::map<std::string, std::string>& map) const {
    std::ofstream out(_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Failed to open snapshot for writing: " + _path);
    }
    for (const auto& [key, value] : map) {
        writeString(out, key);
        writeString(out, value);
    }
    out.close();
    if (!out) {
        throw std::runtime_error("Failed to close snapshot: " + _path);
    }
}

Snapshot::Snapshot(std::string path, std::map<std::string, std::string>& map)
    : _path(std::move(path)) {
    if (!std::filesystem::exists(_path)) {
        map.clear();
        return;
    }
    std::ifstream in(_path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("Failed to open snapshot for reading: " + _path);
    }
    const auto fileSize = in.tellg();
    if (fileSize < 0) {
        throw std::runtime_error("Failed to determine snapshot size: " + _path);
    }
    in.seekg(0);
    if (!in) {
        throw std::runtime_error("Failed to seek snapshot: " + _path);
    }
    auto remaining = static_cast<std::uint64_t>(fileSize);
    std::map<std::string, std::string> restored;
    while (remaining != 0) {
        auto key = readString(in, remaining);
        auto value = readString(in, remaining);
        if (!restored.emplace(std::move(key), std::move(value)).second) {
            throw std::runtime_error("Duplicate key in snapshot");
        }
    }
    map = std::move(restored);
}
