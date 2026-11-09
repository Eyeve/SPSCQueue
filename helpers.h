#pragma once

#include "SPSCQueue.h"


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

inline OpCount opCount;
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

#ifdef __cpp_lib_hardware_interference_size
constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
constexpr size_t kCacheLine = 64;
#endif

constexpr size_t kDefaultCapacity = 1024;

using queue_sizet_t = SPSCQueue< size_t, kDefaultCapacity >;
using queue_string_t = SPSCQueue< std::string, kDefaultCapacity >;
using queue_nc_t = SPSCQueue< NoCopy, kDefaultCapacity >;
using queue_co_t = SPSCQueue< CountedOpObj<size_t>, kDefaultCapacity >;

void prepare(queue_co_t& q)
{
	size_t n = q.GetCapacity() / 2;
	for (size_t i = 0; i < n; ++i)
	{
		q.TryPush(CountedOpObj<size_t>(i));
	}
	opCount.clearCounters();
}

template <std::size_t Size>
struct Payload
{
	static_assert(Size > sizeof(std::uint64_t), "Payload must be bigger than seq");
	std::uint64_t seq;
	std::array<std::byte, Size - sizeof(std::uint64_t)> pad;
};

using U64 = std::uint64_t;
using P1 = Payload<kCacheLine>;
using P4 = Payload<kCacheLine * 4>;

template <typename T>
T MakeValue(std::uint64_t seq)
{
	if constexpr (std::is_integral_v<T>)
	{
		return static_cast<T>(seq);
	}
	else
	{
		T v{};
		v.seq = seq;
		return v;
	}
}
