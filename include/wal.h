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
#include <utility>

// Формат WAL: записи идут подряд, без заголовка файла и разделителей.
//
// Put (operation = 1):
// +-----------+------------+-------------+--------------+---------------+
// | operation | key_size   | key         | value_size   | value         |
// | 1 byte    | 8 bytes    | key_size B  | 8 bytes      | value_size B  |
// +-----------+------------+-------------+--------------+---------------+
//
// Delete (operation = 2):
// +-----------+------------+-------------+
// | operation | key_size   | key         |
// | 1 byte    | 8 bytes    | key_size B  |
// +-----------+------------+-------------+
//
// operation — uint8_t; размеры — uint64_t в порядке байтов платформы.
// key и value — сырые байты без завершающего '\0'; размеры могут быть нулевыми.
// Delete не содержит ни value_size, ни value.
// При восстановлении записи применяются последовательно, от начала до конца.
// Недописанная последняя запись отбрасывается, файл обрезается до полной записи.
class Wal {
public:
    enum class Operation : std::uint8_t {
        Put = 1,
        Delete = 2,
    };

    // Replay complete records in order, retaining deletion markers for SSTables.
    // Truncate an incomplete final record before accepting new writes.
    Wal(std::string path, std::map<std::string, std::pair<std::string, bool>>& map);

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
