//
// Created by oleg on 9/28/26.
//

#ifndef KVSTORAGE_DB_H
#define KVSTORAGE_DB_H
#include <cstddef>
#include <optional>
#include <string>

#include "sstable.h"
#include "wal.h"

class DB {
public:
    static constexpr std::size_t MAX_MEMTABLE_SIZE = 20;

    explicit DB(const std::string& path);

    void put(std::string key, std::string value);
    std::optional<std::string> get(const std::string& key);
    void remove(const std::string& key);

private:
    SSTable::Entries _memtable;
    SSTable _sstable;
    Wal _wal;

    void flushIfNeeded();
};

#endif  // KVSTORAGE_DB_H
