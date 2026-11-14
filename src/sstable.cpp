#include "sstable.h"

#include <algorithm>
#include <sstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

template <typename T>
T read(File& file) {
    T value;
    file.read(&value, sizeof(value));
    return value;
}

void readAt(File& file, void* data, std::size_t size, off_t& offset) {
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
        const auto count = file.pread(bytes, size, offset);
        if (count <= 0) {
            throw std::ios_base::failure("Failed to read SSTable file");
        }
        bytes += count;
        size -= static_cast<std::size_t>(count);
        offset += count;
    }
}

template <typename T>
T readAt(File& file, off_t& offset) {
    T value;
    readAt(file, &value, sizeof(value), offset);
    return value;
}

std::string readStringAt(File& file, off_t& offset) {
    const auto size = readAt<std::uint64_t>(file, offset);
    if (size > std::string().max_size()) {
        throw std::runtime_error("Invalid SSTable string size");
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    readAt(file, value.data(), value.size(), offset);
    return value;
}

template <typename T>
void write(File& file, T value) {
    file.write(&value, sizeof(value));
}

std::string readString(File& file) {
    const auto size = read<std::uint64_t>(file);
    if (size > std::string().max_size()) {
        throw std::runtime_error("Invalid SSTable string size");
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    file.read(value.data(), value.size());
    return value;
}

void writeString(File& file, const std::string& value) {
    write(file, static_cast<std::uint64_t>(value.size()));
    file.write(value.data(), value.size());
}

struct Record {
    std::string key;
    std::optional<std::string> value;
};

Record readRecord(File& file) {
    const auto flag = read<std::uint8_t>(file);
    if (flag > 1) {
        throw std::runtime_error("Invalid SSTable presence flag");
    }
    Record record{readString(file), std::nullopt};
    if (flag == 1) {
        record.value = readString(file);
    }
    return record;
}

void writeRecord(File& file, const Record& record) {
    write(file, static_cast<std::uint8_t>(record.value.has_value()));
    writeString(file, record.key);
    if (record.value) {
        writeString(file, *record.value);
    }
}

class Reader {
public:
    explicit Reader(File& input) : file(input) {
        file.seek(0);
        remaining = read<std::uint64_t>(file);
        file.seek(static_cast<off_t>(sizeof(std::uint64_t) * (remaining + 1)));
        advance();
    }

    void advance() {
        if (remaining == 0) {
            current.reset();
        } else {
            current = readRecord(file);
            --remaining;
        }
    }

    std::optional<Record> current;

private:
    File& file;
    std::uint64_t remaining;
};

template <typename Emit>
void mergeRecords(File& left, File& right, Emit emit) {
    Reader older(left);
    Reader newer(right);
    while (older.current || newer.current) {
        if (!newer.current || (older.current && older.current->key < newer.current->key)) {
            emit(*older.current);
            older.advance();
        } else {
            if (older.current && older.current->key == newer.current->key) {
                older.advance();
            }
            emit(*newer.current);
            newer.advance();
        }
    }
}

}  // namespace

SSTable::SSTable(std::shared_ptr<const FileSystem> factory, std::string path)
    : _fileFactory(std::move(factory)), _path(std::move(path)) {
    if (!_fileFactory->exists(namedPath("MANIFEST").string())) {
        writeManifest(next_id, levels);
        return;
    }
    auto manifest = _fileFactory->open(namedPath("MANIFEST").string(), "r");
    std::string content(static_cast<std::size_t>(manifest->size()), '\0');
    manifest->read(content.data(), content.size());
    std::istringstream input(content);
    std::uint64_t count;
    if (!(input >> next_id >> count) || count > 64) {
        throw std::runtime_error("Invalid MANIFEST header");
    }
    std::vector<std::uint64_t> ids;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t level, id;
        if (!(input >> level >> id) || level >= 64 ||
            id >= next_id || !levels.emplace(level, id).second ||
            std::find(ids.begin(), ids.end(), id) != ids.end()) {
            throw std::runtime_error("Invalid level in MANIFEST");
        }
        if (!_fileFactory->isRegularFile(sstFilePath(id).string())) {
            throw std::runtime_error("Missing SSTable referenced by MANIFEST");
        }
        ids.push_back(id);
    }
    input >> std::ws;
    if (!input.eof()) {
        throw std::runtime_error("Unexpected data in MANIFEST");
    }
    for (const auto& [level, id] : levels) {
        auto file = openFile(id);
        file->seek(0);
        const auto count = read<std::uint64_t>(*file);
        BloomFilter filter(std::max<std::uint64_t>(1, count), 0.01);
        Reader reader(*file);
        while (reader.current) {
            filter.add(reader.current->key);
            reader.advance();
        }
        bloom_filters_.emplace(id, std::move(filter));
        files.emplace(level, std::move(file));
    }
}

