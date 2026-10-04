//
// Created by oleg on 10/4/26.
//

#include "wal.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

namespace {

class WalTest : public ::testing::Test {
protected:
    void SetUp() override {
        path = std::filesystem::path(::testing::TempDir()) /
               ("kvstorage_wal_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        directoryCreated = std::filesystem::create_directory(path);
        ASSERT_TRUE(directoryCreated);
        path /= "test.wal";
    }

    void TearDown() override {
        if (!directoryCreated) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path.parent_path(), error);
    }

    std::filesystem::path path;
    bool directoryCreated = false;
};

TEST_F(WalTest, RestoresSmallMapAndRecordCount) {
    const std::map<std::string, std::string> initial{
        {"apple", "red"}, {"banana", "yellow"}, {"grape", "purple"}};
    std::map<std::string, std::string> empty;
    Wal wal(path.string(), empty);
    ASSERT_EQ(wal.size(), 0u);

    for (const auto& [key, value] : initial) {
        wal.put(key, value);
    }
    ASSERT_EQ(wal.size(), initial.size());

    std::map<std::string, std::string> restored;
    Wal loaded(path.string(), restored);
    ASSERT_EQ(loaded.size(), initial.size());
    ASSERT_EQ(restored.size(), initial.size());
    EXPECT_EQ(restored, initial);

    loaded.put("apple", "green");
    loaded.remove("banana");
    ASSERT_EQ(loaded.size(), 5u);

    restored.clear();
    Wal reloaded(path.string(), restored);
    const std::map<std::string, std::string> expected{
        {"apple", "green"}, {"grape", "purple"}};
    ASSERT_EQ(reloaded.size(), 5u);
    ASSERT_EQ(restored.size(), expected.size());
    EXPECT_EQ(restored, expected);
}

}  // namespace
