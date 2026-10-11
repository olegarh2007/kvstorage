//
// Created by oleg on 9/28/26.
//

#include "../include/DB.h"

#include <utility>

DB::DB(const std::string& path) : _wal(path + ".wal", _memtable) {
    flushIfNeeded();
}

void DB::put(std::string key, std::string value) {
    _wal.put(key, value);
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

void DB::flushIfNeeded() {
    if (_memtable.size() < MAX_MEMTABLE_SIZE) {
        return;
    }

    _sstable.flush(_memtable);
    _wal.clear();
    _memtable.clear();
}
