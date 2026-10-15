#include "wal.h"
#include "file.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

//
// Created by oleg on 10/14/26.
//
TEST(WAL, AppendSimple) {
    in_memory_fs fs;
    const FileFactory factory(fs);
    const std::string filename = "testfile";
    std::map<std::string, std::pair<std::string, bool>> map;
    Wal wal(filename, map, factory);
    wal.append("key", "info");
    wal.append("abc", "123");
    wal.append("k3", "v3");
    fs.crash();

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
    appendExpected("abc", "123");
    appendExpected("k3", "v3");

    std::unique_ptr<File> input = factory.create(filename, "rb");
    ASSERT_TRUE(static_cast<bool>(*input));
    ASSERT_EQ(input->size(), static_cast<off_t>(expected.size()));
    std::string actual(expected.size(), '\0');
    input->read(actual.data(), actual.size());
    ASSERT_EQ(actual, expected);
}

TEST(WAL, AppendRestartAppend) {
    in_memory_fs fs;
    const FileFactory factory(fs);
    const std::string filename = "testfile";
    {
        std::map<std::string, std::pair<std::string, bool>> map;
        Wal wal(filename, map, factory);
        wal.append("key1", "info1");
    }
    {
        std::map<std::string, std::pair<std::string, bool>> map;
        Wal wal(filename, map, factory);
        wal.append("key2", "info2");
    }
    fs.crash();

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

    std::unique_ptr<File> input = factory.create(filename, "rb");
    ASSERT_TRUE(static_cast<bool>(*input));
    ASSERT_EQ(input->size(), static_cast<off_t>(expected.size()));
    std::string actual(expected.size(), '\0');
    input->read(actual.data(), actual.size());
    ASSERT_EQ(actual, expected);
}
