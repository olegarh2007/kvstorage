//
// Created by oleg on 11/6/26.
//

#include "DB.h"

#include <random>

#include "gtest/gtest.h"

namespace {
constexpr std::size_t MEMTABLE_MAX_SIZE = 3;
}

TEST(db, Put) {
    auto check_old = [](const std::shared_ptr<MemFS>& fs) {
        DB db("test", fs, MEMTABLE_MAX_SIZE);
        const std::map<std::string, std::string> expected = {{"a", "b"}};
        return db.dump() == expected;
    };
    auto check_new = [](const std::shared_ptr<MemFS>& fs) {
        DB db("test", fs, MEMTABLE_MAX_SIZE);
        const std::map<std::string, std::string> expected = {{"a", "b"}, {"c", "d"}};
        return db.dump() == expected;
    };
    auto check_old_db = [](const DB& db) {
        const std::map<std::string, std::string> expected = {{"a", "b"}};
        return db.dump() == expected;
    };
    auto check_new_db = [](const DB& db) {
        const std::map<std::string, std::string> expected = {{"a", "b"}, {"c", "d"}};
        return db.dump() == expected;
    };
    auto test = [&](int i, int put_count) {
        auto fs = std::make_shared<MemFS>();
        DB db("test", fs, MEMTABLE_MAX_SIZE);
        for (int j = 0; j < put_count; ++j) {
            db.put("a", "b");
        }
        fs->setSteps(i);
        bool failed = false;
        try {
            db.put("c", "d");
        } catch (...) {
            failed = true;
        }
        if (failed) {
            EXPECT_TRUE(check_old(fs) || check_new(fs));
            return false;
        }
        EXPECT_TRUE(check_new_db(db));
        fs->crash();
        EXPECT_TRUE(check_new(fs));
        return true;
    };
    {
        int i = 0;
        // testing put not overlows wal
        while (!test(i, MEMTABLE_MAX_SIZE - 2)) {
            ++i;
        }
    }
    {
        int i = 0;
        // testing put overflows wal
        while (!test(i, MEMTABLE_MAX_SIZE - 1)) {
            ++i;
        }
    }
}

TEST(db, GetMemtable) {
    auto fs = std::make_shared<MemFS>();
    DB db("test", fs);
    db.put("a", "b");
    EXPECT_EQ(db.get("a"), "b");
}

TEST(db, GetWal) {
    auto fs = std::make_shared<MemFS>();
    {
        DB db("test", fs);
        db.put("a", "b");
    }
    fs->crash();
    DB db("test", fs);
    EXPECT_EQ(db.get("a"), "b");
}

TEST(db, GetSSTable) {
    auto fs = std::make_shared<MemFS>();
    DB db("test", fs, MEMTABLE_MAX_SIZE);
    for (int i = 0; i < MEMTABLE_MAX_SIZE; ++i) {
        db.put("a", "b");
    }
    EXPECT_EQ(db.get("a"), "b");
}

// тест что значение из memtable перекрывает значение из sstable
TEST(db, GetSSTablePriority) {
    auto fs = std::make_shared<MemFS>();
    DB db("test", fs, MEMTABLE_MAX_SIZE);
    for (int i = 0; i < MEMTABLE_MAX_SIZE; ++i) {
        db.put("a", "old");
    }
    db.put("a", "new");
    EXPECT_EQ(db.get("a"), "new");

    db.put("a", std::nullopt);
    EXPECT_EQ(db.get("a"), std::nullopt);
}

TEST(db, Randomized) {
    std::mt19937 mt(42);
    auto fs = std::make_shared<MemFS>();
    std::map<std::string, std::string> expected;

    auto random_string = [&](int n) {
        std::string s;
        for (int i = 0; i < n; ++i) {
            s += 'a' + mt() % 8;
        }
        return s;
    };

    for (int i = 0; i < 10; ++i) {
        {
            DB db("test", fs, MEMTABLE_MAX_SIZE);

            for (int j = 0; j < 100; ++j) {
                auto key = random_string(2);
                int op = mt() % 3;

                if (op == 0) {
                    auto value = random_string(mt() % 10);
                    db.put(key, value);
                    expected[key] = value;
                } else if (op == 1) {
                    db.put(key, std::nullopt);
                    expected.erase(key);
                } else {
                    auto it = expected.find(key);
                    EXPECT_EQ(db.get(key), it == expected.end()
                        ? std::nullopt : std::make_optional(it->second));
                }
            }

            EXPECT_EQ(db.dump(), expected);
        }

        fs->crash();

        DB restored("test", fs, MEMTABLE_MAX_SIZE);
        EXPECT_EQ(restored.dump(), expected);
    }
}

TEST(db, PathIsolatesPersistentFiles) {
    auto fs = std::make_shared<MemFS>();
    {
        DB first("/data/first", fs, MEMTABLE_MAX_SIZE);
        DB second("/data/second", fs, MEMTABLE_MAX_SIZE);
        for (std::size_t i = 0; i < MEMTABLE_MAX_SIZE * 2; ++i) {
            first.put("key", "first");
            second.put("key", "second");
        }
        first.put("wal-only", "first");
        second.put("wal-only", "second");
    }
    EXPECT_TRUE(fs->exists("/data/first.MANIFEST"));
    EXPECT_TRUE(fs->exists("/data/second.MANIFEST"));
    EXPECT_TRUE(fs->exists("/data/first.wal"));
    EXPECT_FALSE(fs->exists("MANIFEST"));
    fs->crash();
    DB first("/data/first", fs, MEMTABLE_MAX_SIZE);
    DB second("/data/second", fs, MEMTABLE_MAX_SIZE);
    EXPECT_EQ(first.get("key"), "first");
    EXPECT_EQ(second.get("key"), "second");
    EXPECT_EQ(first.get("wal-only"), "first");
    EXPECT_EQ(second.get("wal-only"), "second");
}
