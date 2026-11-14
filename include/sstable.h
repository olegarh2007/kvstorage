#ifndef KVSTORAGE_SSTABLE_H
#define KVSTORAGE_SSTABLE_H

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "BloomFilter.h"
#include "file.h"

// управляет файлами с данными .sst, отвечает за запись map в новый файл,
// слияние фалов
class SSTable {
public:
    // A value represents an existing entry; nullopt represents a deletion.
    using EntryMap = std::map<std::string, std::optional<std::string>>;

    // Optional file prefix, including its parent directory (which must exist).
    explicit SSTable(std::shared_ptr<const FileSystem> factory, std::string path = "");
    // созраняет EntryMap на диске и запускает compaction так,
    // что если flush успешно завершился,
    // то после сбоя запись останется.
    // будет записан новый MANIFEST и
    // на каждом уровне будет не более одного файла указанного в MANIFEST.
    //
    // если сбой происходит во время flush то
    // изменения либо применяются полностью либо не применяются совсем
    // могут остаться ненужные файлы
    //
    // по завершении метода все .sst файлы должны иметь соответсвующие блум-фильтры в мапе
    void flush(const EntryMap& entries);

    // возвращает значение последнюю запись которая связана с key
    // среди всех sstable-файлов
    //
    // если последняя запись tomstone или вовсе нет ни одной записи
    // среди всех файлов то возвращается std::nullopt
    //
    // гарантируется что файлы не повредятся
    std::optional<std::string> get(const std::string& key) const;
    // отдает все ключ-значение содержащиеся в базе, хранящиеся в SSTable
    std::map<std::string, std::string> dump() const;
private:
    std::shared_ptr<const FileSystem> _fileFactory;
    std::string _path;
    using Levels = std::map<std::uint64_t, std::uint64_t>;
    // Maps a level (not a file id) to its open file; copies share file ownership.
    using FilePool = std::map<std::uint64_t, std::shared_ptr<File>>;
public:
    struct PreparedFlush {
        Levels levels;
        FilePool files;
        std::uint64_t nextId;
        std::vector<std::uint64_t> created;
        std::vector<std::uint64_t> obsolete;
    };
    PreparedFlush prepareFlush(const EntryMap& entries);
    void commitFlush(PreparedFlush& prepared);
    void rollbackFlush(PreparedFlush& prepared);

private:
    std::uint64_t next_id = 0;
    Levels levels;
    // Exactly one open file per occupied MANIFEST level.
    FilePool files;
    // converts fileId to its BloomFilter
    std::unordered_map<uint64_t, BloomFilter> bloom_filters_;

    // Native byte order: uint64_t count, uint64_t offsets[count], sorted records.
    // Record: uint8_t added, uint64_t keySize, key bytes, and, if added,
    // uint64_t valueSize followed by value bytes. Offsets are absolute.
    void save(const std::filesystem::path& path, const EntryMap& entries) const;

    // MANIFEST: next_id, occupied level count, then pairs of level and file id.
    void writeManifest(std::uint64_t nextId, const Levels& levels);
    std::filesystem::path namedPath(const std::string& name) const;
    std::filesystem::path sstFilePath(std::uint64_t id) const;
    std::shared_ptr<File> openFile(std::uint64_t id);
    void merge(File& older, File& newer, std::uint64_t output);
    std::optional<std::string> getThroughId(const std::string& key, std::uint64_t id) const;
};

#endif  // KVSTORAGE_SSTABLE_H
