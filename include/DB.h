//
// Created by oleg on 9/28/26.
//

#ifndef KVSTORAGE_DB_H
#define KVSTORAGE_DB_H
#include <cstddef>
#include <map>
#include <optional>
#include <string>

#include "sstable.h"
#include "wal.h"

class DB {
public:
    static constexpr std::size_t MAX_WAL_SIZE = 100;

    explicit DB(const std::string& path, FileFactory factory = {});

    // гарантирует запись пары key value,если завершился успешно
    // если завершился с ошибкой, то после восстановления
    // может быть доступно как старое так и новое значение
    // частичная запись недоступна
    void put(std::string key, std::string value);
    // Возвращает текущее значение, связанное с key,
    // если ключ существует в БД.
    // Если ключ отсутствует, возвращает std::nullopt.
    // Не изменяет логическое состояние БД.
    std::optional<std::string> get(const std::string& key);
    void remove(const std::string& key);
    std::map<std::string, std::string> dump() const;

private:
    FileFactory _fileFactory;
    SSTable::Entries _memtable;
    SSTable _sstable;
    Wal _wal;

    void flushIfNeeded();
};

#endif  // KVSTORAGE_DB_H
