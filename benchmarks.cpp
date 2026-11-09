#include <benchmark/benchmark.h>
#include <thread>

#include "helpers.h"


template <typename T, std::size_t Capacity>
void BM_PushPop_SingleThread(benchmark::State& state)
{
    SPSCQueue<T, Capacity> q;
    const T in = MakeValue<T>(1);
    T out{};

    for (auto _ : state)
    {
        const bool pushed = q.TryPush(in);
        const bool popped = q.TryPop(out);
        benchmark::DoNotOptimize(out);
        if (!pushed || !popped) [[unlikely]]
        {
            state.SkipWithError("push/pop failed on empty queue");
            break;
        }
    }

    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(sizeof(T)));
}

BENCHMARK_TEMPLATE2(BM_PushPop_SingleThread, U64, 1024);
BENCHMARK_TEMPLATE2(BM_PushPop_SingleThread, P1, 1024);
BENCHMARK_TEMPLATE2(BM_PushPop_SingleThread, P4, 1024);

BENCHMARK_MAIN();
