//
// Created by oleg on 10/3/26.
//

#ifndef KVSTORAGE_SNAPSHOT_H
#define KVSTORAGE_SNAPSHOT_H

#include <map>
#include <string>

class Snapshot {
public:
    Snapshot(std::string path, std::map<std::string, std::string>& map);

    void save(const std::map<std::string, std::string>& map) const;

private:
    std::string _path;
};

#endif  // KVSTORAGE_SNAPSHOT_H
