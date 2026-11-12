//
// Created by oleg on 11/12/26.
//
#include <array>
#include <barrier>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "DB.h"
#include "gtest/gtest.h"
//
// Created by oleg on 11/12/26.
//
namespace {
constexpr std::size_t MEMTABLE_MAX_SIZE = 3;
}

TEST(DBConcurrencyTest, ConcurrentPuts) {
    auto fs = std::make_shared<MemFS>();
    DB db("test", fs, MEMTABLE_MAX_SIZE);

    std::barrier sync(8);
    auto test_put = [&](std::string_view code, size_t count){
        sync.arrive_and_wait();

        for (size_t i = 0; i < count; ++i) {
            auto key = "key" + std::string(code) + std::to_string(i);
            auto value = "value" + std::string(code) + std::to_string(i);
            db.put(key, value);
        }
    };
    {
        std::jthread th1(test_put, "a", 100);
        std::jthread th2(test_put, "b", 100);
        std::jthread th3(test_put, "c", 100);
        std::jthread th4(test_put, "d", 100);
        std::jthread th5(test_put, "e", 100);
        std::jthread th6(test_put, "f", 100);
        std::jthread th7(test_put, "g", 100);
        std::jthread th8(test_put, "h", 100);
    }
    std::map<std::string, std::string> expected;
    for (char code = 'a'; code <= 'h'; ++code) {
        for (size_t i = 0; i < 100; ++i) {
            expected.emplace("key" + std::string(1, code) + std::to_string(i),
                             "value" + std::string(1, code) + std::to_string(i));
        }
    }
    EXPECT_EQ(db.dump(), expected);
}

TEST(DBConcurrencyTest, ConcurrentGets) {
    auto fs = std::make_shared<MemFS>();
    DB db("test", fs, MEMTABLE_MAX_SIZE);
    std::map<std::string, std::string> expected;
    for (char code = 'a'; code <= 'h'; ++code) {
        for (size_t i = 0; i < 100; ++i) {
            auto k = "key" + std::string(1, code) + std::to_string(i);
            auto v = "value" + std::string(1, code) + std::to_string(i);
            db.put(k, v);
            expected.emplace(k, v);
        }
    }
    std::barrier sync(8);
    auto test_get = [&](std::string_view code, size_t count){
        sync.arrive_and_wait();

        for (const auto &[k,v]:expected) {
            EXPECT_EQ(db.get(k), v);
        }
    };
    {
        std::jthread th1(test_get, "a", 100);
        std::jthread th2(test_get, "b", 100);
        std::jthread th3(test_get, "c", 100);
        std::jthread th4(test_get, "d", 100);
        std::jthread th5(test_get, "e", 100);
        std::jthread th6(test_get, "f", 100);
        std::jthread th7(test_get, "g", 100);
        std::jthread th8(test_get, "h", 100);
    }
}

TEST(DBConcurrencyTest, ConcurrentPutGet) {
    auto fs = std::make_shared<MemFS>();
    DB db("test", fs, MEMTABLE_MAX_SIZE);
    std::array<std::vector<std::pair<std::string, std::string>>, 8> keys;
    for (char code = 'a'; code <= 'h'; ++code) {
        auto &group_keys = keys[code - 'a'];
        group_keys.reserve(1000);
        for (size_t i = 0; i < 1000; ++i) {
            auto k = "key" + std::string(1, code) + std::to_string(i);
            auto v = "value" + std::string(1, code) + std::to_string(i);
            group_keys.emplace_back(k, v);
        }
    }

    std::barrier sync(8);
    {
        std::array<std::jthread, 8> threads;
        for (size_t group = 0; group < keys.size(); ++group) {
            threads[group] = std::jthread([&, group] {
                const auto &entries = keys[group];
                std::map<std::string, std::string> expected;
                sync.arrive_and_wait();

                for (size_t i = 0; i < entries.size(); ++i) {
                    const auto &[put_key, put_value] = entries[i];
                    db.put(put_key, put_value);
                    expected[put_key] = put_value;

                    const auto &get_key = entries[entries.size() - 1 - i].first;
                    const auto found = expected.find(get_key);
                    const auto expected_value = found == expected.end()
                        ? std::optional<std::string>{}
                        : std::optional<std::string>{found->second};
                    EXPECT_EQ(db.get(get_key), expected_value) << "key: " << get_key;
                }

                for (const auto &[key, value] : expected) {
                    EXPECT_EQ(db.get(key), value) << "key: " << key;
                }
            });
        }
    }
}
