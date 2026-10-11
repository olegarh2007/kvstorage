#ifndef KVSTORAGE_SSTABLE_H
#define KVSTORAGE_SSTABLE_H

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

class SSTable {
public:
    // The pair holds the value and a flag: true = added, false = deleted.
    using Entries = std::map<std::string, std::pair<std::string, bool>>;

    SSTable();
    void flush(const Entries& entries);

    // Native byte order: uint64_t count, uint64_t offsets[count], sorted records.
    // Record: uint8_t added, uint64_t keySize, key bytes, and, if added,
    // uint64_t valueSize followed by value bytes. Offsets are absolute.
    void save(const std::filesystem::path& path, const Entries& entries) const;
    std::optional<std::string> get(const std::string& key) const;

    // Search active files with id <= i, newest first.
    std::optional<std::string> getFromFile(const std::string& key, int i) const;

private:
    using Levels = std::map<std::uint64_t, std::uint64_t>;
    using FilePool = std::map<std::uint64_t, std::shared_ptr<std::ifstream>>;
    std::uint64_t next_id = 0;
    Levels levels;
    // Exactly one open input stream per occupied MANIFEST level.
    FilePool files;

    // MANIFEST: next_id, occupied level count, then pairs of level and file id.
    static void writeManifest(std::uint64_t nextId, const Levels& levels);
    static std::filesystem::path filePath(std::uint64_t id);
    static std::shared_ptr<std::ifstream> openFile(std::uint64_t id);
    static void merge(std::ifstream& older, std::ifstream& newer, std::uint64_t output);
    std::optional<std::string> getThroughId(const std::string& key, std::uint64_t id) const;
};

#endif  // KVSTORAGE_SSTABLE_H
