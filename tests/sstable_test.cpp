#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "DB.h"

class DBTest : public ::testing::Test {
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

    void fillForFlush(DB& table) {
        for (std::size_t i = 0; i < DB::MAX_MEMTABLE_SIZE - 1; ++i) {
            table.put("filler_" + std::to_string(i), "value");
        }
    }

private:
    std::filesystem::path originalPath;
    std::filesystem::path testPath;
};

TEST_F(DBTest, AddedRowsCanBeRead) {
    DB table("test.db");
    table.put("name", "Oleg");
    table.put("city", "Moscow");
    table.put("empty", "");

    EXPECT_EQ(table.get("name"), "Oleg");
    EXPECT_EQ(table.get("city"), "Moscow");
    EXPECT_EQ(table.get("empty"), "");
    EXPECT_FALSE(table.get("missing").has_value());
}

TEST_F(DBTest, RemovedRowIsAbsentAndOtherRowsRemain) {
    DB table("test.db");
    table.put("name", "Oleg");
    table.put("city", "Moscow");

    table.remove("name");
    table.remove("missing");

    EXPECT_FALSE(table.get("name").has_value());
    EXPECT_FALSE(table.get("missing").has_value());
    EXPECT_EQ(table.get("city"), "Moscow");
}

TEST_F(DBTest, RowCanBeUpdatedAndAddedAgainAfterRemoval) {
    DB table("test.db");
    table.put("key", "first");
    table.put("key", "second");
    EXPECT_EQ(table.get("key"), "second");

    table.remove("key");
    EXPECT_FALSE(table.get("key").has_value());

    table.put("key", "third");
    EXPECT_EQ(table.get("key"), "third");
}

TEST_F(DBTest, FlushedRowsCanBeReadAfterReopening) {
    {
        DB table("test.db");
        for (std::size_t i = 0; i < DB::MAX_MEMTABLE_SIZE; ++i) {
            table.put("key_" + std::to_string(i), "value_" + std::to_string(i));
        }
        ASSERT_TRUE(std::filesystem::exists("0.sst"));
    }

    DB reopened("test.db");
    for (std::size_t i = 0; i < DB::MAX_MEMTABLE_SIZE; ++i) {
        EXPECT_EQ(reopened.get("key_" + std::to_string(i)), "value_" + std::to_string(i));
    }
    EXPECT_FALSE(reopened.get("missing").has_value());
}

TEST_F(DBTest, RemovalOfFlushedRowPersistsAfterReopening) {
    {
        DB table("test.db");
        table.put("key", "value");
        fillForFlush(table);
        ASSERT_TRUE(std::filesystem::exists("0.sst"));
        ASSERT_EQ(table.get("key"), "value");

        table.remove("key");
        EXPECT_FALSE(table.get("key").has_value());
        fillForFlush(table);
        ASSERT_TRUE(std::filesystem::exists("2.sst"));
        EXPECT_FALSE(table.get("key").has_value());
    }

    DB reopened("test.db");
    EXPECT_FALSE(reopened.get("key").has_value());
    EXPECT_EQ(reopened.get("filler_0"), "value");
}

class SSTableTest : public DBTest {};

TEST_F(SSTableTest, ReopenedTableKeepsManifestFilesOpenForRepeatedReads) {
    {
        SSTable table;
        table.flush({{"a", {"first", true}}, {"z", {"last", true}}});
    }

    SSTable reopened;
    // Reads must use the open handle even when the original path is unavailable.
    std::filesystem::rename("0.sst", "0.sst.held");
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(reopened.get("z"), "last");
        EXPECT_FALSE(reopened.get("missing"));
        EXPECT_EQ(reopened.get("a"), "first");
        EXPECT_EQ(reopened.getFromFile("a", 0), "first");
    }
}

TEST_F(SSTableTest, CascadingMergesReuseOpenFilesAndCommitTheNewPool) {
    SSTable table;
    table.flush({{"old", {"value", true}}, {"shared", {"first", true}}});
    std::filesystem::rename("0.sst", "0.sst.held");
    table.flush({{"shared", {"second", true}}});
    EXPECT_EQ(table.get("old"), "value");
    EXPECT_EQ(table.get("shared"), "second");

    table.flush({{"deleted", {"value", true}}});
    // Both occupied levels must be read from the pool during the next flush.
    std::filesystem::rename("2.sst", "2.sst.held");
    std::filesystem::rename("3.sst", "3.sst.held");
    table.flush({{"deleted", {"", false}}, {"shared", {"latest", true}}});
    EXPECT_EQ(table.get("old"), "value");
    EXPECT_EQ(table.get("shared"), "latest");
    EXPECT_FALSE(table.get("deleted"));

    SSTable reopened;
    EXPECT_EQ(reopened.get("old"), "value");
    EXPECT_EQ(reopened.get("shared"), "latest");
    EXPECT_FALSE(reopened.get("deleted"));
}

