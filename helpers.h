#pragma once

#include "SPSCQueue.h"

#ifdef __cpp_lib_hardware_interference_size
	constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
	constexpr size_t kCacheLine = 64;
#endif

constexpr size_t kDefaultCapacity = 128;

class NoCopy
{
public:
	int value;
	NoCopy(int value = 0) noexcept : value(value) {}
	NoCopy(NoCopy &&) = default;
	NoCopy(const NoCopy &) { throw -1; }

	NoCopy &operator=(NoCopy &&) = default;
	NoCopy &operator=(const NoCopy &) { throw -2; }
};

struct OpCount
{
	size_t creationCount;
	size_t ctorCount;
	size_t mtorCount;
	size_t copCount;
	size_t mopCount;
	size_t dtorCount;

	OpCount(
		size_t creation_count = 0,
		size_t ctor_count = 0,
		size_t mtor_count = 0,
		size_t cop_count = 0,
		size_t mop_count = 0,
		size_t dtor_count = 0)
		: creationCount(creation_count),
	      ctorCount(ctor_count),
	      mtorCount(mtor_count),
	      copCount(cop_count),
		  mopCount(mop_count),
	      dtorCount(dtor_count)
	{
	}

	bool operator==(const OpCount &rhs) const = default;

	void clearCounters()
	{
		creationCount = 0;
		ctorCount = 0;
		mtorCount = 0;
		copCount = 0;
		mopCount = 0;
		dtorCount = 0;
	}

	friend std::ostream &operator<<(std::ostream &os, const OpCount &stat)
	{
		os << "creationCount: " << stat.creationCount
		   << " ctorCount: " << stat.ctorCount
		   << " mtorCount: " << stat.mtorCount
		   << " copCount: " << stat.copCount
		   << " mopCount: " << stat.mopCount
		   << " dtorCount: " << stat.dtorCount;
		return os;
	}
};

OpCount opCount;
const OpCount NO_OP = OpCount();

template <typename T>
struct CountedOpObj
{
	T value;

	explicit CountedOpObj(size_t value) noexcept: value(value) { ++opCount.creationCount; }
	CountedOpObj(const CountedOpObj &oth) noexcept : value(oth.value) { opCount.ctorCount++; }
	CountedOpObj(CountedOpObj &&oth) noexcept : value(oth.value) { opCount.mtorCount++; }
	CountedOpObj &operator=(const CountedOpObj &) noexcept
	{
		opCount.copCount++;
		return *this;
	};
	CountedOpObj &operator=(CountedOpObj &&) noexcept
	{
		opCount.mopCount++;
		return *this;
	};
	~CountedOpObj() noexcept { opCount.dtorCount++; }
	bool operator==(const CountedOpObj &rhs) const { return value == rhs.value; }
};

template <size_t Capacity>
SPSCQueue< CountedOpObj<size_t>, Capacity > prepare()
{
	size_t n = Capacity / 2;
	auto b = SPSCQueue< CountedOpObj<size_t>, Capacity >();
	for (size_t i = 0; i < n; ++i)
	{
		b.insert(CountedOpObj<size_t>(i));
	}
	opCount.clearCounters();
	return b;
}


using queue_sizet_t = SPSCQueue< size_t, kDefaultCapacity >;
using queue_string_t = SPSCQueue< std::string, kDefaultCapacity >;
using queue_nc_t = SPSCQueue< NoCopy, kDefaultCapacity >;
using queue_co_t = SPSCQueue< CountedOpObj<size_t>, kDefaultCapacity >;

/*

// template <std::size_t Size>
// struct Payload
// {
// 	static_assert(Size > sizeof(std::uint64_t), "Payload must be bigger than seq");
// 	std::uint64_t seq;
// 	std::array<std::byte, Size - sizeof(std::uint64_t)> pad;
// };
//
// using U64 = std::uint64_t;
// using P64 = Payload<kCacheLine>;    // одна кэш-линия
// using P256 = Payload<kCacheLine * 4>;  // несколько кэш-линий

// template <typename T>
// T MakeValue(std::uint64_t seq)
// {
// 	if constexpr (std::is_integral_v<T>)
// 	{
// 		return static_cast<T>(seq);
// 	}
// 	else
// 	{
// 		T v{};
// 		v.seq = seq;
// 		return v;
// 	}
// }

// template <typename T>
// std::uint64_t SeqOf(const T& v)
// {
// 	if constexpr (std::is_integral_v<T>)
// 	{
// 		return static_cast<std::uint64_t>(v);
// 	}
// 	else
// 	{
// 		return v.seq;
// 	}
// }

// inline void CpuRelax() noexcept
// {
// #if defined(__x86_64__) || defined(__i386__)
// 	_mm_pause();
// #else
// 	std::this_thread::yield();
// #endif
// }

// Закрепляет текущий поток за CPU из переменной окружения (если она задана).
// void PinThisThread([[maybe_unused]] const char* envName)
// {
// #if defined(__linux__)
// 	if (const char* s = std::getenv(envName))
// 	{
// 		cpu_set_t set;
// 		CPU_ZERO(&set);
// 		CPU_SET(std::atoi(s), &set);
// 		pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
// 	}
// #endif
// }

*/
