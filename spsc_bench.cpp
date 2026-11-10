// spsc_bench.cpp - benchmarks for SPSCQueue (+ comparison with SPSCQueueEx, naive SPSC, mutex queue)
//
// Build:
//   g++ -std=c++20 -O3 -march=native -DNDEBUG spsc_bench.cpp -lbenchmark -lpthread -o spsc_bench
//
// Run:
//   ./spsc_bench --spsc_prod_cpu=2 --spsc_cons_cpu=4
//   ./spsc_bench --help_spsc
//
// Adding a queue: write a "Kind" struct (see EyeKind) and add it to AllQueues.

// SPSCQueue.h uses these without including them, so include them first.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "SPSCQueue.h"
#include "source/SPSCQueueEx.h"

#if defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#elif defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#  include <immintrin.h>
#endif

namespace {

using u64 = std::uint64_t;

// =====================================================================
// Options (--spsc_* flags)
// =====================================================================

struct Load { u64 intervalNs; u64 burst; };

struct Options
{
	int prodCpu = -1;                 // -1 = do not pin
	int consCpu = -1;
	u64 items = 1u << 21;             // items per throughput run
	u64 work = 300;                   // busy-loop units in the slow_* scenarios
	double latSeconds = 1.0;          // target duration of one latency run
	std::vector<Load> latLoads{ { 1000, 1 }, { 10000, 1 }, { 100000, 1 }, { 20000, 16 } };
	std::vector<std::string> queues;  // empty = all

	bool Enabled(const std::string &name) const
	{
		return queues.empty() || std::find(queues.begin(), queues.end(), name) != queues.end();
	}
};

Options g_opt;

// =====================================================================
// Platform helpers
// =====================================================================

inline void CpuRelax() noexcept
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
	_mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
	asm volatile("yield" ::: "memory");
#else
	std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

inline u64 NowNs() noexcept
{
	return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

void PinThisThread(int cpu)
{
	if (cpu < 0)
		return;
	bool ok = false;
#if defined(__linux__)
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	ok = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
	ok = SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{ 1 } << cpu) != 0;
#endif
	static std::atomic<bool> warned{ false };
	if (!ok && !warned.exchange(true))
		std::fprintf(stderr, "WARNING: failed to pin thread to cpu %d\n", cpu);
}

// Dependent busy loop simulating work (~1-2 cycles per unit).
inline void SpinWork(u64 n) noexcept
{
	for (u64 i = 0; i < n; ++i)
		benchmark::DoNotOptimize(i);
}

double ClockOverheadNs()
{
	constexpr int kN = 100000;
	u64 sum = 0;
	const u64 t0 = NowNs();
	for (int i = 0; i < kN; ++i)
		sum += NowNs();
	const u64 t1 = NowNs();
	benchmark::DoNotOptimize(sum);
	return double(t1 - t0) / kN;
}

// Runs `producer` and `consumer` on two pinned threads started simultaneously.
// Returns wall time (seconds) from the start signal until both threads finished.
template<class A, class B>
double RunPair(A &&producer, B &&consumer)
{
	std::atomic<int> ready{ 0 };
	std::atomic<bool> go{ false };
	auto spawn = [&](int cpu, auto &fn) {
		return std::thread([&, cpu] {
			PinThisThread(cpu);
			ready.fetch_add(1, std::memory_order_acq_rel);
			while (!go.load(std::memory_order_acquire))
				CpuRelax();
			fn();
		});
	};
	std::thread a = spawn(g_opt.prodCpu, producer);
	std::thread b = spawn(g_opt.consCpu, consumer);
	while (ready.load(std::memory_order_acquire) < 2)
		std::this_thread::yield();
	const u64 t0 = NowNs();
	go.store(true, std::memory_order_release);
	a.join();
	b.join();
	return double(NowNs() - t0) * 1e-9;
}

// Publishes "Mops" (million items/s) and "ns/op" for `items` processed in total.
void ReportThroughput(benchmark::State &st, double items)
{
	using C = benchmark::Counter;
	st.counters["Mops"] = C(items * 1e-6, C::kIsRate);
	st.counters["ns/op"] = C(items * 1e-9, C::kIsRate | C::kInvert);
}

// =====================================================================
// Payload types (all expose seq() and touch())
// =====================================================================

template<std::size_t Bytes>
struct Payload
{
	static_assert(Bytes >= 8 && Bytes % 8 == 0);
	static constexpr std::size_t kWords = Bytes / 8;
	u64 w[kWords];

