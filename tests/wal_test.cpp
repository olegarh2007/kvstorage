#include "wal.h"
#include "file.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

//
// Created by oleg on 10/14/26.
//
TEST(WAL, AppendSimple) {
    const auto fs = std::make_shared<MemFS>();
    const std::string filename = "testfile";
    std::map<std::string, std::optional<std::string>> map;
    Wal wal(filename, map, fs);
    wal.append("key", "info");
    wal.append("ab\0c", "12\n3");
    wal.append("k3", "v3");
    fs->crash();

    std::string expected;
    const auto appendExpected = [&expected](std::string_view key, std::string_view value) {
        const std::uint64_t keySize = key.size();
        const std::uint64_t valueSize = value.size();
        expected.push_back('\x01');
        expected.append(reinterpret_cast<const char*>(&keySize), sizeof(keySize));
        expected.append(key);
        expected.append(reinterpret_cast<const char*>(&valueSize), sizeof(valueSize));
        expected.append(value);
    };
    appendExpected("key", "info");
    appendExpected("ab\0c", "12\n3");
    appendExpected("k3", "v3");

    std::unique_ptr<File> input = fs->open(filename, "rb");
    ASSERT_TRUE(static_cast<bool>(*input));
    ASSERT_EQ(input->size(), static_cast<off_t>(expected.size()));
    std::string actual(expected.size(), '\0');
    input->read(actual.data(), actual.size());
    ASSERT_EQ(actual, expected);
}

TEST(WAL, AppendRestartAppend) {
    const auto fs = std::make_shared<MemFS>();
    const std::string filename = "testfile";
    {
        std::map<std::string, std::optional<std::string>> map;
        Wal wal(filename, map, fs);
        wal.append("key1", "info1");
    }
    {
        std::map<std::string, std::optional<std::string>> map;
        Wal wal(filename, map, fs);
        wal.append("key2", "info2");
    }
    fs->crash();

    std::string expected;
    const auto appendExpected = [&expected](std::string_view key, std::string_view value) {
        const std::uint64_t keySize = key.size();
        const std::uint64_t valueSize = value.size();
        expected.push_back('\x01');
        expected.append(reinterpret_cast<const char*>(&keySize), sizeof(keySize));
        expected.append(key);
        expected.append(reinterpret_cast<const char*>(&valueSize), sizeof(valueSize));
        expected.append(value);
    };
    appendExpected("key1", "info1");
    appendExpected("key2", "info2");

    std::unique_ptr<File> input = fs->open(filename, "rb");
    ASSERT_TRUE(static_cast<bool>(*input));
    ASSERT_EQ(input->size(), static_cast<off_t>(expected.size()));
    std::string actual(expected.size(), '\0');
    input->read(actual.data(), actual.size());
    ASSERT_EQ(actual, expected);
}

TEST(WAL, ReplayValuesAndDeletions) {
    const auto fs = std::make_shared<MemFS>();
    const std::string filename = "testfile";
    {
        std::map<std::string, std::optional<std::string>> map;
        Wal wal(filename, map, fs);
        wal.append("key", "value");
        wal.append("empty", "");
        wal.append("deleted", "old");
        wal.append("deleted", std::nullopt);
        wal.append("missing", std::nullopt);
        wal.append("restored", std::nullopt);
        wal.append("restored", "new");
    }
    fs->crash();

    std::map<std::string, std::optional<std::string>> restored;
    Wal recovered(filename, restored, fs);
    ASSERT_EQ(restored.size(), 5);
    EXPECT_EQ(restored.at("key"), std::optional<std::string>("value"));
    EXPECT_EQ(restored.at("empty"), std::optional<std::string>(""));
    EXPECT_EQ(restored.at("deleted"), std::nullopt);
    EXPECT_EQ(restored.at("missing"), std::nullopt);
    EXPECT_EQ(restored.at("restored"), std::optional<std::string>("new"));
}

TEST(WAL, Size) {
    const auto fs = std::make_shared<MemFS>();
    const std::string filename = "testfile";

    {
        std::map<std::string, std::optional<std::string>> map;
        Wal wal(filename, map, fs);
        EXPECT_EQ(wal.size(), 0);

        wal.append("key1", "value1");
        wal.append("key2", "value2");
        wal.append("key1", "updated");
        wal.append("key2", std::nullopt);
        EXPECT_EQ(wal.size(), 4);
    }

    std::map<std::string, std::optional<std::string>> restored;
    Wal recovered(filename, restored, fs);
    // Count all complete records, including updates and deletions of the same key.
    EXPECT_EQ(recovered.size(), 4);

    recovered.append("key3", "value3");
    EXPECT_EQ(recovered.size(), 5);
}
