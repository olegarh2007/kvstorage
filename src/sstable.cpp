#include "sstable.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

template <typename T>
T read(std::istream& file) {
    T value;
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return value;
}

template <typename T>
void write(std::ostream& file, T value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

std::string readString(std::istream& file) {
    const auto size = read<std::uint64_t>(file);
    if (size > std::string().max_size() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("Invalid SSTable string size");
    }
    std::string value(static_cast<std::size_t>(size), '\0');
    file.read(value.data(), static_cast<std::streamsize>(size));
    return value;
}

void writeString(std::ostream& file, const std::string& value) {
    write(file, static_cast<std::uint64_t>(value.size()));
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
}

struct Record {
    std::string key;
    std::string value;
    bool added;
};

Record readRecord(std::istream& file) {
    const auto flag = read<std::uint8_t>(file);
    if (flag > 1) {
        throw std::runtime_error("Invalid SSTable presence flag");
    }
    Record record{readString(file), {}, flag == 1};
    if (record.added) {
        record.value = readString(file);
    }
    return record;
}

void writeRecord(std::ostream& file, const Record& record) {
    write(file, static_cast<std::uint8_t>(record.added));
    writeString(file, record.key);
    if (record.added) {
        writeString(file, record.value);
    }
}

class Reader {
public:
    explicit Reader(std::ifstream& input) : file(input) {
        file.clear();
        file.seekg(0);
        remaining = read<std::uint64_t>(file);
        file.seekg(static_cast<std::streamoff>(sizeof(std::uint64_t) * (remaining + 1)));
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
    std::ifstream& file;
    std::uint64_t remaining;
};

template <typename Emit>
void mergeRecords(std::ifstream& left,
                  std::ifstream& right,
                  Emit emit) {
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

SSTable::SSTable() {
    if (!std::filesystem::exists("MANIFEST")) {
        writeManifest(next_id, levels);
        return;
    }
    std::ifstream manifest("MANIFEST");
    std::uint64_t count;
    if (!(manifest >> next_id >> count) || count > 64) {
        throw std::runtime_error("Invalid MANIFEST header");
    }
    std::vector<std::uint64_t> ids;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t level, id;
        if (!(manifest >> level >> id) || level >= 64 || id >= next_id ||
            !levels.emplace(level, id).second ||
            std::find(ids.begin(), ids.end(), id) != ids.end()) {
            throw std::runtime_error("Invalid level in MANIFEST");
        }
        if (!std::filesystem::is_regular_file(filePath(id))) {
            throw std::runtime_error("Missing SSTable referenced by MANIFEST");
        }
        ids.push_back(id);
    }
    manifest >> std::ws;
    if (!manifest.eof()) {
        throw std::runtime_error("Unexpected data in MANIFEST");
    }
    for (const auto& [level, id] : levels) {
        files.emplace(level, openFile(id));
    }
}

std::filesystem::path SSTable::filePath(std::uint64_t id) {
    return std::to_string(id) + ".sst";
}

std::shared_ptr<std::ifstream> SSTable::openFile(std::uint64_t id) {
    auto file = std::make_shared<std::ifstream>();
    file->exceptions(std::ios::failbit | std::ios::badbit);
    file->open(filePath(id), std::ios::binary);
    return file;
}

void SSTable::writeManifest(std::uint64_t nextId, const Levels& newLevels) {
    std::ofstream manifest;
    manifest.exceptions(std::ios::failbit | std::ios::badbit);
    manifest.open("MANIFEST.tmp", std::ios::trunc);
    manifest << nextId << '\n' << newLevels.size() << '\n';
    for (const auto& [level, id] : newLevels) {
        manifest << level << ' ' << id << '\n';
    }
    manifest.close();
    std::filesystem::rename("MANIFEST.tmp", "MANIFEST");
}

void SSTable::flush(const Entries& entries) {
    auto newLevels = levels;
    // Share unchanged streams until MANIFEST commits; rollback keeps the old pool.
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
        save(filePath(current), entries);
        created.push_back(current);
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
        writeManifest(newNextId, newLevels);
    } catch (...) {
        newFiles.clear();
        for (const auto id : created) {
            std::error_code error;
            std::filesystem::remove(filePath(id), error);
        }
        throw;
    }
    levels.swap(newLevels);
    files.swap(newFiles);
    next_id = newNextId;
    // Close obsolete streams before deleting their files.
    newFiles.clear();
    for (const auto id : obsolete) {
        // Once committed, cleanup errors must not make DB retain a flushed WAL.
        std::error_code error;
        std::filesystem::remove(filePath(id), error);
    }
}

void SSTable::save(const std::filesystem::path& path, const Entries& entries) const {
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(path, std::ios::binary | std::ios::trunc);
    const auto count = static_cast<std::uint64_t>(entries.size());
    write(file, count);
    std::uint64_t offset = sizeof(count) + count * sizeof(std::uint64_t);
    for (const auto& [key, value] : entries) {
        write(file, offset);
        offset += sizeof(std::uint8_t) + sizeof(std::uint64_t) + key.size();
        if (value.second) {
            offset += sizeof(std::uint64_t) + value.first.size();
        }
    }
    for (const auto& [key, value] : entries) {
        writeRecord(file, {key, value.first, value.second});
    }
    file.close();
}

void SSTable::merge(std::ifstream& older, std::ifstream& newer, std::uint64_t output) {
    // Count records first to reserve space for the index before the data.
    std::uint64_t count = 0;
    mergeRecords(older, newer, [&](const Record&) { ++count; });
    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(filePath(output), std::ios::binary | std::ios::trunc);
    write(file, count);
    const auto offset = sizeof(count) + count * sizeof(std::uint64_t);
    std::vector<std::uint64_t> offsets;
    offsets.reserve(count);
    file.seekp(static_cast<std::streamoff>(offset));
    mergeRecords(older, newer, [&](const Record& record) {
        offsets.push_back(static_cast<std::uint64_t>(file.tellp()));
        writeRecord(file, record);
    });
    file.seekp(static_cast<std::streamoff>(sizeof(count)));
    for (const auto recordOffset : offsets) {
        write(file, recordOffset);
    }
    file.close();
}

std::optional<std::string> SSTable::get(const std::string& key) const {
    return getThroughId(key, std::numeric_limits<std::uint64_t>::max());
}

std::optional<std::string> SSTable::getFromFile(const std::string& key, int i) const {
    return i < 0 ? std::nullopt : getThroughId(key, static_cast<std::uint64_t>(i));
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
        auto& file = *files.at(level);
        file.clear();
        file.seekg(0);
        const auto count = read<std::uint64_t>(file);
        std::uint64_t left = 0, right = count;
        while (left < right) {
            const auto middle = left + (right - left) / 2;
            file.seekg(static_cast<std::streamoff>(sizeof(count) + middle * sizeof(std::uint64_t)));
            const auto offset = read<std::uint64_t>(file);
            file.seekg(static_cast<std::streamoff>(offset));
            const auto flag = read<std::uint8_t>(file);
            if (flag > 1) {
                throw std::runtime_error("Invalid SSTable presence flag");
            }
            const auto storedKey = readString(file);
            if (storedKey == key) {
                return flag == 1 ? std::optional<std::string>(readString(file)) : std::nullopt;
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