std::filesystem::path SSTable::namedPath(const std::string& name) const {
    return _path.empty() ? name : _path + "." + name;
}

std::filesystem::path SSTable::sstFilePath(std::uint64_t id) const {
    return namedPath(std::to_string(id) + ".sst");
}

std::shared_ptr<File> SSTable::openFile(std::uint64_t id) {
    return std::shared_ptr<File>(_fileFactory->open(sstFilePath(id).string(), "rb"));
}

void SSTable::writeManifest(std::uint64_t nextId, const Levels& newLevels) {
    auto manifest = _fileFactory->open(namedPath("MANIFEST.tmp").string(), "w");
    std::string content = std::to_string(nextId) + "\n" + std::to_string(newLevels.size()) + "\n";
    for (const auto& [level, id] : newLevels) {
        content += std::to_string(level) + " " + std::to_string(id) + "\n";
    }
    manifest->write(content.data(), content.size());
    if (manifest->fsync() != 0) {
        throw std::ios_base::failure("Failed to sync MANIFEST");
    }
    if (manifest->close() != 0) {
        throw std::ios_base::failure("Failed to close MANIFEST");
    }
    _fileFactory->rename(namedPath("MANIFEST.tmp").string(), namedPath("MANIFEST").string());
}

static void fill_bloom_filter(BloomFilter &filter, const SSTable::EntryMap& entries) {
    for (const auto& [key, value] : entries) {
        filter.add(key);
    }
}

void SSTable::flush(const EntryMap& entries) {
    auto newLevels = levels;
    // Share unchanged files until MANIFEST commits; rollback keeps the old pool.
    auto newFiles = files;
    auto newNextId = next_id;
    std::vector<std::uint64_t> created;
    std::vector<std::uint64_t> obsolete;
    auto allocate = [&]() {
        if (newNextId == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("SSTable file ids exhausted");
        }
        return newNextId++;
    };
    try {
        auto current = allocate();
        save(sstFilePath(current), entries);
        created.push_back(current);

        auto [it, _] = bloom_filters_.try_emplace(current, std::max<std::size_t>(1, entries.size()), 0.01);
        fill_bloom_filter(it->second, entries);

        auto currentFile = openFile(current);
        std::uint64_t level = 0;
        while (newLevels.contains(level)) {
            const auto previous = newLevels.at(level);
            const auto output = allocate();
            merge(*newFiles.at(level), *currentFile, output);
            created.push_back(output);
            auto outputFile = openFile(output);
            obsolete.push_back(previous);
            obsolete.push_back(current);
            newLevels.erase(level);
            newFiles.erase(level);
            current = output;
            currentFile = std::move(outputFile);
            ++level;
        }
        newLevels.emplace(level, current);
        newFiles.emplace(level, std::move(currentFile));
        // Include intermediate merge files, which are no longer in the pool.
        for (const auto id : created) {
            auto file = _fileFactory->open(sstFilePath(id).string(), "rb+");
            if (file->fsync() != 0) {
                throw std::ios_base::failure("Failed to sync SSTable file");
            }
        }
        writeManifest(newNextId, newLevels);
    } catch (...) {
        newFiles.clear();
        for (const auto id : created) {
            bloom_filters_.erase(id);
            _fileFactory->remove(sstFilePath(id).string());
        }
        throw;
    }
    levels.swap(newLevels);
    files.swap(newFiles);
    next_id = newNextId;
    // Close obsolete handles before deleting their files.
    newFiles.clear();
    for (const auto id : obsolete) {
        bloom_filters_.erase(id);
        // Once committed, cleanup errors must not make DB retain a flushed WAL.
        _fileFactory->remove(sstFilePath(id).string());
    }
}

