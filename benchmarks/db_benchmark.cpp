#include <benchmark/benchmark.h>

#include <filesystem>
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "DB.h"

static std::string prepareBenchmarkPath() {
    const auto directory = std::filesystem::temp_directory_path() / "kvstorage_benchmark";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return (directory / "test").string();
}

static void BM_Put(benchmark::State& state) {
    DB db(prepareBenchmarkPath());

    std::size_t i = 0;
    for (auto _ : state) {
        db.put(std::to_string(i++), "value");
    }
}

static void BM_Compaction(benchmark::State& state) {
    DB db(prepareBenchmarkPath(), std::make_shared<RegularFS>(), 1);

    std::size_t i = 0;
    for (auto _ : state) {
        db.put(std::to_string(i++), "value");
    }
}

static void BM_GetMemtable(benchmark::State& state) {
    DB db(prepareBenchmarkPath(), std::make_shared<RegularFS>(), 1'000'01);

    std::vector<std::string> keys;
    for (int i = 0; i < 1'000'00; ++i) {
        keys.push_back(std::to_string(i));
        db.put(keys.back(), "value");
    }

    std::mt19937 mt(42);
    std::shuffle(keys.begin(), keys.end(), mt);

    size_t i = 0;
    for (auto _ : state) {
        auto value = db.get(keys[i++ % keys.size()]);
        benchmark::DoNotOptimize(value);
    }
}