TEST_F(SSTableTest, FlushWritesProvidedRowsAndDeletionMarkers) {
    SSTable table;
    const SSTable::Entries entries{
        {"name", {"Oleg", true}},
        {"empty", {"", true}},
        {"deleted", {"", false}},
    };
    table.flush(entries);

    SSTable reopened;
    EXPECT_EQ(reopened.get("name"), "Oleg");
    EXPECT_EQ(reopened.get("empty"), "");
    EXPECT_FALSE(reopened.get("deleted").has_value());
    EXPECT_FALSE(reopened.get("missing").has_value());
}

TEST_F(DBTest, MemoryUpdateOverridesFlushedValueAndPersistsAfterFlush) {
    {
        DB table("test.db");
        table.put("key", "old");
        fillForFlush(table);
        ASSERT_EQ(table.get("key"), "old");

        table.put("key", "new");
        EXPECT_EQ(table.get("key"), "new");
        fillForFlush(table);
    }

    DB reopened("test.db");
    EXPECT_EQ(reopened.get("key"), "new");
}

TEST_F(DBTest, WalRecordsAllMemoryOperationsInOrder) {
    {
        DB table("test.db");
        table.put("updated", "old");
        table.put("updated", "new");
        table.put("deleted", "value");
        table.remove("deleted");
        table.remove("missing");
        table.put("reinserted", "old");
        table.remove("reinserted");
        table.put("reinserted", "new");
        table.put("empty", "");
    }

    ASSERT_FALSE(std::filesystem::exists("0.sst"));
    SSTable::Entries restored;
    Wal wal("test.db.wal", restored);
    const SSTable::Entries expected{
        {"updated", {"new", true}},
        {"deleted", {"", false}},
        {"missing", {"", false}},
        {"reinserted", {"new", true}},
        {"empty", {"", true}},
    };
    EXPECT_EQ(wal.size(), 9);
    EXPECT_EQ(restored, expected);

    const auto walBytes = std::filesystem::file_size("test.db.wal");
    DB reopened("test.db");
    EXPECT_EQ(reopened.get("updated"), "new");
    EXPECT_EQ(reopened.get("reinserted"), "new");
    EXPECT_EQ(reopened.get("empty"), "");
    EXPECT_FALSE(reopened.get("deleted").has_value());
    EXPECT_FALSE(reopened.get("missing").has_value());
    EXPECT_EQ(std::filesystem::file_size("test.db.wal"), walBytes);
}

TEST_F(DBTest, RecoveredDeletionHidesValueInSSTable) {
    {
        DB table("test.db");
        table.put("key", "disk value");
        fillForFlush(table);
        ASSERT_EQ(std::filesystem::file_size("test.db.wal"), 0);
        table.remove("key");
    }

    ASSERT_EQ(SSTable().get("key"), "disk value");
    DB reopened("test.db");
    EXPECT_FALSE(reopened.get("key").has_value());
    EXPECT_EQ(reopened.get("filler_0"), "value");
}

TEST_F(DBTest, WalIsClearedAfterEachFlushAndContainsOnlyPendingOperations) {
    {
        DB table("test.db");
        table.put("key", "old");
        fillForFlush(table);
        ASSERT_TRUE(std::filesystem::exists("0.sst"));
        EXPECT_EQ(std::filesystem::file_size("test.db.wal"), 0);

        table.remove("key");
        fillForFlush(table);
        ASSERT_TRUE(std::filesystem::exists("2.sst"));
        EXPECT_EQ(std::filesystem::file_size("test.db.wal"), 0);

        table.put("pending", "value");
        SSTable::Entries restored;
        Wal wal("test.db.wal", restored);
        const SSTable::Entries expected{{"pending", {"value", true}}};
        EXPECT_EQ(wal.size(), 1);
        EXPECT_EQ(restored, expected);
    }

    DB reopened("test.db");
    EXPECT_EQ(reopened.get("pending"), "value");
    EXPECT_FALSE(reopened.get("key").has_value());
}

TEST_F(DBTest, RecoveredMemoryCountsTowardNextFlush) {
    {
        DB table("test.db");
        fillForFlush(table);
    }
    ASSERT_FALSE(std::filesystem::exists("0.sst"));

    DB reopened("test.db");
    reopened.put("last", "value");
    EXPECT_TRUE(std::filesystem::exists("0.sst"));
    EXPECT_EQ(std::filesystem::file_size("test.db.wal"), 0);
    EXPECT_EQ(SSTable().get("last"), "value");
    EXPECT_EQ(SSTable().get("filler_0"), "value");
}

