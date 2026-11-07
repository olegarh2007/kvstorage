#include <benchmark/benchmark.h>

#include <filesystem>
#include <random>

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

static void BM_Get(benchmark::State& state) {
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

BENCHMARK(BM_Put)
    ->Iterations(1000)
    ->UseRealTime();

BENCHMARK(BM_Get)
    ->UseRealTime();

BENCHMARK(BM_Get);

BENCHMARK_MAIN();
