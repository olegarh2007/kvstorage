#include "BloomFilter.h"

#include <string>

#include "gtest/gtest.h"

TEST(BloomFilterTest, EmptyFilter) {
    const BloomFilter filter;

    EXPECT_FALSE(filter.possiblyContains(""));
    EXPECT_FALSE(filter.possiblyContains("missing"));
    EXPECT_FALSE(filter.possiblyContains("another key"));
}

TEST(BloomFilterTest, InsertedKeys) {
    BloomFilter filter(1000);
    for (int i = 0; i < 1000; ++i) {
        filter.add("key-" + std::to_string(i));
    }

    for (int i = 0; i < 1000; ++i) {
        EXPECT_TRUE(filter.possiblyContains("key-" + std::to_string(i))) << i;
    }
}

TEST(BloomFilterTest, FalsePositiveRate) {
    constexpr int inserted_count = 10000;
    constexpr int query_count = 100000;
    BloomFilter filter(inserted_count, 0.01);
    for (int i = 0; i < inserted_count; ++i) {
        filter.add("inserted-" + std::to_string(i));
    }

    int false_positives = 0;
    for (int i = 0; i < query_count; ++i) {
        false_positives += filter.possiblyContains("absent-" + std::to_string(i));
    }

    EXPECT_LT(static_cast<double>(false_positives) / query_count, 0.02)
        << "False positives: " << false_positives;
}
