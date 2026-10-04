//
// Created by oleg on 9/28/26.
//

#include "../include/DB.h"

#include <utility>

DB::DB(const std::string& path) : _snapshot(path + ".snapshot", _map), _wal(path + ".wal", _map) {}

void DB::put(std::string key, std::string value) {
    _wal.put(key, value);
    _map.insert_or_assign(std::move(key), std::move(value));
    if (_wal.size() >= MAX_WAL_SIZE) {
        _snapshot.save(_map);
        _wal.clear();
    }
}

std::optional<std::string> DB::get(const std::string& key) {
    const auto it = _map.find(key);
    if (it == _map.end()) {
        return std::nullopt;
    }
    return it->second;
}

void DB::remove(const std::string& key) {
    _wal.remove(key);
    _map.erase(key);
    if (_wal.size() >= MAX_WAL_SIZE) {
        _snapshot.save(_map);
        _wal.clear();
    }
}
