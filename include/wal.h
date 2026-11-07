//
// Created by oleg on 10/3/26.
//

#ifndef KVSTORAGE_WAL_H
#define KVSTORAGE_WAL_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "file.h"

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
    Wal(std::string path,
        std::map<std::string, std::optional<std::string>>& map,
        std::shared_ptr<const FileSystem> factory = std::make_shared<RegularFS>());

    // при успешном выполнении запись сохраняется в wal
    // и гарантированно переживает сбой
    // пока не удалена последущей операцией
    // std::nullopt записывает удаление ключа; пустая строка сохраняется как значение.
    void append(std::string_view key, std::optional<std::string_view> value);
    void clear();

    // выдает количество полностью записанных записей
    std::size_t size() const noexcept;

private:
    std::string _path;
    std::shared_ptr<const FileSystem> _fileFactory;
    std::unique_ptr<File> _out;
    std::size_t _size = 0;
};

#endif  // KVSTORAGE_WAL_H