TEST_F(DBTest, FailedWalWriteDoesNotChangeMemory) {
    if (!std::filesystem::exists("/dev/full")) {
        GTEST_SKIP() << "Requires /dev/full to simulate a write failure";
    }
    std::filesystem::create_symlink("/dev/full", "test.db.wal");
    DB table("test.db");

    EXPECT_THROW(table.put("key", "changed"), std::runtime_error);
    EXPECT_THROW(table.remove("key"), std::runtime_error);
    EXPECT_FALSE(table.get("key").has_value());
}

TEST_F(DBTest, FailedSSTableWriteKeepsWalForRecovery) {
    {
        DB table("test.db");
        fillForFlush(table);
        ASSERT_TRUE(std::filesystem::create_directory("0.sst"));

        EXPECT_THROW(table.put("last", "value"), std::ios_base::failure);
        EXPECT_GT(std::filesystem::file_size("test.db.wal"), 0);
        EXPECT_EQ(table.get("last"), "value");
    }

    std::filesystem::remove("0.sst");
    DB reopened("test.db");
    EXPECT_EQ(reopened.get("last"), "value");
    EXPECT_EQ(reopened.get("filler_0"), "value");
    EXPECT_TRUE(std::filesystem::is_regular_file("0.sst"));
    EXPECT_EQ(std::filesystem::file_size("test.db.wal"), 0);
}

TEST_F(DBTest, TruncatedLastWalRecordIsDiscardedAndNewWritesSurviveReopening) {
    {
        DB table("test.db");
        table.put("updated", "old");
        table.put("updated", "new");
        table.put("deleted", "value");
        table.remove("deleted");
        table.put("reinserted", "old");
        table.remove("reinserted");
        table.put("reinserted", "new");
    }
    const auto completeBytes = std::filesystem::file_size("test.db.wal");

    for (const auto operation : {Wal::Operation::Put, Wal::Operation::Delete}) {
        // Encode the final record independently, then append only its prefix.
        std::string record(1, static_cast<char>(operation));
        const auto appendString = [&record](const std::string& value) {
            const std::uint64_t size = value.size();
            record.append(reinterpret_cast<const char*>(&size), sizeof(size));
            record.append(value);
        };
        appendString("updated");
        if (operation == Wal::Operation::Put) {
            appendString("incomplete value");
        }

        for (std::size_t bytes = 1; bytes < record.size(); ++bytes) {
            SCOPED_TRACE(::testing::Message()
                         << "operation=" << static_cast<int>(operation) << ", bytes=" << bytes);
            std::filesystem::resize_file("test.db.wal", completeBytes);
            {
                std::ofstream out("test.db.wal", std::ios::binary | std::ios::app);
                ASSERT_TRUE(out);
                out.write(record.data(), static_cast<std::streamsize>(bytes));
                out.close();
                ASSERT_TRUE(out);
            }

            {
                DB recovered("test.db");
                EXPECT_EQ(recovered.get("updated"), "new");
                EXPECT_EQ(recovered.get("reinserted"), "new");
                EXPECT_FALSE(recovered.get("deleted").has_value());
                EXPECT_EQ(std::filesystem::file_size("test.db.wal"), completeBytes);

                SSTable::Entries entries;
                Wal wal("test.db.wal", entries);
                EXPECT_EQ(wal.size(), 7);

                recovered.put("after_recovery", "saved");
                recovered.remove("reinserted");
            }

            DB reopened("test.db");
            EXPECT_EQ(reopened.get("updated"), "new");
            EXPECT_EQ(reopened.get("after_recovery"), "saved");
            EXPECT_FALSE(reopened.get("deleted").has_value());
            EXPECT_FALSE(reopened.get("reinserted").has_value());
        }
    }
}

TEST_F(DBTest, UnknownWalOperationIsRejected) {
    {
        DB table("test.db");
        table.put("key", "value");
    }
    {
        std::ofstream out("test.db.wal", std::ios::binary | std::ios::app);
        ASSERT_TRUE(out);
        out.put(static_cast<char>(255));
        out.close();
        ASSERT_TRUE(out);
    }
    const auto originalBytes = std::filesystem::file_size("test.db.wal");
    EXPECT_THROW(DB("test.db"), std::runtime_error);
    EXPECT_EQ(std::filesystem::file_size("test.db.wal"), originalBytes);
}

