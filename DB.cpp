//
// Created by oleg on 9/28/26.
//

#include "DB.h"

#include <cstdint>

#include <cstdint>
#include <ostream>
#include <string_view>

enum class Operation : std::uint8_t {
    Put = 1,
    Delete = 2,
};

struct Record {
    Operation operation;
    std::string key;
    std::string value;
};

bool read(std::ifstream& file,
          Operation& op,
          std::string& key,
          std::string& value) {
    size_t key_size;
    size_t value_size;

    if (!file.read((char*)&op, sizeof(op)))
        return false;

    file.read((char*)&key_size, sizeof(key_size));
    key.resize(key_size);
    file.read(key.data(), key_size);

    file.read((char*)&value_size, sizeof(value_size));
    value.resize(value_size);
    file.read(value.data(), value_size);

    return true;
}

void write(std::ofstream& file,
           Operation op,
           const std::string& key,
           const std::string& value) {
    size_t key_size = key.size();
    size_t value_size = value.size();

    file.write((char*)&op, sizeof(op));
    file.write((char*)&key_size, sizeof(key_size));
    file.write(key.data(), key_size);
    file.write((char*)&value_size, sizeof(value_size));
    file.write(value.data(), value_size);
}

DB::DB(const std::string &path) : _journal(path) {
    std::ifstream input_journal(path);
    Operation op;
    std::string key, value;
    while (read(input_journal, op, key, value)) {
        if (op == Operation::Put) {
            _map[key] = value;
        } else if (op == Operation::Delete) {
            _map.erase(key);
        }
    }
}

void DB::put(std::string key, std::string value) {
    write(_journal, Operation::Put, key, value);
    _map[key] = value;
}

std::optional<std::string> DB::get(const std::string &key) {
    if (!_map.contains(key)) {
        return std::nullopt;
    }
    return _map[key];
}

void DB::remove(const std::string &key) {
    write(_journal, Operation::Delete, key, "");
    _map.erase(key);
}

void DB::flush() {
    _journal.flush();
}


