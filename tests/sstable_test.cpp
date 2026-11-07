//
// Created by oleg on 10/16/26.
//

#include "sstable.h"

#include "gtest/gtest.h"

TEST(SSTable, FlushTwoEntryMaps) {
    const auto fs = std::make_shared<MemFS>();

    const SSTable::EntryMap older = {
        {"apple", "red"},
        {"cherry", "red"},
        {"pear", "green"},
    };
    const SSTable::EntryMap newer = {
        {"banana", "yellow"},
        {"cherry", "dark red"},
        {"plum", "purple"},
    };
    const std::map<std::string, std::string> expected = {
        {"apple", "red"},
        {"banana", "yellow"},
        {"cherry", "dark red"},
        {"pear", "green"},
        {"plum", "purple"},
    };

    SSTable ss_table(fs);
    ss_table.flush(older);
    ss_table.flush(newer);

    EXPECT_EQ(ss_table.dump(), expected);
    auto dir_content = fs->list(".");
    EXPECT_EQ(fs->list("."), (std::vector<std::string>{"2.sst", "MANIFEST"}));
}

TEST(SSTable, FlushCrashConsistent) {
    int i = 1;
    while (1) {
        const auto fs = std::make_shared<MemFS>();

        const SSTable::EntryMap a = {
            {"a", "1"},
            {"c", "1"},
            {"d", "2"},
        };
        const SSTable::EntryMap b = {
            {"b", "3"},
            {"c", "4"},
            {"a", "5"},
        };

        auto check_old_state = [&]() {
            const std::map<std::string, std::string> expected = {
                {"a", "1"},
                {"c", "1"},
                {"d", "2"},
            };
            SSTable sst(fs);
            return sst.dump() == expected;
        };
        auto check_new_state = [&]() {
            const std::map<std::string, std::string> expected = {
                {"a", "5"},
                {"b", "3"},
                {"c", "4"},
                {"d", "2"},
            };
            SSTable sst(fs);
            return sst.dump() == expected;
        };
        SSTable ss_table(fs);
        ss_table.flush(a);
        fs->setSteps(i);
        try {
            ss_table.flush(b);
            fs->crash();
            ASSERT_TRUE(check_new_state());
            break;
        } catch (...) {
            ASSERT_TRUE(check_old_state() || check_new_state());
        }
        ++i;
    }
}

TEST(SSTable, Get) {
    const auto fs = std::make_shared<MemFS>();

    SSTable ss_table(fs);

    ss_table.flush({
        {"a", "old"},
        {"b", "value"},
        {"c", ""},
    });

    ss_table.flush({
        {"a", "new"},
        {"b", std::nullopt},
    });

    ss_table.flush({
        {"a", std::nullopt},
        {"d", "value"},
    });

    EXPECT_EQ(ss_table.get("a"), std::nullopt);
    EXPECT_EQ(ss_table.get("b"), std::nullopt);
    EXPECT_EQ(ss_table.get("c"), "");
    EXPECT_EQ(ss_table.get("d"), "value");
    EXPECT_EQ(ss_table.get("unknown"), std::nullopt);

    // Проверка границ бинарного поиска
    EXPECT_EQ(ss_table.get("0"), std::nullopt); // до первого ключа
    EXPECT_EQ(ss_table.get("aa"), std::nullopt); // между ключами
    EXPECT_EQ(ss_table.get("z"), std::nullopt); // после последнего ключа

    // Проверка первого и последнего ключей отдельного файла
    const auto fs2 = std::make_shared<MemFS>();
    SSTable single_table(fs2);

    single_table.flush({
        {"b", "1"},
        {"d", "2"},
        {"f", "3"},
        {"h", "4"},
        {"j", "5"},
    });

    EXPECT_EQ(single_table.get("b"), "1");
    EXPECT_EQ(single_table.get("f"), "3");
    EXPECT_EQ(single_table.get("j"), "5");

    EXPECT_EQ(single_table.get("a"), std::nullopt);
    EXPECT_EQ(single_table.get("c"), std::nullopt);
    EXPECT_EQ(single_table.get("z"), std::nullopt);
}
