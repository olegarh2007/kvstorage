//
// Created by oleg on 9/28/26.
//

#include "../include/DB.h"

#include <utility>

DB::DB(const std::string& path, std::shared_ptr<const FileSystem> factory,
       std::size_t memtable_max_size)
    : _fileFactory(std::move(factory)),
      _memtableMaxSize(memtable_max_size),
      _sstable(_fileFactory, path),
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
void DB::put(std::string key, std::optional<std::string> value) {
    std::lock_guard lock(put_mutex_);
    _wal.append(key, value);
    {
        std::lock_guard get_lock(put_get_mutex_);
        _memtable.insert_or_assign(std::move(key), std::move(value));
    }
    flushIfNeeded();
}

std::optional<std::string> DB::get(const std::string& key) {
    std::shared_lock put_lock(put_get_mutex_);
    const auto entry = _memtable.find(key);
    if (entry != _memtable.end()) {
        return entry->second;
    }
    return _sstable.get(key);
}

std::map<std::string, std::string> DB::dump() const {
    auto result = _sstable.dump();
    for (const auto& [key, entry] : _memtable) {
        if (entry) {
            result.insert_or_assign(key, *entry);
        } else {
            result.erase(key);
        }
    }
    return result;
}

void DB::flushIfNeeded() {
    if (_wal.size() < _memtableMaxSize) {
        return;
    }

    auto prepared = _sstable.prepareFlush(_memtable);
    {
        std::lock_guard get_lock(put_get_mutex_);
        _sstable.commitFlush(prepared);
    }

    _wal.clear();
    _memtable.clear();
}

