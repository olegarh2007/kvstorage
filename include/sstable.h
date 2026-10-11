#ifndef KVSTORAGE_SSTABLE_H
#define KVSTORAGE_SSTABLE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

class SSTable {
public:
    // The pair holds the value and a flag: true = added, false = deleted.
    using Entries = std::map<std::string, std::pair<std::string, bool>>;

    SSTable() {
        if (std::filesystem::exists("MANIFEST")) {
            std::ifstream manifest;
            manifest.exceptions(std::ios::failbit | std::ios::badbit);
            manifest.open("MANIFEST");
            manifest >> files_count;
            if (files_count < 0) {
                throw std::runtime_error("Invalid file count in MANIFEST");
            }
        } else {
            writeManifest(0);
        }
    }

    void flush(const Entries& entries) {
        save("ss" + std::to_string(files_count), entries);
        writeManifest(files_count + 1);
        ++files_count;
    }

    // Format (integers use native byte order):
    // uint64_t count, uint64_t offsets[count], then sorted records.
    // Each offset is absolute and points to a record's presence flag.
    // Record: uint8_t added, uint64_t keySize, key bytes.
    // If added == 1, append uint64_t valueSize and value bytes.
    void save(const std::filesystem::path& path, const Entries& entries) const {
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(path, std::ios::binary | std::ios::trunc);

        const auto count = static_cast<std::uint64_t>(entries.size());
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));

        std::uint64_t offset = sizeof(count) + count * sizeof(std::uint64_t);
        for (const auto& [key, value] : entries) {
            file.write(reinterpret_cast<const char*>(&offset), sizeof(offset));
            offset += sizeof(std::uint8_t) + sizeof(std::uint64_t) + key.size();
            if (value.second) {
                offset += sizeof(std::uint64_t) + value.first.size();
            }
        }

        for (const auto& [key, value] : entries) {
            const std::uint8_t flag = value.second ? 1 : 0;
            const auto length = static_cast<std::uint64_t>(key.size());
            file.write(reinterpret_cast<const char*>(&flag), sizeof(flag));
            file.write(reinterpret_cast<const char*>(&length), sizeof(length));
            file.write(key.data(), static_cast<std::streamsize>(key.size()));
            if (value.second) {
                const auto valueLength = static_cast<std::uint64_t>(value.first.size());
                file.write(reinterpret_cast<const char*>(&valueLength), sizeof(valueLength));
                file.write(value.first.data(), static_cast<std::streamsize>(value.first.size()));
            }
        }
        file.close();
    }

    std::optional<std::string> get(const std::string& key) const {
        return getFromFile(key, files_count - 1);
    }

    // Search ss<i>, then older files down to ss0 if the key is absent.
    std::optional<std::string> getFromFile(const std::string& key, int i) const {
        if (i < 0) {
            return std::nullopt;
        }

        const auto path = std::filesystem::path("ss" + std::to_string(i));
        if (!std::filesystem::exists(path)) {
            return i == 0 ? std::nullopt : getFromFile(key, i - 1);
        }

        std::ifstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(path, std::ios::binary);

        auto readString = [&file]() {
            std::uint64_t size;
            file.read(reinterpret_cast<char*>(&size), sizeof(size));
            std::string result(static_cast<std::size_t>(size), '\0');
            file.read(result.data(), static_cast<std::streamsize>(size));
            return result;
        };

        std::uint64_t count;
        file.read(reinterpret_cast<char*>(&count), sizeof(count));
        std::uint64_t left = 0;
        std::uint64_t right = count;
        while (left < right) {
            const auto middle = left + (right - left) / 2;
            file.seekg(static_cast<std::streamoff>(sizeof(count) + middle * sizeof(std::uint64_t)));
            std::uint64_t offset;
            file.read(reinterpret_cast<char*>(&offset), sizeof(offset));
            file.seekg(static_cast<std::streamoff>(offset));

            std::uint8_t flag;
            file.read(reinterpret_cast<char*>(&flag), sizeof(flag));
            const auto storedKey = readString();
            if (storedKey == key) {
                if (flag == 1) {
                    return readString();
                }
                return std::nullopt;
            }
            if (storedKey < key) {
                left = middle + 1;
            } else {
                right = middle;
            }
        }

        file.close();
        return i == 0 ? std::nullopt : getFromFile(key, i - 1);
    }

private:
    int files_count = 0;

    static void writeManifest(int count) {
        std::ofstream manifest;
        manifest.exceptions(std::ios::failbit | std::ios::badbit);
        manifest.open("MANIFEST", std::ios::trunc);
        manifest << count << '\n';
        manifest.close();
    }

};

#endif  // KVSTORAGE_SSTABLE_H
