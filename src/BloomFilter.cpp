//
// Created by oleg on 11/11/26.
//

#include "BloomFilter.h"

#include <cmath>
#include <stdexcept>

BloomFilter::BloomFilter(size_t expected_count, double false_positive_rate) {
    if (expected_count == 0 || !(false_positive_rate > 0 && false_positive_rate < 1)) {
        throw std::invalid_argument("Invalid BloomFilter parameters");
    }

    constexpr double ln2 = 0.6931471805599453;

    size_t bit_count = std::ceil(
        -static_cast<double>(expected_count) * std::log(false_positive_rate) / (ln2 * ln2)
    );

    hash_count_ = std::max<size_t>(
        1, std::round(static_cast<double>(bit_count) / expected_count * ln2)
    );

    bits_.resize(bit_count);
}

uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

void BloomFilter::add(std::string_view key) {
    size_t h = std::hash<std::string_view>{}(key);
    size_t h1 = mix(h);
    size_t h2 = mix(h ^ 0x9e3779b97f4a7c15ULL) | 1ULL;
    for (int i = 0; i < hash_count_; ++i) {
        size_t id = (h1 + i * h2) % bits_.size();
        bits_[id] = true;
    }
}

bool BloomFilter::possiblyContains(std::string_view key) const {
    size_t h = std::hash<std::string_view>{}(key);
    size_t h1 = mix(h);
    size_t h2 = mix(h ^ 0x9e3779b97f4a7c15ULL) | 1ULL;
    for (int i = 0; i < hash_count_; ++i) {
        size_t id = (h1 + i * h2) % bits_.size();
        if (!bits_[id]) {
            return false;
        }
    }
    return true;
}

