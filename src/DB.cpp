//
// Created by oleg on 9/28/26.
//

#include "../include/DB.h"

#include <utility>

DB::DB(const std::string& path, FileFactory factory)
    : _fileFactory(std::move(factory)),
      _sstable(_fileFactory),
      _wal(path + ".wal", _memtable, _fileFactory) {
    flushIfNeeded();
}

// если пут вернулся успешно то и вал.аппенд вернулся успешно
// а значит на диске останется корректная копия =>
// по контракту wal подберется рекавери => окажется в мем тейбл после успешного
// wal.append,после краша
// значение окажется в мемтейбл по insert_or_assign или по контракту
// восстановления wal.append из wal =>
// flushIfNeeded сбрасывает memtable в sstable файл
// по контракту сстейбл операция которая успешно сброщена в сстейбл восстановима
void DB::put(std::string key, std::string value) {
    _wal.append(key, value);
    _memtable.insert_or_assign(std::move(key), std::make_pair(std::move(value), true));
    flushIfNeeded();
}

std::optional<std::string> DB::get(const std::string& key) {
    const auto entry = _memtable.find(key);
    if (entry != _memtable.end()) {
        if (entry->second.second) {
            return entry->second.first;
        }
        return std::nullopt;
    }
    return _sstable.get(key);
}

void DB::remove(const std::string& key) {
    _wal.remove(key);
    _memtable.insert_or_assign(key, std::make_pair(std::string{}, false));
    flushIfNeeded();
}

std::map<std::string, std::string> DB::dump() const {
    auto result = _sstable.dump();
    for (const auto& [key, entry] : _memtable) {
        if (entry.second) {
            result.insert_or_assign(key, entry.first);
        } else {
            result.erase(key);
        }
    }
    return result;
}

// сбрасывает накопленный wal в sstable фвйл
// либо сбрасывает либо не теряет
void DB::flushIfNeeded() {
    if (_wal.size() < MAX_WAL_SIZE) {
        return;
    }

    _sstable.flush(_memtable);
    _wal.clear();
    _memtable.clear();
}
