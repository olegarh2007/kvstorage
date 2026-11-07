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
    // File prefix: path.wal, path.MANIFEST, path.<id>.sst.
    // The parent directory must already exist.
    // Flush after memtable_max_size WAL records, including repeated keys.
    explicit DB(const std::string& path,
                std::shared_ptr<const FileSystem> factory = std::make_shared<RegularFS>(),
                std::size_t memtable_max_size = 1'00'00);

    // гарантирует запись пары key value,если завершился успешно
    // если завершился с ошибкой, то после восстановления
    // может быть доступно как старое так и новое значение
    // частичная запись недоступна
    // std::nullopt удаляет ключ; пустая строка сохраняется как значение.
    void put(std::string key, std::optional<std::string> value);
    // Возвращает текущее значение, связанное с key,
    // если ключ существует в БД.
    // Если ключ отсутствует, возвращает std::nullopt.
    // Не изменяет логическое состояние БД.
    std::optional<std::string> get(const std::string& key);
    std::map<std::string, std::string> dump() const;
private:
    std::shared_ptr<const FileSystem> _fileFactory;
    std::size_t _memtableMaxSize;
    SSTable::EntryMap _memtable;
    SSTable _sstable;
    Wal _wal;

    void flushIfNeeded();
};

#endif  // KVSTORAGE_DB_H