	Payload() noexcept : w{} {}
	explicit Payload(u64 s) noexcept
	{
		for (std::size_t i = 0; i < kWords; ++i)
			w[i] = s + i;
	}
	u64 seq() const noexcept { return w[0]; }
	u64 touch() const noexcept { return w[0] + w[kWords - 1]; }
};

// Non-trivially copyable/destructible type (SSO string, no heap allocation).
struct Str
{
	std::string name;
	u64 id{ 0 };
	Str() = default;
	explicit Str(u64 s) : name("sym-ABCDEFGH"), id(s) {}
	u64 seq() const noexcept { return id; }
	u64 touch() const noexcept { return id + name.size(); }
};

// 64-byte message carrying a send timestamp (latency benchmark).
struct TimedMsg
{
	u64 sendNs{ 0 };
	u64 seq{ 0 };
	u64 pad[6]{};
	TimedMsg() = default;
	TimedMsg(u64 t, u64 s) : sendNs(t), seq(s) {}
};

using P8 = Payload<8>;
using P64 = Payload<64>;
using P256 = Payload<256>;

// =====================================================================
// Queues under test. Required interface: default ctor, TryEmplace(args...), Front(), Pop().
// =====================================================================

// Rigtorp's queue (SPSCQueueEx.h): runtime capacity -> adapt to Queue<T, N>.
// It does not touch its memory on construction, so pre-fault it here, the same way
// SPSCQueue does with touch=true. Otherwise page faults would land in the measured region.
template<typename T, std::size_t N>
class RigtorpQueue : public SPSCQueueEx<T>
{
public:
	RigtorpQueue() : SPSCQueueEx<T>(N)
	{
		for (std::size_t i = 0; i < N; ++i)
			(void)this->TryEmplace();
		// Drain through Front(): it refreshes the internal write-index cache, Pop() alone does not.
		while (this->Front())
			this->Pop();
	}
};

// Textbook lock-free SPSC: no index caching, all fields share one cache line.
template<typename T, std::size_t N>
class NaiveQueue
{
	static_assert(std::has_single_bit(N));
public:
	NaiveQueue() : buf_(std::make_unique<T[]>(N)) {}

