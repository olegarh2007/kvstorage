//
// Created by oleg on 10/4/26.
//

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "DB.h"

namespace {

struct Operation {
    bool put;
    std::string key;
    std::string value;
};

class TemporaryDirectory {
public:
    TemporaryDirectory()
        : path(std::filesystem::temp_directory_path() /
               ("kvstorage_benchmark_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("Failed to create benchmark directory");
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

void BM_Integrated(benchmark::State& state) {
    const auto operationCount = static_cast<std::size_t>(state.range(0));
    std::mt19937 random(42);
    std::bernoulli_distribution choosePut(0.75);
    std::uniform_int_distribution<int> chooseKey(0, 999);
    std::vector<Operation> operations;
    operations.reserve(operationCount);

    std::size_t putCount = 0;
    for (std::size_t i = 0; i < operationCount; ++i) {
        const bool put = choosePut(random);
        operations.push_back({put,
                              "key_" + std::to_string(chooseKey(random)),
                              put ? "value_" + std::to_string(i) : std::string{}});
        putCount += put;
    }

    TemporaryDirectory directory;
    const auto dbPath = (directory.path / "benchmark.db").string();

    for (auto _ : state) {
        state.PauseTiming();
        std::filesystem::remove(dbPath + ".snapshot");
        std::filesystem::remove(dbPath + ".wal");
        {
            DB db(dbPath);
            state.ResumeTiming();

            for (const auto& operation : operations) {
                if (operation.put) {
                    db.put(operation.key, operation.value);
                } else {
                    db.remove(operation.key);
                }
            }

            state.PauseTiming();
        }
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(operationCount));
    state.counters["puts"] = static_cast<double>(putCount);
    state.counters["deletes"] = static_cast<double>(operationCount - putCount);
}

BENCHMARK(BM_Integrated)->Arg(1000000)->UseRealTime()->Unit(benchmark::kMillisecond);

}  // namespace
