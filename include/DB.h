//
// Created by oleg on 9/28/26.
//

#ifndef KVSTORAGE_DB_H
#define KVSTORAGE_DB_H
#include <map>
#include <optional>
#include <string>

#include "snapshot.h"
#include "wal.h"

#define MAX_WAL_SIZE 20

class DB {
public:
    explicit DB(const std::string& path);

    void put(std::string key, std::string value);
    std::optional<std::string> get(const std::string& key);
    void remove(const std::string& key);

private:
    std::map<std::string, std::string> _map;
    Snapshot _snapshot;
    Wal _wal;
};

#endif  // KVSTORAGE_DB_H
