//
// Created by oleg on 10/3/26.
//

#ifndef KVSTORAGE_WAL_H
#define KVSTORAGE_WAL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

class Wal {
public:
    enum class Operation : std::uint8_t {
        Put = 1,
        Delete = 2,
    };

    Wal(std::string path, std::map<std::string, std::string>& map);

    // Append a record; sizes are uint64_t in native byte order.
    void put(std::string_view key, std::string_view value);
    void remove(std::string_view key);
    void clear();

    // Number of records in the WAL, including restored records.
    std::size_t size() const noexcept;

private:
    void write(Operation operation, std::string_view key, std::string_view value);

    std::string _path;
    std::size_t _size = 0;
};

#endif  // KVSTORAGE_WAL_H
