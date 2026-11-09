
#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>

constexpr size_t N = 1 << 14;

using Data = std::array<uint8_t, N>;

#if defined(__GNUC__) || defined(__clang__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

NOINLINE uint64_t BuyPath(uint32_t i) noexcept {
    return uint64_t(i) * 3 + 17;
}

NOINLINE uint64_t SellPath(uint32_t i) noexcept {
    return uint64_t(i) * 7 + 31;
}

const Data& PredictableData() {
    static const Data data = [] {
        Data d{};

        std::fill(d.begin() + N / 2, d.end(), 1);

        return d;
    }();

    return data;
}

const Data& RandomData() {
    static const Data data = [] {
        Data d{};

        std::fill(d.begin() + N / 2, d.end(), 1);

        std::mt19937 rng(12345);
        std::shuffle(d.begin(), d.end(), rng);

        return d;
    }();

    return data;
}

NOINLINE uint64_t Process(const Data& data) noexcept {
    uint64_t result = 0;

    for (uint32_t i = 0; i < N; ++i) {
        if (data[i]) {
            result += BuyPath(i);
        } else {
            result += SellPath(i);
        }
    }

    return result;
}

static void BM_Predictable(benchmark::State& state) {
    const auto& data = PredictableData();

    for (auto _ : state) {
        auto result = Process(data);

        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(
        state.iterations() * N
    );
}

static void BM_Random(benchmark::State& state) {
    const auto& data = RandomData();

    for (auto _ : state) {
        auto result = Process(data);

        benchmark::DoNotOptimize(result);
    }

    state.SetItemsProcessed(
        state.iterations() * N
    );
}

BENCHMARK(BM_Predictable);
BENCHMARK(BM_Random);
