#include <benchmark/benchmark.h>
#include <pthread.h>
#include <sched.h>
#include <memory>
#include <thread>

#include "helpers.h"
#include "source/SPSCQueueEx.h"

// ------------------------------------------
// UTILS
// ------------------------------------------

static int CoreA()
{
	static int c = getenv("BENCH_CORE_A") ? atoi(getenv("BENCH_CORE_A")) : 2;
	return c;
}

static int CoreB()
{
	static int c = getenv("BENCH_CORE_B") ? atoi(getenv("BENCH_CORE_B")) : 3;
	return c;
}

static void Pin(int core)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(core, &set);
	if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
		std::abort();
}

template<typename T, std::size_t Capacity>
struct QueueFactory
{
	using Queue = SPSCQueue< T, Capacity >;

	static std::unique_ptr< Queue > Create()
	{
		return std::make_unique< Queue >();
	}
};

template<typename T, std::size_t Capacity>
struct QueueExFactory
{
	using Queue = SPSCQueueEx< T >;

	static std::unique_ptr< Queue > Create()
	{
		return std::make_unique< Queue >(Capacity);
	}
};

template<template <typename, std::size_t> typename Factory, typename T, std::size_t Capacity>
struct Shared
{
	static inline std::unique_ptr< typename Factory< T, Capacity >::Queue > q;
};

// ------------------------------------------
// BENCHMARKS
// ------------------------------------------

template<template <typename, std::size_t> typename Factory, typename T, std::size_t Capacity>
void BM_PushPop_SingleThread(benchmark::State &state)
{
	using S = Shared< Factory, T, Capacity >;
	Pin(CoreA());

	// заполняем наполовину: так индексы "ходят" по всему буферу
	for (std::size_t i = 0; i < Capacity / 2; ++i)
		S::q->TryEmplace(MakeValue< T >(i));

	T in = MakeValue< T >(1);

	for (auto _ : state)
	{
		benchmark::DoNotOptimize(in);
		bool pushed = S::q->TryEmplace(in);
		const T *p = S::q->Front();
		benchmark::DoNotOptimize(pushed);
		benchmark::DoNotOptimize(p);
		if (p)
			benchmark::DoNotOptimize(*p);
		S::q->Pop();
	}
	state.SetItemsProcessed(state.iterations());
}

template<template <typename, std::size_t> typename Factory, typename T, std::size_t Capacity>
void BM_Throughput_2T(benchmark::State &state)
{
	using S = Shared< Factory, T, Capacity >;
	const bool producer = state.thread_index() == 0;
	Pin(producer ? CoreA() : CoreB());

	if (producer)
	{
		std::uint64_t seq = 0;
		for (auto _ : state)
		{
			T v = MakeValue< T >(seq++);
			while (!S::q->TryEmplace(v))
			{
				/* spin */
			}
		}
	}
	else
	{
		for (auto _ : state)
		{
			const T *p;
			while ((p = S::q->Front()) == nullptr)
			{
				/* spin */
			}
			benchmark::DoNotOptimize(*p);
			S::q->Pop();
		}
	}
	state.SetItemsProcessed(state.iterations());
}

// ------------------------------------------
// BENCHMARK REGISTRATION
// ------------------------------------------

constexpr std::size_t kCap = 1024;

static void Cfg(benchmark::internal::Benchmark* b)
{
	b->Repetitions(3)                 // 30 независимых прогонов
	 ->MinWarmUpTime(1.0)             // прогрев 1 с перед каждым прогоном
	 // ->MinTime(2.0)                   // каждый прогон минимум 2 с
	 ->ReportAggregatesOnly(true);    // в файл/консоль только агрегаты
	 // ->DisplayAggregatesOnly(true)
	 // ->ComputeStatistics("min", [](const std::vector<double>& v) {
		//  return *std::min_element(v.begin(), v.end());
	 // })
	 // ->ComputeStatistics("p95", [](const std::vector<double>& v) {
		//  std::vector<double> s(v);
		//  std::sort(s.begin(), s.end());
		//  return s[static_cast<std::size_t>(s.size() * 0.95)];
	 // });
}

// ---- single thread ----
#define REG_1T(F, T) \
	BENCHMARK_TEMPLATE(BM_PushPop_SingleThread, F, T, kCap) \
		->Setup([](const benchmark::State&) { Shared<F, T, kCap>::q = F<T, kCap>::Create(); }) \
		->Teardown([](const benchmark::State&) { Shared<F, T, kCap>::q.reset(); }) \
		// ->Apply(Cfg)

REG_1T(QueueFactory,   U64);
REG_1T(QueueFactory,   P1);
REG_1T(QueueFactory,   P4);
REG_1T(QueueExFactory, U64);
REG_1T(QueueExFactory, P1);
REG_1T(QueueExFactory, P4);

// ---- two threads (producer + consumer) ----
#define REG_2T(F, T) \
	BENCHMARK_TEMPLATE(BM_Throughput_2T, F, T, kCap) \
		->Threads(2) \
		->UseRealTime() \
		->Setup([](const benchmark::State&) { Shared<F, T, kCap>::q = F<T, kCap>::Create(); }) \
		->Teardown([](const benchmark::State&) { Shared<F, T, kCap>::q.reset(); }) \
		// ->Apply(Cfg)

REG_2T(QueueFactory,   U64);
REG_2T(QueueFactory,   P1);
REG_2T(QueueFactory,   P4);
REG_2T(QueueExFactory, U64);
REG_2T(QueueExFactory, P1);
REG_2T(QueueExFactory, P4);

BENCHMARK_MAIN();
