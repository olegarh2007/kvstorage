//
// Created by oleg on 11/11/26.
//

#ifndef KVSTORAGE_BLOOMFILTER_H
#define KVSTORAGE_BLOOMFILTER_H
#include <cstdint>
#include <string_view>
#include <vector>

class BloomFilter {
public:
    BloomFilter(size_t expected_count = 1000, double false_positive_rate = 0.01);

    void add(std::string_view key);
    bool possiblyContains(std::string_view key) const;

private:
    std::vector<bool> bits_;
    size_t hash_count_;
};

#endif  // KVSTORAGE_BLOOMFILTER_H
