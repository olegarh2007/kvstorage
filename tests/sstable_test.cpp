#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "sstable.h"

class SSTableTest : public ::testing::Test {
protected:
    void SetUp() override {
        originalPath = std::filesystem::current_path();
        const auto id = std::chrono::steady_clock::now().time_since_epoch().count();
        testPath = std::filesystem::temp_directory_path() /
                   ("kvstorage_sstable_test_" + std::to_string(id));
        ASSERT_TRUE(std::filesystem::create_directory(testPath));
        std::filesystem::current_path(testPath);
    }

    void TearDown() override {
        std::filesystem::current_path(originalPath);
        std::filesystem::remove_all(testPath);
    }

    void fillForFlush(SSTable& table) {
        for (std::size_t i = 0; i < SSTable::MAX_MEMTABLE_SIZE - 1; ++i) {
            table.put("filler_" + std::to_string(i), "value");
        }
    }

private:
    std::filesystem::path originalPath;
    std::filesystem::path testPath;
};

TEST_F(SSTableTest, AddedRowsCanBeRead) {
    SSTable table;
    table.put("name", "Oleg");
    table.put("city", "Moscow");
    table.put("empty", "");

    EXPECT_EQ(table.get("name"), "Oleg");
    EXPECT_EQ(table.get("city"), "Moscow");
    EXPECT_EQ(table.get("empty"), "");
    EXPECT_FALSE(table.get("missing").has_value());
}

TEST_F(SSTableTest, RemovedRowIsAbsentAndOtherRowsRemain) {
    SSTable table;
    table.put("name", "Oleg");
    table.put("city", "Moscow");

    table.remove("name");
    table.remove("missing");

    EXPECT_FALSE(table.get("name").has_value());
    EXPECT_FALSE(table.get("missing").has_value());
    EXPECT_EQ(table.get("city"), "Moscow");
}

TEST_F(SSTableTest, RowCanBeUpdatedAndAddedAgainAfterRemoval) {
    SSTable table;
    table.put("key", "first");
    table.put("key", "second");
    EXPECT_EQ(table.get("key"), "second");

    table.remove("key");
    EXPECT_FALSE(table.get("key").has_value());

    table.put("key", "third");
    EXPECT_EQ(table.get("key"), "third");
}

TEST_F(SSTableTest, FlushedRowsCanBeReadAfterReopening) {
    {
        SSTable table;
        for (std::size_t i = 0; i < SSTable::MAX_MEMTABLE_SIZE; ++i) {
            table.put("key_" + std::to_string(i), "value_" + std::to_string(i));
        }
        ASSERT_TRUE(table.MemTable.empty());
    }

    SSTable reopened;
    for (std::size_t i = 0; i < SSTable::MAX_MEMTABLE_SIZE; ++i) {
        EXPECT_EQ(reopened.get("key_" + std::to_string(i)),
                  "value_" + std::to_string(i));
    }
    EXPECT_FALSE(reopened.get("missing").has_value());
}

TEST_F(SSTableTest, RemovalOfFlushedRowPersistsAfterReopening) {
    {
        SSTable table;
        table.put("key", "value");
        fillForFlush(table);
        ASSERT_TRUE(table.MemTable.empty());
        ASSERT_EQ(table.get("key"), "value");

        table.remove("key");
        EXPECT_FALSE(table.get("key").has_value());
        fillForFlush(table);
        ASSERT_TRUE(table.MemTable.empty());
        EXPECT_FALSE(table.get("key").has_value());
    }

    SSTable reopened;
    EXPECT_FALSE(reopened.get("key").has_value());
    EXPECT_EQ(reopened.get("filler_0"), "value");
}
