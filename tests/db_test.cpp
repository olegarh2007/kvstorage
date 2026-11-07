//
// Created by oleg on 11/6/26.
//

#include "DB.h"

#include "gtest/gtest.h"

TEST(db, crash_cons_test) {
    auto check_old = [](const std::shared_ptr<MemFS>& fs) {
        DB db("test", fs);
        const std::map<std::string, std::string> expected = {{"a", "b"}};
        return db.dump() == expected;
    };
    auto check_new = [](const std::shared_ptr<MemFS>& fs) {
        DB db("test", fs);
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
        DB db("test", fs);
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
        while (!test(i, DB::MAX_WAL_SIZE - 1)) {
            ++i;
        }
    }
    {
        int i = 0;
        while (!test(i, DB::MAX_WAL_SIZE)) {
            ++i;
        }
    }
}


