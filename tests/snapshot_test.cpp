//
// Created by oleg on 10/4/26.
//

#include "snapshot.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

namespace {

class SnapshotTest : public ::testing::Test {
protected:
    void SetUp() override {
        path = std::filesystem::path(::testing::TempDir()) /
               ("kvstorage_snapshot_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        directoryCreated = std::filesystem::create_directory(path);
        ASSERT_TRUE(directoryCreated);
        path /= "snapshot.db";
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

TEST_F(SnapshotTest, EmptySnapshotRestoresEmptyMap) {
    std::map<std::string, std::string> empty;
    Snapshot snapshot(path.string(), empty);
    snapshot.save(empty);

    std::map<std::string, std::string> restored{{"old", "value"}};
    Snapshot loaded(path.string(), restored);

    EXPECT_TRUE(restored.empty());
}

TEST_F(SnapshotTest, RestoresSmallMap) {
    const std::map<std::string, std::string> expected{
        {"apple", "red"}, {"banana", "yellow"}, {"grape", "purple"}};
    std::map<std::string, std::string> empty;
    Snapshot snapshot(path.string(), empty);
    snapshot.save(expected);

    std::map<std::string, std::string> restored;
    Snapshot loaded(path.string(), restored);

    ASSERT_EQ(restored.size(), expected.size());
    EXPECT_EQ(restored, expected);
}

}  // namespace
