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

TEST(WAL, ReplayTruncatesIncompleteTail) {
    // Build the input independently of Wal::append, using the on-disk format.
    const auto encodeString = [](std::string_view value) {
        const std::uint64_t size = value.size();
        std::string bytes(reinterpret_cast<const char*>(&size), sizeof(size));
        bytes.append(value);
        return bytes;
    };
    const auto put = [&](std::string_view key, std::string_view value) {
        return std::string(1, '\x01') + encodeString(key) + encodeString(value);
    };
    const auto remove = [&](std::string_view key) {
        return std::string(1, '\x02') + encodeString(key);
    };
    const std::string complete = put("key", "old") + put("empty", "") +
        put("deleted", "value") + remove("deleted") + put("key", "new");
    const std::map<std::string, std::optional<std::string>> expected = {
        {"key", "new"}, {"empty", ""}, {"deleted", std::nullopt},
    };

    // Try every incomplete prefix, including cuts inside lengths and payloads.
    for (const auto& lastRecord : {put("key", "unfinished"), remove("key")}) {
        for (std::size_t tailSize = 1; tailSize < lastRecord.size(); ++tailSize) {
            SCOPED_TRACE(::testing::Message()
                         << "operation=" << static_cast<int>(lastRecord.front())
                         << ", tail size=" << tailSize);
            const auto fs = std::make_shared<MemFS>();
            const std::string filename = "testfile";
            const std::string content = complete + lastRecord.substr(0, tailSize);
            {
                auto file = fs->open(filename, "wb");
                file->write(content.data(), content.size());
                ASSERT_EQ(file->close(), 0);
            }

            std::map<std::string, std::optional<std::string>> restored;
            {
                Wal recovered(filename, restored, fs);
                EXPECT_EQ(restored, expected);
                EXPECT_EQ(recovered.size(), 5);
                // Check the file itself: ignoring the tail during replay is insufficient.
                EXPECT_EQ(fs->get(filename), complete);
            }

            restored.clear();
            Wal reopened(filename, restored, fs);
            EXPECT_EQ(restored, expected);
            EXPECT_EQ(reopened.size(), 5);
            EXPECT_EQ(fs->get(filename), complete);
        }
    }
}


