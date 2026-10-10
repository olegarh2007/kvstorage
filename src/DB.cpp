//
// Created by oleg on 9/28/26.
//

#include "../include/DB.h"

#include <utility>

DB::DB(const std::string&) {}

void DB::put(std::string key, std::string value) {
    _sstable.put(std::move(key), std::move(value));
}

std::optional<std::string> DB::get(const std::string& key) {
    return _sstable.get(key);
}

void DB::remove(const std::string& key) {
    _sstable.remove(key);
}