static void BM_GetSSTable(benchmark::State& state) {
    DB db(prepareBenchmarkPath(), std::make_shared<RegularFS>(), 10'000);

    std::vector<std::string> keys;
    for (int i = 0; i < 1'000'00; ++i) {
        keys.push_back(std::to_string(i));
        db.put(keys.back(), "value");
    }

    std::mt19937 mt(42);
    std::shuffle(keys.begin(), keys.end(), mt);

    size_t i = 0;
    for (auto _ : state) {
        auto value = db.get(keys[i++ % keys.size()]);
        benchmark::DoNotOptimize(value);
    }
}

static void BM_GetMiss(benchmark::State& state) {
    DB db(prepareBenchmarkPath(), std::make_shared<RegularFS>(), 10'000);

    std::vector<std::string> keys;
    for (int i = 0; i < 1'000'00; ++i) {
        keys.push_back(std::to_string(i));
        db.put(keys.back(), "value");
        keys.back().push_back('a');
    }

    std::mt19937 mt(42);
    std::shuffle(keys.begin(), keys.end(), mt);

    size_t i = 0;
    for (auto _ : state) {
        auto value = db.get(keys[i++ % keys.size()]);
        benchmark::DoNotOptimize(value);
    }
}

// Populate once; measure only lookups and writing their results.
// Each reader owns a disjoint result range; no writes occur during reads.
template <std::size_t ThreadCount>
static void BM_DBRead(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    // Keep all entries in the memtable to compare concurrent DB::get calls.
    DB db(prepareBenchmarkPath(), std::make_shared<RegularFS>(), count + 1);
    std::vector<std::string> keys(count);
    std::vector<std::optional<std::string>> expected(count), results(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys[i] = std::to_string(i);
        db.put(keys[i], keys[i]);
    }
    std::mt19937 random(42);
    std::shuffle(keys.begin(), keys.end(), random);
    for (std::size_t i = 0; i < count; ++i) {
        expected[i] = keys[i];
    }
    const auto read = [&](std::size_t part) {
        for (auto i = count * part / ThreadCount;
             i < count * (part + 1) / ThreadCount; ++i) {
            results[i] = db.get(keys[i]);
        }
    };

    // Keep workers alive across iterations to exclude thread creation.
    std::barrier start(static_cast<std::ptrdiff_t>(ThreadCount + 1));
    std::barrier finish(static_cast<std::ptrdiff_t>(ThreadCount + 1));
    std::array<std::jthread, ThreadCount == 1 ? 0 : ThreadCount> workers;
    if constexpr (ThreadCount > 1) {
        for (std::size_t part = 0; part < ThreadCount; ++part) {
            workers[part] = std::jthread([&, part](std::stop_token token) {
                for (;;) {
                    start.arrive_and_wait();
                    if (token.stop_requested()) return;
                    read(part);
                    finish.arrive_and_wait();
                }
            });
        }
    }

    for (auto _ : state) {
        if constexpr (ThreadCount == 1) {
            read(0);
        } else {
            start.arrive_and_wait();
            finish.arrive_and_wait();
        }
        state.PauseTiming();
        const bool correct = results == expected;
        state.ResumeTiming();
        if (!correct) {
            state.SkipWithError("DB read results differ from reference");
            break;
        }
    }
    if constexpr (ThreadCount > 1) {
        for (auto& worker : workers) {
            worker.request_stop();
        }
        start.arrive_and_wait();
    }
    state.SetItemsProcessed(state.iterations() * count);
}
static constexpr std::size_t PutGetIterations = 10;

template <std::size_t PutThreads, std::size_t GetThreads>
static void BMPutGet(benchmark::State& state) {
    constexpr auto ThreadCount = PutThreads + GetThreads;
    static_assert(ThreadCount > 0);
    const auto count = static_cast<std::size_t>(state.range(0));
    DB db(prepareBenchmarkPath());
    std::vector<std::string> keys(count);
    std::vector<std::optional<std::string>> results(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys[i] = "key" + std::to_string(i);
        db.put(keys[i], keys[i]);
    }

    std::barrier start(static_cast<std::ptrdiff_t>(ThreadCount + 1));
    std::barrier finish(static_cast<std::ptrdiff_t>(ThreadCount + 1));
    std::array<std::vector<double>, GetThreads> getTimes;
    for (std::size_t reader = 0; reader < GetThreads; ++reader) {
        const auto part = PutThreads + reader;
        const auto size = count * (part + 1) / ThreadCount - count * part / ThreadCount;
        getTimes[reader].reserve(size * PutGetIterations);
    }
    std::array<std::jthread, ThreadCount> workers;
    for (std::size_t part = 0; part < ThreadCount; ++part) {
        workers[part] = std::jthread([&, part](std::stop_token token) {
            const auto begin = count * part / ThreadCount;
            const auto end = count * (part + 1) / ThreadCount;
            // The initial keys occupy [0, count); each batch gets a new range.
            std::size_t batch = 1;
            for (;;) {
                start.arrive_and_wait();
                if (token.stop_requested()) return;
                for (auto i = begin; i < end; ++i) {
                    if (part < PutThreads) {
                        const auto key = "key" + std::to_string(batch * count + i);
                        db.put(key, key);
                    } else {
                        const auto started = std::chrono::steady_clock::now();
                        auto value = db.get(keys[i]);
                        const auto finished = std::chrono::steady_clock::now();
                        getTimes[part - PutThreads].push_back(
                            std::chrono::duration<double, std::nano>(finished - started).count());
                        results[i] = std::move(value);
                    }
                }
                ++batch;
                finish.arrive_and_wait();
            }
        });
    }

    for (auto _ : state) {
        start.arrive_and_wait();
        finish.arrive_and_wait();
        state.PauseTiming();
        bool correct = true;
        for (auto i = count * PutThreads / ThreadCount; i < count; ++i) {
            if (results[i] != keys[i]) {
                correct = false;
                break;
            }
        }
        state.ResumeTiming();
        if (!correct) {
            state.SkipWithError("DB get results differ from expected values");
            break;
        }
    }
    for (auto& worker : workers) {
        worker.request_stop();
    }
    start.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }

    std::vector<double> times;
    std::size_t sampleCount = 0;
    for (const auto& samples : getTimes) sampleCount += samples.size();
    times.reserve(sampleCount);
    for (const auto& samples : getTimes) {
        times.insert(times.end(), samples.begin(), samples.end());
    }
    std::sort(times.begin(), times.end());
    if (!times.empty()) {
        // Nearest-rank percentiles across all get calls and iterations.
        const auto percentile = [&](std::size_t percent) {
            return times[(times.size() * percent + 99) / 100 - 1];
        };
        state.counters["get_p50_ns"] = percentile(50);
        state.counters["get_p95_ns"] = percentile(95);
        state.counters["get_p99_ns"] = percentile(99);
    }
    state.SetItemsProcessed(state.iterations() * count);
}

BENCHMARK_TEMPLATE(BMPutGet, 1, 1)->Arg(100'000)->Iterations(PutGetIterations)->UseRealTime();
BENCHMARK_TEMPLATE(BMPutGet, 4, 4)->Arg(100'000)->Iterations(PutGetIterations)->UseRealTime();
BENCHMARK_TEMPLATE(BMPutGet, 1, 7)->Arg(100'000)->Iterations(PutGetIterations)->UseRealTime();
BENCHMARK_TEMPLATE(BMPutGet, 7, 1)->Arg(100'000)->Iterations(PutGetIterations)->UseRealTime();

BENCHMARK_TEMPLATE(BM_DBRead, 1)->Arg(100'000)->Arg(1'000'000)->UseRealTime();
BENCHMARK_TEMPLATE(BM_DBRead, 8)->Arg(100'000)->Arg(1'000'000)->UseRealTime();

BENCHMARK(BM_Put)
    ->Iterations(1000);

BENCHMARK(BM_Compaction)
    ->Iterations(1000);

BENCHMARK(BM_GetMemtable);

BENCHMARK(BM_GetSSTable);

BENCHMARK(BM_GetMiss);

BENCHMARK_MAIN();