	template<typename... A>
	bool TryEmplace(A &&... a)
	{
		const std::size_t w = w_.load(std::memory_order_relaxed);
		if (w - r_.load(std::memory_order_acquire) == N)
			return false;
		buf_[w & (N - 1)] = T(std::forward<A>(a)...);
		w_.store(w + 1, std::memory_order_release);
		return true;
	}
	T *Front()
	{
		const std::size_t r = r_.load(std::memory_order_relaxed);
		return r == w_.load(std::memory_order_acquire) ? nullptr : &buf_[r & (N - 1)];
	}
	void Pop() { r_.store(r_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

private:
	std::atomic<std::size_t> w_{ 0 };
	std::atomic<std::size_t> r_{ 0 };
	std::unique_ptr<T[]> buf_;
};

// std::mutex + std::deque (safe for SPSC: push_back keeps references to existing elements valid).
template<typename T, std::size_t N>
class MutexQueue
{
public:
	template<typename... A>
	bool TryEmplace(A &&... a)
	{
		std::lock_guard<std::mutex> g(m_);
		if (d_.size() >= N)
			return false;
		d_.emplace_back(std::forward<A>(a)...);
		return true;
	}
	T *Front()
	{
		std::lock_guard<std::mutex> g(m_);
		return d_.empty() ? nullptr : &d_.front();
	}
	void Pop()
	{
		std::lock_guard<std::mutex> g(m_);
		d_.pop_front();
	}

private:
	std::mutex m_;
	std::deque<T> d_;
};

// "Kinds" give each queue a CLI name and a Queue<T, N> alias.
struct EyeKind
{
	static constexpr const char *name = "eye";
	template<class T, std::size_t N> using Queue = SPSCQueue<T, N>;
};
struct RigtorpKind
{
	static constexpr const char *name = "rigtorp";
	template<class T, std::size_t N> using Queue = RigtorpQueue<T, N>;
};
struct NaiveKind
{
	static constexpr const char *name = "naive";
	template<class T, std::size_t N> using Queue = NaiveQueue<T, N>;
};
struct MutexKind
{
	static constexpr const char *name = "mutex";
	template<class T, std::size_t N> using Queue = MutexQueue<T, N>;
};

template<class... Ks> struct List {};
using AllQueues = List<EyeKind, RigtorpKind, NaiveKind, MutexKind>;

template<class... Ks>
bool IsKnownQueue(const std::string &n, List<Ks...>) { return ((n == Ks::name) || ...); }

template<class... Ks>
std::string QueueNames(List<Ks...>)
{
	std::string s;
	for (const char *n : { Ks::name... })
	{
		if (!s.empty()) s += ',';
		s += n;
	}
	return s;
}

template<class F, class... Ks>
void ForEachQueueImpl(F &f, List<Ks...>)
{
	((g_opt.Enabled(Ks::name) ? (void)f.template operator()<Ks>() : (void)0), ...);
}

// Calls f.operator()<Kind>() for every enabled queue.
template<class F>
void ForEachQueue(F &&f) { ForEachQueueImpl(f, AllQueues{}); }

template<class K, class T, std::size_t N>
auto MakeQueue() { return std::make_unique<typename K::template Queue<T, N>>(); }

// =====================================================================
// Single-thread benchmarks
// =====================================================================

// push + front + pop on a ~empty queue: best-case cost of the code path.
template<class K, class T, std::size_t Cap>
void BM_PushPop(benchmark::State &st)
{
	auto q = MakeQueue<K, T, Cap>();
	u64 i = 0, sink = 0;
	for (auto _ : st)
	{
		benchmark::DoNotOptimize(q->TryEmplace(i++));
		T *p = q->Front();
		if (!p) { st.SkipWithError("unexpected empty queue"); return; }
		sink += p->touch();
		q->Pop();
	}
	benchmark::DoNotOptimize(sink);
	ReportThroughput(st, double(st.iterations()));
}

// Fill to exactly Cap, then drain: shows the working-set effect (L1 -> L2 -> L3 -> RAM).
template<class K, class T, std::size_t Cap>
void BM_FillDrain(benchmark::State &st)
{
	auto q = MakeQueue<K, T, Cap>();
	u64 sink = 0;
	for (auto _ : st)
	{
		for (std::size_t i = 0; i < Cap; ++i)
			if (!q->TryEmplace(static_cast<u64>(i))) { st.SkipWithError("queue full before Cap"); return; }
		for (std::size_t i = 0; i < Cap; ++i)
		{
			T *p = q->Front();
			if (!p) { st.SkipWithError("unexpected empty queue"); return; }
			sink += p->touch();
			q->Pop();
		}
	}
	benchmark::DoNotOptimize(sink);
	ReportThroughput(st, double(st.iterations()) * Cap);
}

// Fail-fast paths: what a spinning consumer / blocked producer pays per attempt.
template<class K>
void BM_EmptyFront(benchmark::State &st)
{
	auto q = MakeQueue<K, P64, 1024>();
	for (auto _ : st)
		benchmark::DoNotOptimize(q->Front());
	ReportThroughput(st, double(st.iterations()));
}

template<class K>
void BM_FullPush(benchmark::State &st)
{
	auto q = MakeQueue<K, P64, 1024>();
	for (std::size_t i = 0; i < 1024; ++i)
		(void)q->TryEmplace(static_cast<u64>(i));
	for (auto _ : st)
		benchmark::DoNotOptimize(q->TryEmplace(u64{ 1 }));
	ReportThroughput(st, double(st.iterations()));
}

// =====================================================================
// Two-thread benchmarks
// =====================================================================

struct Scenario { const char *name; u64 prodWork; u64 consWork; };

// Producer pushes items, consumer verifies order and data. Items/s over the whole run.
template<class K, class T, std::size_t Cap>
void BM_Throughput(benchmark::State &st, Scenario sc)
{
	const u64 n = g_opt.items;
	for (auto _ : st)
	{
		auto q = MakeQueue<K, T, Cap>();
		// On the first violation the consumer stops and tells the producer to stop too,
		// so a broken queue fails the run instead of hanging it.
		std::atomic<bool> failed{ false };
		u64 badItem = 0, badSeq = 0;
		const double sec = RunPair(
			[&] {
				for (u64 i = 0; i < n; ++i)
				{
					SpinWork(sc.prodWork);
					while (!q->TryEmplace(i))
					{
						if (failed.load(std::memory_order_acquire))
							return;
						CpuRelax();
					}
				}
			},
			[&] {
				u64 sum = 0;
				for (u64 i = 0; i < n; ++i)
				{
					T *p;
					while ((p = q->Front()) == nullptr)
						CpuRelax();
					if (p->seq() != i)
					{
						badItem = i;
						badSeq = p->seq();
						failed.store(true, std::memory_order_release);
						return;
					}
					sum += p->touch();
					q->Pop();
					SpinWork(sc.consWork);
				}
				benchmark::DoNotOptimize(sum);
			});
		if (failed.load(std::memory_order_acquire))
		{
			std::fprintf(stderr, "FIFO violation: item %llu expected, got seq %llu\n",
						 static_cast<unsigned long long>(badItem), static_cast<unsigned long long>(badSeq));
			st.SkipWithError("FIFO order / data violation");
			return;
		}
		st.SetIterationTime(sec);
	}
	ReportThroughput(st, double(n) * double(st.iterations()));
}

// Round trip through two queues. ns/op is the one-way latency (RTT / 2).
template<class K>
void BM_OneWayLatency(benchmark::State &st)
{
	using T = P8;
	constexpr u64 kWarmup = 10000, kRounds = 200000;
	for (auto _ : st)
	{
		auto req = MakeQueue<K, T, 1024>();
		auto rsp = MakeQueue<K, T, 1024>();
		std::atomic<bool> stop{ false };
		bool bad = false;
		double sec = 0;
		RunPair(
			[&] { // ping
				auto round = [&](u64 i) {
					while (!req->TryEmplace(i))
						CpuRelax();
					T *p;
					while ((p = rsp->Front()) == nullptr)
						CpuRelax();
					const bool ok = p->seq() == i;
					rsp->Pop();
					return ok;
				};
				for (u64 i = 0; i < kWarmup; ++i)
					round(i);
				const u64 t0 = NowNs();
				for (u64 i = 0; i < kRounds; ++i)
					if (!round(kWarmup + i)) bad = true;
				sec = double(NowNs() - t0) * 1e-9;
				stop.store(true, std::memory_order_relaxed);
			},
			[&] { // echo
				for (;;)
				{
					if (T *p = req->Front())
					{
						const T v = *p;
						req->Pop();
						while (!rsp->TryEmplace(v))
							CpuRelax();
					}
					else if (stop.load(std::memory_order_relaxed))
						return;
					else
						CpuRelax();
				}
			});
		if (bad) { st.SkipWithError("data violation"); return; }
		st.SetIterationTime(sec);
	}
	ReportThroughput(st, 2.0 * kRounds * double(st.iterations()));
}

// Latency distribution under paced load: every `intervalNs` the producer sends `burst` messages.
// The timestamp is the *scheduled* send time, so producer stalls count as latency
// (no coordinated omission). Includes ~one clock read (see banner).
template<class K>
void BM_Latency(benchmark::State &st, Load load)
{
	const u64 msgs = std::clamp<u64>(
		static_cast<u64>(double(load.burst) * 1e9 / double(load.intervalNs) * g_opt.latSeconds), 10000, 2000000);
	for (auto _ : st)
	{
		auto q = MakeQueue<K, TimedMsg, 1024>();
		std::vector<u64> lat;
		lat.reserve(msgs);
		bool bad = false;
		const double sec = RunPair(
			[&] {
				u64 next = NowNs() + 200'000; // lead time so the consumer is already spinning
				for (u64 sent = 0; sent < msgs;)
				{
					while (NowNs() < next)
						CpuRelax();
					for (u64 b = 0; b < load.burst && sent < msgs; ++b, ++sent)
						while (!q->TryEmplace(next, sent))
							CpuRelax();
					next += load.intervalNs;
				}
			},
			[&] {
				for (u64 i = 0; i < msgs; ++i)
				{
					TimedMsg *p;
					while ((p = q->Front()) == nullptr)
						CpuRelax();
					const u64 now = NowNs();
					if (p->seq != i) bad = true;
					lat.push_back(now - p->sendNs);
					q->Pop();
				}
			});
		if (bad) { st.SkipWithError("order violation"); return; }
		st.SetIterationTime(sec);

		lat.erase(lat.begin(), lat.begin() + static_cast<std::ptrdiff_t>(msgs / 100)); // warm-up
		std::sort(lat.begin(), lat.end());
		auto pct = [&](double p) { return double(lat[static_cast<std::size_t>(p * double(lat.size() - 1) + 0.5)]); };
		st.counters["p50"] = pct(0.50);
		st.counters["p90"] = pct(0.90);
		st.counters["p99"] = pct(0.99);
		st.counters["p99.9"] = pct(0.999);
		st.counters["max"] = double(lat.back());
	}
}

// =====================================================================
// Registration. Benchmark names are "<Suite>/<params>/<queue>", so the same test
// for different queues is printed on adjacent lines.
// =====================================================================

std::string Name(std::initializer_list<std::string> parts)
{
	std::string s;
	for (const std::string &p : parts)
		s += (s.empty() ? "" : "/") + p;
	return s;
}

template<class F>
auto Add(const std::string &name, F &&f) { return benchmark::RegisterBenchmark(name.c_str(), std::forward<F>(f)); }

template<class T, std::size_t Cap>
void AddPushPop(const char *tn)
{
	ForEachQueue([=]<class K>() { Add(Name({ "PushPop", tn, K::name }), BM_PushPop<K, T, Cap>); });
}

template<class T, std::size_t Cap>
void AddFillDrain(const char *tn)
{
	ForEachQueue([=]<class K>() {
		Add(Name({ "FillDrain", tn, "cap:" + std::to_string(Cap), K::name }), BM_FillDrain<K, T, Cap>);
	});
}

template<class T, std::size_t Cap>
void AddThroughput(const char *tn, const std::vector<Scenario> &scenarios)
{
	for (const Scenario &sc : scenarios)
		ForEachQueue([&]<class K>() {
			Add(Name({ "Throughput", tn, "cap:" + std::to_string(Cap), sc.name, K::name }),
				[sc](benchmark::State &st) { BM_Throughput<K, T, Cap>(st, sc); })->UseManualTime();
		});
}

void RegisterAll()
{
	const u64 w = g_opt.work;
	const std::vector<Scenario> allScenarios{
		{ "max_rate", 0, 0 }, { "slow_consumer", 0, w }, { "slow_producer", w, 0 }, { "balanced", w / 2, w / 2 } };
	const std::vector<Scenario> maxRate{ allScenarios.front() };

	// single thread
	AddPushPop<P8, 1024>("P8");
	AddPushPop<P64, 1024>("P64");
	AddPushPop<P256, 1024>("P256");
	AddPushPop<Str, 1024>("Str");
	ForEachQueue([]<class K>() { Add(Name({ "EmptyFront", K::name }), BM_EmptyFront<K>); });
	ForEachQueue([]<class K>() { Add(Name({ "FullPush", K::name }), BM_FullPush<K>); });
	AddFillDrain<P64, 256>("P64");       // 16 KiB
	AddFillDrain<P64, 4096>("P64");      // 256 KiB
	AddFillDrain<P64, 65536>("P64");     // 4 MiB
	AddFillDrain<P64, 1048576>("P64");   // 64 MiB

	// two threads: load scenarios, payload sizes, queue depth
	AddThroughput<P64, 1024>("P64", allScenarios);
	AddThroughput<P8, 1024>("P8", maxRate);
	AddThroughput<P256, 1024>("P256", maxRate);
	AddThroughput<Str, 1024>("Str", maxRate);
	AddThroughput<P64, 16>("P64", maxRate);
	AddThroughput<P64, 128>("P64", maxRate);
	AddThroughput<P64, 16384>("P64", maxRate);
	AddThroughput<P64, 262144>("P64", maxRate);

	// two threads: latency
	ForEachQueue([]<class K>() { Add(Name({ "OneWayLatency", K::name }), BM_OneWayLatency<K>)->UseManualTime(); });
	for (const Load &load : g_opt.latLoads)
		ForEachQueue([&]<class K>() {
			Add(Name({ "Latency", "every:" + std::to_string(load.intervalNs) + "ns",
					   "burst:" + std::to_string(load.burst), K::name }),
				[load](benchmark::State &st) { BM_Latency<K>(st, load); })->UseManualTime();
		});
}

// =====================================================================
// Command line
// =====================================================================

std::vector<std::string> Split(const std::string &s, char sep)
{
	std::vector<std::string> out;
	std::size_t pos = 0;
	while (pos <= s.size())
	{
		const std::size_t end = std::min(s.find(sep, pos), s.size());
		if (end > pos) out.push_back(s.substr(pos, end - pos));
		pos = end + 1;
	}
	return out;
}

bool TakeFlag(const char *arg, const char *name, std::string &value)
{
	const std::string prefix = std::string("--spsc_") + name + "=";
	if (std::strncmp(arg, prefix.c_str(), prefix.size()) != 0)
		return false;
	value = arg + prefix.size();
	return true;
}

void PrintHelp()
{
	std::printf(
		"SPSC benchmark options (all optional):\n"
		"  --spsc_queues=LIST      queues to run, any of: %s (default: all)\n"
		"  --spsc_prod_cpu=N       pin producer thread to cpu N (default: unpinned)\n"
		"  --spsc_cons_cpu=N       pin consumer thread to cpu N (default: unpinned)\n"
		"  --spsc_items=N          items per throughput run (default: %llu)\n"
		"  --spsc_work=N           busy-loop units used by slow_* scenarios (default: %llu)\n"
		"  --spsc_lat_loads=LIST   latency loads as INTERVAL_NS:BURST,... (default: 1000:1,10000:1,100000:1,20000:16)\n"
		"  --spsc_lat_seconds=S    target duration of one latency run (default: 1)\n"
		"Standard --benchmark_* flags work too (--benchmark_filter, --benchmark_repetitions, --benchmark_out, ...).\n"
		"Benchmark names: <Suite>/<params>/<queue>; suites: PushPop EmptyFront FullPush FillDrain Throughput OneWayLatency Latency.\n",
		QueueNames(AllQueues{}).c_str(),
		static_cast<unsigned long long>(g_opt.items), static_cast<unsigned long long>(g_opt.work));
}

// Consumes --spsc_* flags from argv, leaves everything else for Google Benchmark.
void ParseOptions(int &argc, char **argv)
{
	int kept = 1;
	for (int i = 1; i < argc; ++i)
	{
		std::string v;
		if (std::strcmp(argv[i], "--help_spsc") == 0) { PrintHelp(); std::exit(0); }
		else if (TakeFlag(argv[i], "prod_cpu", v)) g_opt.prodCpu = std::stoi(v);
		else if (TakeFlag(argv[i], "cons_cpu", v)) g_opt.consCpu = std::stoi(v);
		else if (TakeFlag(argv[i], "items", v)) g_opt.items = std::stoull(v);
		else if (TakeFlag(argv[i], "work", v)) g_opt.work = std::stoull(v);
		else if (TakeFlag(argv[i], "lat_seconds", v)) g_opt.latSeconds = std::stod(v);
		else if (TakeFlag(argv[i], "queues", v)) g_opt.queues = Split(v, ',');
		else if (TakeFlag(argv[i], "lat_loads", v))
		{
			g_opt.latLoads.clear();
			for (const std::string &item : Split(v, ','))
			{
				const std::size_t colon = item.find(':');
				if (colon == std::string::npos) throw std::invalid_argument("bad --spsc_lat_loads item: " + item);
				g_opt.latLoads.push_back({ std::stoull(item.substr(0, colon)), std::stoull(item.substr(colon + 1)) });
			}
		}
		else argv[kept++] = argv[i];
	}
	argc = kept;
	argv[argc] = nullptr;

	for (const std::string &q : g_opt.queues)
		if (!IsKnownQueue(q, AllQueues{}))
			throw std::invalid_argument("unknown queue '" + q + "' (known: " + QueueNames(AllQueues{}) + ")");
	for (const Load &l : g_opt.latLoads)
		if (l.intervalNs == 0 || l.burst == 0)
			throw std::invalid_argument("--spsc_lat_loads: interval and burst must be > 0");
	if (g_opt.items == 0 || g_opt.latSeconds <= 0)
		throw std::invalid_argument("--spsc_items and --spsc_lat_seconds must be > 0");
}

void PrintBanner()
{
	auto cpu = [](int c) { return c < 0 ? std::string("any") : std::to_string(c); };
#ifdef NDEBUG
	const char *build = "release";
#else
	const char *build = "DEBUG (asserts on, results are not representative)";
#endif
	std::printf("build: %s | producer cpu: %s | consumer cpu: %s | items/run: %llu | clock read: ~%.0f ns\n",
				build, cpu(g_opt.prodCpu).c_str(), cpu(g_opt.consCpu).c_str(),
				static_cast<unsigned long long>(g_opt.items), ClockOverheadNs());
	if (g_opt.prodCpu < 0 || g_opt.consCpu < 0)
		std::printf("WARNING: threads are not pinned; use --spsc_prod_cpu / --spsc_cons_cpu\n");
}

// =====================================================================
// Compact reporter: "<name>  key=value ...". Replaces the default console table
// (its CPU-time column is meaningless for multi-threaded, manual-time runs).
// =====================================================================

class TableReporter : public benchmark::BenchmarkReporter
{
public:
	bool ReportContext(const Context &) override
	{
		std::printf("ns/op = time per item (OneWayLatency: one-way latency), Mops = million items/s, "
					"p50..max = latency in ns\n\n");
		return true;
	}

	void ReportRuns(const std::vector<Run> &runs) override
	{
		static constexpr const char *kColumns[] = { "ns/op", "Mops", "p50", "p90", "p99", "p99.9", "max" };
		for (const Run &run : runs)
		{
			std::string name = run.benchmark_name();
			if (const std::size_t pos = name.find("/manual_time"); pos != std::string::npos)
				name.erase(pos, std::strlen("/manual_time"));

			std::printf("%-56s", name.c_str());
			bool any = false;
			for (const char *key : kColumns)
			{
				const auto it = run.counters.find(key);
				if (it == run.counters.end()) continue;
				const double v = it->second.value;
				std::printf("  %s=%.*f", key, v >= 100 ? 0 : v >= 10 ? 1 : 2, v);
				any = true;
			}
			std::printf(any ? "\n" : "  (skipped)\n");
		}
		std::fflush(stdout);
	}
};

} // namespace

int main(int argc, char **argv)
{
	try
	{
		ParseOptions(argc, argv);
	}
	catch (const std::exception &e)
	{
		std::fprintf(stderr, "error: %s (see --help_spsc)\n", e.what());
		return 1;
	}
	benchmark::Initialize(&argc, argv);
	if (benchmark::ReportUnrecognizedArguments(argc, argv))
		return 1;

	PrintBanner();
	RegisterAll();
	TableReporter reporter;
	benchmark::RunSpecifiedBenchmarks(&reporter);
	benchmark::Shutdown();
	return 0;
}