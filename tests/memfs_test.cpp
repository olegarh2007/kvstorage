#include "file.h"

#include "gtest/gtest.h"

#include <thread>

TEST(MemFS, OpeningMissingFileForReadFails) {
    MemFS fs;
    EXPECT_THROW(fs.open("dir/missing", "rb"), std::ios_base::failure);
    EXPECT_THROW(fs.open("dir/missing", "r+"), std::ios_base::failure);
    EXPECT_FALSE(fs.exists("dir/missing"));
    EXPECT_TRUE(fs.open("dir/missing", "ab"));
}

TEST(MemFS, FileSyncDoesNotPersistDirectoryEntry) {
    MemFS fs;
    auto file = fs.open("dir/file", "wb");
    file->write("abc", 3);
    ASSERT_EQ(file->fsync(), 0);
    fs.crash();
    EXPECT_FALSE(fs.exists("dir/file"));
    EXPECT_TRUE(fs.list("dir").empty());
}

TEST(MemFS, DirectorySyncDoesNotPersistContents) {
    MemFS fs;
    auto file = fs.open("dir/file", "wb");
    file->write("abc", 3);
    fs.syncDirectory("dir/");
    fs.crash();
    ASSERT_TRUE(fs.exists("dir/file"));
    EXPECT_EQ(fs.open("dir/file", "rb")->size(), 0);
}

TEST(MemFS, RenameAndRemoveRequireDirectorySync) {
    MemFS fs;
    auto file = fs.open("dir/a", "wb");
    file->write("abc", 3);
    ASSERT_EQ(file->fsync(), 0);
    fs.syncDirectory("dir");
    fs.rename("dir/a", "dir/b");
    fs.crash();
    EXPECT_TRUE(fs.exists("dir/a"));
    EXPECT_FALSE(fs.exists("dir/b"));
    fs.rename("dir/a", "dir/b");
    fs.syncDirectory("dir");
    fs.crash();
    ASSERT_TRUE(fs.exists("dir/b"));
    EXPECT_FALSE(fs.exists("dir/a"));
    EXPECT_EQ(fs.open("dir/b", "rb")->size(), 3);
    fs.remove("dir/b");
    fs.crash();
    EXPECT_TRUE(fs.exists("dir/b"));
    fs.remove("dir/b");
    fs.syncDirectory("dir");
    fs.crash();
    EXPECT_FALSE(fs.exists("dir/b"));
}

TEST(MemFS, SyncOnlyPersistsSelectedDirectoryAndCountsAsStep) {
    MemFS fs;
    fs.open("one/a", "wb");
    fs.open("two/b", "wb");
    fs.setSteps(1);
    EXPECT_THROW(fs.syncDirectory("one"), std::ios_base::failure);
    EXPECT_TRUE(fs.exists("one/a"));
    EXPECT_FALSE(fs.exists("two/b"));
}

TEST(MemFS, ConcurrentAppendPreservesAllWrites) {
    MemFS fs;
    fs.open("dir/file", "wb");
    fs.syncDirectory("dir");
    std::vector<std::thread> writers;
    for (int i = 0; i < 4; ++i) {
        writers.emplace_back([&fs, i] {
            auto file = fs.open("dir/file", "ab");
            const char value = static_cast<char>('a' + i);
            for (int j = 0; j < 500; ++j) {
                file->write(&value, 1);
            }
            EXPECT_EQ(file->close(), 0);
        });
    }
    for (auto& writer : writers) writer.join();
    fs.fsync("dir/file");
    fs.crash();
    const auto content = fs.get("dir/file");
    ASSERT_EQ(content.size(), 2000);
    for (char value = 'a'; value <= 'd'; ++value) {
        EXPECT_EQ(std::count(content.begin(), content.end(), value), 500);
    }
}
