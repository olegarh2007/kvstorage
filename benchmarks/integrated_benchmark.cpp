#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "DB.h"

namespace {

constexpr std::size_t kOperationCount = 50'000;
constexpr std::size_t kOldKeyCount = 1'000;
constexpr std::size_t kWriteCount = kOperationCount / 5;

// SSTable uses relative paths for MANIFEST and its files, so isolate the entire cwd.
class TemporaryDirectory {
public:
    TemporaryDirectory()
        : originalPath(std::filesystem::current_path()),
          path(std::filesystem::temp_directory_path() /
               ("kvstorage_benchmark_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        if (!std::filesystem::create_directory(path)) {
            throw std::runtime_error("Failed to create benchmark directory");
        }
        try {
            std::filesystem::current_path(path);
        } catch (...) {
            std::filesystem::remove_all(path);
            throw;
        }
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::current_path(originalPath, error);
        std::filesystem::remove_all(path, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

private:
    std::filesystem::path originalPath;
    std::filesystem::path path;
};

enum class Scenario { PutNew, PutUpdate, Delete, GetExisting, GetMissing, GetOld, Mixed };

template <Scenario scenario>
void Run(benchmark::State& state) {
    // Generate keys and the deterministic access order outside the timed region.
    std::vector<std::string> keys;
    keys.reserve(kOperationCount);
    for (std::size_t i = 0; i < kOperationCount; ++i) {
        keys.push_back("key_" + std::to_string(i));
    }
    std::mt19937 random(42);
    std::vector<std::size_t> accesses(kOperationCount);
    if constexpr (scenario == Scenario::GetExisting || scenario == Scenario::GetOld ||
                  scenario == Scenario::Mixed) {
        const auto keyCount = scenario == Scenario::GetOld ? kOldKeyCount : kOperationCount;
        std::uniform_int_distribution<std::size_t> chooseKey(0, keyCount - 1);
        for (auto& index : accesses) {
            index = chooseKey(random);
        }
    } else {
        // Visit every key once: new inserts stay new, and deletes stay existing.
        std::iota(accesses.begin(), accesses.end(), 0);
        std::shuffle(accesses.begin(), accesses.end(), random);
    }
    std::vector<std::uint8_t> writes(kOperationCount, 0);
    if constexpr (scenario == Scenario::Mixed) {
        std::fill_n(writes.begin(), kWriteCount, 1);
        std::shuffle(writes.begin(), writes.end(), random);
    }
    const std::string initialValue(64, 'a');
    const std::string updatedValue(64, 'b');

    for (auto _ : state) {
        state.PauseTiming();
        {
            TemporaryDirectory directory;
            DB db("benchmark.db");
            if constexpr (scenario != Scenario::PutNew) {
                // Missing lookups also use a populated database, with disjoint keys.
                // GetOld queries only the first 1,000 keys after 9,000 later inserts.
                for (const auto& key : keys) {
                    db.put(key, initialValue);
                }
            }
            std::vector<std::string> missingKeys;
            if constexpr (scenario == Scenario::GetMissing) {
                missingKeys.reserve(kOperationCount);
                for (std::size_t i = 0; i < kOperationCount; ++i) {
                    // Interleave with existing keys to exercise SSTable searches.
                    missingKeys.push_back(keys[i] + "_missing");
                }
            }
            state.ResumeTiming();

            for (std::size_t i = 0; i < kOperationCount; ++i) {
                const auto& key = keys[accesses[i]];
                if constexpr (scenario == Scenario::PutNew) {
                    db.put(key, initialValue);
                } else if constexpr (scenario == Scenario::PutUpdate) {
                    db.put(key, updatedValue);
                } else if constexpr (scenario == Scenario::Delete) {
                    db.remove(key);
                } else if constexpr (scenario == Scenario::GetMissing) {
                    benchmark::DoNotOptimize(db.get(missingKeys[accesses[i]]));
                } else if constexpr (scenario == Scenario::Mixed) {
                    if (writes[i]) {
                        db.put(key, updatedValue);
                    } else {
                        benchmark::DoNotOptimize(db.get(key));
                    }
                } else {
                    benchmark::DoNotOptimize(db.get(key));
                }
            }

            state.PauseTiming();
        }
        state.ResumeTiming();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kOperationCount));
}

void Put_New(benchmark::State& state) {
    Run<Scenario::PutNew>(state);
}
void Put_Update(benchmark::State& state) {
    Run<Scenario::PutUpdate>(state);
}
void Delete(benchmark::State& state) {
    Run<Scenario::Delete>(state);
}
void Get_Existing(benchmark::State& state) {
    Run<Scenario::GetExisting>(state);
}
void Get_Missing(benchmark::State& state) {
    Run<Scenario::GetMissing>(state);
}
void Get_Old(benchmark::State& state) {
    Run<Scenario::GetOld>(state);
}
void Mixed(benchmark::State& state) {
    Run<Scenario::Mixed>(state);
}

// One batch per scenario keeps the default run bounded at 10,000 measured operations.
BENCHMARK(Put_New)->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(Put_Update)->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(Delete)->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(Get_Existing)->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(Get_Missing)->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(Get_Old)->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);
BENCHMARK(Mixed)->Iterations(1)->UseRealTime()->Unit(benchmark::kMillisecond);

}  // namespace
