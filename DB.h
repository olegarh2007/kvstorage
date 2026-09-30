//
// Created by oleg on 9/28/26.
//

#ifndef KVSTORAGE_DB_H
#define KVSTORAGE_DB_H
#include <map>
#include <optional>
#include <string>
#include <iostream>
#include <fstream>

class DB {
public:
    explicit DB(const std::string& path);

    void put(std::string key, std::string value);
    std::optional<std::string> get(const std::string& key);
    void remove(const std::string& key);

    void flush();
private:
    std::map<std::string, std::string> _map;
    std::ofstream _journal;
};

#endif //KVSTORAGE_DB_H