TEST_F(SSTableTest, CascadingMergesPersistLevelsAndAllocateMonotonicIds) {
    for (int batch = 0; batch < 8; ++batch) {
        SSTable table;
        table.flush({{"key_" + std::to_string(batch), {"value", true}},
                     {"shared", {std::to_string(batch), true}}});
        SSTable reopened;
        EXPECT_EQ(reopened.get("shared"), std::to_string(batch));
        for (int i = 0; i <= batch; ++i) {
            EXPECT_EQ(reopened.get("key_" + std::to_string(i)), "value");
        }
        std::ifstream manifest("MANIFEST");
        std::uint64_t nextId, count;
        ASSERT_TRUE(static_cast<bool>(manifest >> nextId >> count));
        std::size_t expectedCount = 0;
        auto batches = batch + 1;
        for (std::uint64_t level = 0; batches != 0; ++level, batches >>= 1) {
            if ((batches & 1) != 0) {
                std::uint64_t storedLevel, id;
                ASSERT_TRUE(static_cast<bool>(manifest >> storedLevel >> id));
                EXPECT_EQ(storedLevel, level);
                EXPECT_LT(id, nextId);
                EXPECT_TRUE(std::filesystem::is_regular_file(std::to_string(id) + ".sst"));
                ++expectedCount;
            }
        }
        EXPECT_EQ(count, expectedCount);
        std::size_t fileCount = 0;
        for (const auto& entry : std::filesystem::directory_iterator(".")) {
            if (entry.path().extension() == ".sst") {
                ++fileCount;
            }
        }
        EXPECT_EQ(fileCount, expectedCount);
    }
    std::ifstream manifest("MANIFEST");
    std::uint64_t nextId, count, level, id;
    ASSERT_TRUE(static_cast<bool>(manifest >> nextId >> count >> level >> id));
    EXPECT_EQ(nextId, 15);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(level, 3);
    EXPECT_EQ(id, 14);
}

TEST_F(SSTableTest, NewestFileWinsAcrossLevelsAndMergesPreserveDeletions) {
    SSTable table;
    table.flush(
        {{"deleted", {"old", true}}, {"updated", {"old", true}}, {"reinserted", {"old", true}}});
    table.flush({{"unrelated", {"value", true}}});
    table.flush(
        {{"deleted", {"", false}}, {"updated", {"new", true}}, {"reinserted", {"", false}}});
    EXPECT_FALSE(table.get("deleted"));
    EXPECT_FALSE(table.get("reinserted"));
    EXPECT_EQ(table.get("updated"), "new");
    table.flush({{"reinserted", {"back", true}}});
    SSTable reopened;
    EXPECT_FALSE(reopened.get("deleted"));
    EXPECT_EQ(reopened.get("updated"), "new");
    EXPECT_EQ(reopened.get("reinserted"), "back");
    EXPECT_EQ(reopened.get("unrelated"), "value");
}

TEST_F(SSTableTest, MergeHandlesEmptyFilesAndBinaryKeysAndValues) {
    const std::string key("a\0b", 3);
    const std::string value("x\0y", 3);
    SSTable table;
    table.flush({});
    table.flush({{"", {"", true}}, {key, {value, true}}, {"z", {"last", true}}});
    table.flush({});
    table.flush({{key, {"replacement", true}}});
    SSTable reopened;
    EXPECT_EQ(reopened.get(""), "");
    EXPECT_EQ(reopened.get(key), "replacement");
    EXPECT_EQ(reopened.get("z"), "last");
    EXPECT_FALSE(reopened.get("missing"));
}

TEST_F(SSTableTest, FailedMergeKeepsManifestAndOriginalFilesForRetry) {
    SSTable table;
    table.flush({{"key", {"old", true}}});
    ASSERT_TRUE(std::filesystem::create_directory("2.sst"));
    EXPECT_THROW(table.flush({{"key", {"new", true}}}), std::ios_base::failure);
    EXPECT_EQ(table.get("key"), "old");
    EXPECT_EQ(SSTable().get("key"), "old");
    EXPECT_TRUE(std::filesystem::is_regular_file("0.sst"));
    EXPECT_FALSE(std::filesystem::exists("1.sst"));
    std::filesystem::remove("2.sst");
    table.flush({{"key", {"new", true}}});
    EXPECT_EQ(SSTable().get("key"), "new");
}

TEST_F(SSTableTest, FailedManifestUpdateKeepsPreviouslyCommittedData) {
    SSTable table;
    table.flush({{"key", {"old", true}}});
    ASSERT_TRUE(std::filesystem::create_directory("MANIFEST.tmp"));
    EXPECT_THROW(table.flush({{"key", {"new", true}}}), std::ios_base::failure);
    EXPECT_EQ(table.get("key"), "old");
    EXPECT_EQ(SSTable().get("key"), "old");
    EXPECT_TRUE(std::filesystem::is_regular_file("0.sst"));
    EXPECT_FALSE(std::filesystem::exists("1.sst"));
    EXPECT_FALSE(std::filesystem::exists("2.sst"));
    std::filesystem::remove("MANIFEST.tmp");
    table.flush({{"key", {"new", true}}});
    EXPECT_EQ(SSTable().get("key"), "new");
}