void SSTable::save(const std::filesystem::path& path, const EntryMap& entries) const {
    auto outputFile = _fileFactory->open(path.string(), "wb");
    auto& file = *outputFile;
    const auto count = static_cast<std::uint64_t>(entries.size());
    write(file, count);
    std::uint64_t offset = sizeof(count) + count * sizeof(std::uint64_t);
    for (const auto& [key, value] : entries) {
        write(file, offset);
        offset += sizeof(std::uint8_t) + sizeof(std::uint64_t) + key.size();
        if (value) {
            offset += sizeof(std::uint64_t) + value->size();
        }
    }
    for (const auto& [key, value] : entries) {
        writeRecord(file, {key, value});
    }
    if (file.close() != 0) {
        throw std::ios_base::failure("Failed to close SSTable file");
    }
}

void SSTable::merge(File& older, File& newer, std::uint64_t output) {
    // Count records first to reserve space for the index before the data.
    std::uint64_t count = 0;
    mergeRecords(older, newer, [&](const Record&) { ++count; });
    auto outputFile = _fileFactory->open(sstFilePath(output).string(), "wb");
    auto& file = *outputFile;
    write(file, count);
    const auto offset = sizeof(count) + count * sizeof(std::uint64_t);
    std::vector<std::uint64_t> offsets;
    BloomFilter filter(std::max<std::uint64_t>(1, count), 0.01);
    offsets.reserve(count);
    file.seek(static_cast<off_t>(offset));
    mergeRecords(older, newer, [&](const Record& record) {
        filter.add(record.key);
        offsets.push_back(static_cast<std::uint64_t>(file.tell()));
        writeRecord(file, record);
    });
    file.seek(static_cast<off_t>(sizeof(count)));
    for (const auto recordOffset : offsets) {
        write(file, recordOffset);
    }
    if (file.close() != 0) {
        throw std::ios_base::failure("Failed to close SSTable file");
    }
    bloom_filters_.emplace(output, std::move(filter));
}

std::optional<std::string> SSTable::get(const std::string& key) const {
    return getThroughId(key, std::numeric_limits<std::uint64_t>::max());
}

std::map<std::string, std::string> SSTable::dump() const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ids;
    for (const auto& [level, id] : levels) {
        ids.emplace_back(id, level);
    }
    std::sort(ids.begin(), ids.end());
    std::map<std::string, std::string> result;
    for (const auto& [id, level] : ids) {
        Reader reader(*files.at(level));
        while (reader.current) {
            const auto& record = *reader.current;
            if (record.value) {
                result.insert_or_assign(record.key, *record.value);
            } else {
                result.erase(record.key);
            }
            reader.advance();
        }
    }
    return result;
}

std::optional<std::string> SSTable::getThroughId(const std::string& key, std::uint64_t id) const {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ids;
    for (const auto& [level, fileId] : levels) {
        if (fileId <= id) {
            ids.emplace_back(fileId, level);
        }
    }
    std::sort(ids.rbegin(), ids.rend());
    for (const auto& [fileId, level] : ids) {
        // для каждого активного SSTable существует соответсвующий BloomFilter
        const BloomFilter &bloom_filter = bloom_filters_.at(fileId);
        if (!bloom_filter.possiblyContains(key)) {
            continue;
        }

        auto& file = *files.at(level);
        off_t position = 0;
        const auto count = readAt<std::uint64_t>(file, position);
        std::uint64_t left = 0, right = count;
        while (left < right) {
            const auto middle = left + (right - left) / 2;
            position = static_cast<off_t>(sizeof(count) + middle * sizeof(std::uint64_t));
            const auto offset = readAt<std::uint64_t>(file, position);
            position = static_cast<off_t>(offset);
            const auto flag = readAt<std::uint8_t>(file, position);
            if (flag > 1) {
                throw std::runtime_error("Invalid SSTable presence flag");
            }
            const auto storedKey = readStringAt(file, position);
            if (storedKey == key) {
                return flag == 1 ? std::optional<std::string>(readStringAt(file, position)) : std::nullopt;
            }
            if (storedKey < key) {
                left = middle + 1;
            } else {
                right = middle;
            }
        }
    }
    return std::nullopt;
}
