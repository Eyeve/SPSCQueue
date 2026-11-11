/*
 * Extended tests for SPSCQueue.
 *
 * Exception-safety contract that is verified here (depends on what T guarantees):
 *
 *   operation                               T's operation that may throw        guarantee expected from the queue
 *   --------------------------------------  ----------------------------------  ---------------------------------
 *   TryEmplace / TryEmplaceLazy             T(args...)                          strong: queue unchanged, slot not published
 *   TryPush(const T&) / TryPushLazy(const&) T(const T&)                         strong
 *   TryPush(T&&) / TryPushLazy(T&&)         T(T&&) (only if not noexcept)       strong; if T(T&&) is noexcept -> never throws
 *   push into a full queue                  -                                   returns false, T is NOT constructed, args not consumed
 *   Front / Pop / FrontLazy / PopLazy       only ~T() (noexcept by static_assert) never throws, never copies/moves/assigns
 *   CommitWrite / CommitRead                -                                   never throws
 *   ~SPSCQueue                              ~T()                                destroys all live elements exactly once
 *   SPSCQueue(touch, allocator)             Allocator::allocate                 strong: bad_alloc propagates, nothing leaks
 *
 * In lazy mode a throwing push does not roll back earlier uncommitted pushes of the same batch:
 * they stay pending and are published by the next CommitWrite().
 *
 * The queue must never use T::operator= and never use Allocator::construct / destroy.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <new>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "SPSCQueue.h"

namespace
{

#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
constexpr std::size_t kQueueCacheLine = 128;
#define SPSC_TEST_HAS_ALIGN128 1
#else
constexpr std::size_t kQueueCacheLine = 64;
#endif

// ---------------------------------------------------------------------------------------------
// Fault-injecting element type.
//   NothrowMove == true  : move ctor is noexcept, copy ctor and "emplace" ctor may throw
//   NothrowMove == false : move ctor may throw as well
// failXAt: -1 = disarmed; 0 = the next call throws; n > 0 = the (n+1)-th next call throws.
// `alive` is signed on purpose: a double destruction makes it negative.
// ---------------------------------------------------------------------------------------------
template<bool NothrowMove>
struct Faulty
{
	struct Error {};

	static inline long alive = 0;
	static inline long ctorCalls = 0;
	static inline long copyCalls = 0;
	static inline long moveCalls = 0;
	static inline long assignCalls = 0;
	static inline long dtorCalls = 0;
	static inline long failCtorAt = -1;
	static inline long failCopyAt = -1;
	static inline long failMoveAt = -1;

	static void Disarm() { failCtorAt = failCopyAt = failMoveAt = -1; }
	static void ResetCounts() { ctorCalls = copyCalls = moveCalls = assignCalls = dtorCalls = 0; }
	static void Reset() { alive = 0; ResetCounts(); Disarm(); }
	static void Tick(long &at)
	{
		if (at == 0)
		{
			at = -1;
			throw Error{};
		}
		if (at > 0)
		{
			--at;
		}
	}

	int value;

	explicit Faulty(int v)
	{
		++ctorCalls;
		Tick(failCtorAt);
		value = v;
		++alive;
	}
	Faulty(const Faulty &o) : value(o.value)
	{
		++copyCalls;
		Tick(failCopyAt);
		++alive;
	}
	Faulty(Faulty &&o) noexcept(NothrowMove) : value(o.value)
	{
		++moveCalls;
		if constexpr (!NothrowMove)
		{
			Tick(failMoveAt);
		}
		++alive;
	}
	Faulty &operator=(const Faulty &o)
	{
		++assignCalls;
		value = o.value;
		return *this;
	}
	Faulty &operator=(Faulty &&o) noexcept(NothrowMove)
	{
		++assignCalls;
		value = o.value;
		return *this;
	}
	~Faulty() noexcept
	{
		++dtorCalls;
		--alive;
	}
};

static_assert(std::is_nothrow_move_constructible_v< Faulty<true> >);
static_assert(!std::is_nothrow_move_constructible_v< Faulty<false> >);
static_assert(!std::is_nothrow_copy_constructible_v< Faulty<true> >);

template<class Q>
std::vector<int> Drain(Q &q)
{
	std::vector<int> out;
	while (auto *p = q.Front())
	{
		if constexpr (std::is_arithmetic_v< std::remove_cv_t< std::remove_pointer_t< decltype(p) > > >)
		{
			out.push_back(static_cast< int >(*p));
		}
		else
		{
			out.push_back(p->value);
		}
		q.Pop();
	}
	return out;
}

std::vector<int> Iota(int n, int start = 0)
{
	std::vector<int> v(static_cast<std::size_t>(n));
	std::iota(v.begin(), v.end(), start);
	return v;
}

// ---------------------------------------------------------------------------------------------
// Static properties
// ---------------------------------------------------------------------------------------------
using QU = SPSCQueue< std::size_t, 16 >;
static_assert(QU::GetCapacity() == 16);
static_assert(noexcept(QU::GetCapacity()));
static_assert(noexcept(std::declval< const QU & >().GetApproxSize()));
static_assert(noexcept(std::declval< const QU & >().IsEmpty()));
static_assert(std::is_nothrow_destructible_v< QU >);
static_assert(!std::is_copy_assignable_v< QU >);
static_assert(!std::is_move_assignable_v< QU >);
static_assert(!std::is_copy_constructible_v< QU >);
static_assert(!std::is_move_constructible_v< QU >);

TEST(static_props, capacity_and_typedefs)
{
	using Q = SPSCQueue< int, 64 >;
	EXPECT_EQ(Q::GetCapacity(), 64u);
	static_assert(std::is_same_v< Q::value_type, int >);
	static_assert(std::is_same_v< Q::size_type, std::size_t >);
	static_assert(std::is_same_v< Q::reference, int & >);
	Q q;
	EXPECT_EQ(q.GetCapacity(), 64u);
}

// ---------------------------------------------------------------------------------------------
// Basic single-thread behaviour
// ---------------------------------------------------------------------------------------------
TEST(basic, empty_queue_front_is_null)
{
	SPSCQueue< int, 8 > q;
	EXPECT_TRUE(q.IsEmpty());
	EXPECT_EQ(q.GetApproxSize(), 0u);
	EXPECT_EQ(q.Front(), nullptr);
	EXPECT_EQ(q.FrontLazy(), nullptr);
	q.CommitRead();  // no-op
	q.CommitWrite(); // no-op
	EXPECT_TRUE(q.IsEmpty());
}

TEST(basic, touch_flag_and_default_args)
{
	SPSCQueue< int, 8 > a(true);
	SPSCQueue< int, 8 > b(false);
	ASSERT_TRUE(a.TryPush(1));
	ASSERT_TRUE(b.TryPush(2));
	ASSERT_NE(a.Front(), nullptr);
	ASSERT_NE(b.Front(), nullptr);
	EXPECT_EQ(*a.Front(), 1);
	EXPECT_EQ(*b.Front(), 2);
}

TEST(basic, full_queue_rejects_and_recovers)
{
	SPSCQueue< int, 8 > q;
	for (int i = 0; i < 8; ++i)
	{
		ASSERT_TRUE(q.TryPush(i));
	}
	EXPECT_FALSE(q.TryPush(100));
	EXPECT_FALSE(q.TryEmplace(100));
	int lv = 101;
	EXPECT_FALSE(q.TryPush(lv));
	EXPECT_EQ(q.GetApproxSize(), 8u);

	ASSERT_NE(q.Front(), nullptr);
	EXPECT_EQ(*q.Front(), 0);
	q.Pop();
	EXPECT_TRUE(q.TryPush(8));
	EXPECT_FALSE(q.TryPush(9));

	for (int i = 1; i <= 8; ++i)
	{
		auto *p = q.Front();
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(*p, i);
		q.Pop();
	}
	EXPECT_TRUE(q.IsEmpty());
}

TEST(basic, front_is_stable_and_mutable)
{
	SPSCQueue< std::string, 4 > q;
	ASSERT_TRUE(q.TryEmplace(40, 'a'));
	ASSERT_TRUE(q.TryPush(std::string(40, 'b')));
	std::string *p1 = q.Front();
	std::string *p2 = q.Front();
	ASSERT_NE(p1, nullptr);
	EXPECT_EQ(p1, p2);
	p1->push_back('!');
	EXPECT_EQ(*q.Front(), std::string(40, 'a') + "!");
	q.Pop();
	EXPECT_EQ(*q.Front(), std::string(40, 'b'));
}

TEST(basic, wraparound_many_times_various_fill_levels)
{
	constexpr std::size_t N = 8;
	SPSCQueue< std::uint64_t, N > q;
	std::uint64_t wr = 0, rd = 0;
	for (std::size_t round = 0; round < 5000; ++round)
	{
		const std::size_t k = round % (N + 1);
		for (std::size_t i = 0; i < k; ++i)
		{
			ASSERT_TRUE(q.TryPush(wr++));
		}
		for (std::size_t i = 0; i < k; ++i)
		{
			auto *p = q.Front();
			ASSERT_NE(p, nullptr);
			ASSERT_EQ(*p, rd++);
			q.Pop();
		}
		ASSERT_TRUE(q.IsEmpty());
	}
}

TEST(basic, capacity_one)
{
	SPSCQueue< int, 1 > q;
	for (int i = 0; i < 1000; ++i)
	{
		ASSERT_TRUE(q.TryPush(i));
		ASSERT_FALSE(q.TryPush(-1));
		ASSERT_EQ(q.GetApproxSize(), 1u);
		ASSERT_EQ(*q.Front(), i);
		q.Pop();
		ASSERT_EQ(q.Front(), nullptr);
	}
}

TEST(basic, capacity_one_lazy)
{
	SPSCQueue< int, 1 > q;
	for (int i = 0; i < 1000; ++i)
	{
		ASSERT_TRUE(q.TryPushLazy(i));
		ASSERT_FALSE(q.TryPushLazy(-1));
		ASSERT_EQ(q.Front(), nullptr);
		q.CommitWrite();
		int *p = q.FrontLazy();
		ASSERT_NE(p, nullptr);
		ASSERT_EQ(*p, i);
		q.PopLazy();
		ASSERT_FALSE(q.TryPush(-1)); // slot not released yet
		q.CommitRead();
	}
}

TEST(basic, one_byte_elements_big_capacity)
{
	SPSCQueue< std::uint8_t, 256 > q;
	for (int round = 0; round < 3; ++round)
	{
		for (int i = 0; i < 256; ++i)
		{
			ASSERT_TRUE(q.TryPush(static_cast<std::uint8_t>(i)));
		}
		ASSERT_FALSE(q.TryPush(std::uint8_t{0}));
		for (int i = 0; i < 256; ++i)
		{
			ASSERT_EQ(*q.Front(), static_cast<std::uint8_t>(i));
			q.Pop();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Element type requirements
// ---------------------------------------------------------------------------------------------
TEST(types, move_only)
{
	SPSCQueue< std::unique_ptr< int >, 8 > q;
	ASSERT_TRUE(q.TryPush(std::make_unique< int >(5)));
	ASSERT_TRUE(q.TryEmplace(new int(6)));
	ASSERT_TRUE(q.TryPushLazy(std::make_unique< int >(7)));
	q.CommitWrite();
	for (int expect = 5; expect <= 7; ++expect)
	{
		auto *p = q.Front();
		ASSERT_NE(p, nullptr);
		std::unique_ptr< int > own = std::move(*p);
		q.Pop();
		ASSERT_NE(own, nullptr);
		EXPECT_EQ(*own, expect);
	}
}

struct NoDefault
{
	explicit NoDefault(int a, int b) : sum(a + b) {}
	int sum;
};

TEST(types, not_default_constructible)
{
	SPSCQueue< NoDefault, 8 > q;
	ASSERT_TRUE(q.TryEmplace(1, 2));
	ASSERT_TRUE(q.TryEmplaceLazy(3, 4));
	q.CommitWrite();
	EXPECT_EQ(q.Front()->sum, 3);
	q.Pop();
	EXPECT_EQ(q.Front()->sum, 7);
}

struct ConstMember
{
	explicit ConstMember(int x) : v(x) {}
	const int v; // no assignment operators
};
static_assert(!std::is_copy_assignable_v< ConstMember >);

TEST(types, queue_never_needs_assignment)
{
	SPSCQueue< ConstMember, 4 > q;
	for (int i = 0; i < 100; ++i)
	{
		ASSERT_TRUE(q.TryEmplace(i));
		ASSERT_EQ(q.Front()->v, i);
		q.Pop();
	}
}

template<std::size_t N>
struct Torn
{
	std::array< std::uint64_t, N > w;
	Torn() = default;
	void Fill(std::uint64_t i) { w.fill(i); }
	bool Is(std::uint64_t i) const
	{
		return std::all_of(w.begin(), w.end(), [i](std::uint64_t x) { return x == i; });
	}
};

TEST(types, odd_sizes)
{
	{
		SPSCQueue< Torn< 3 >, 16 > q; // 24 bytes: padding math with a size that does not divide the cache line
		for (std::uint64_t i = 0; i < 100; ++i)
		{
			Torn< 3 > t;
			t.Fill(i);
			ASSERT_TRUE(q.TryPush(t));
			ASSERT_TRUE(q.Front()->Is(i));
			q.Pop();
		}
	}
	{
		SPSCQueue< Torn< 33 >, 4 > q; // 264 bytes
		for (std::uint64_t i = 0; i < 50; ++i)
		{
			Torn< 33 > t;
			t.Fill(i);
			ASSERT_TRUE(q.TryPush(std::move(t)));
			ASSERT_TRUE(q.Front()->Is(i));
			q.Pop();
		}
	}
}

struct alignas(64) A64
{
	std::uint64_t v;
	explicit A64(std::uint64_t x = 0) : v(x) {}
};
#ifdef SPSC_TEST_HAS_ALIGN128
struct alignas(128) A128
{
	std::uint64_t v;
	explicit A128(std::uint64_t x = 0) : v(x) {}
};
#endif

template<class T>
void CheckAlignment()
{
	SPSCQueue< T, 8 > q;
	for (int i = 0; i < 40; ++i)
	{
		ASSERT_TRUE(q.TryEmplace(static_cast<std::uint64_t>(i)));
		T *p = q.Front();
		ASSERT_NE(p, nullptr);
		ASSERT_EQ(reinterpret_cast< std::uintptr_t >(p) % alignof(T), 0u) << "slot " << i;
		q.Pop();
	}
}

TEST(types, over_aligned_elements)
{
	CheckAlignment< std::uint64_t >();
	CheckAlignment< A64 >();
#ifdef SPSC_TEST_HAS_ALIGN128
	CheckAlignment< A128 >();
#endif
}

TEST(types, first_slot_is_cache_line_aligned)
{
	// Design note: std::align moves the data to a cache line boundary.
	SPSCQueue< std::uint64_t, 16 > q;
	ASSERT_TRUE(q.TryPush(1));
	EXPECT_EQ(reinterpret_cast< std::uintptr_t >(q.Front()) % kQueueCacheLine, 0u);
}

// ---------------------------------------------------------------------------------------------
// Lazy mode semantics (single thread)
// ---------------------------------------------------------------------------------------------
TEST(lazy, written_elements_invisible_until_commit)
{
	SPSCQueue< int, 8 > q;
	ASSERT_TRUE(q.TryPushLazy(1));
	ASSERT_TRUE(q.TryEmplaceLazy(2));
	int v = 3;
	ASSERT_TRUE(q.TryPushLazy(v));

	EXPECT_EQ(q.Front(), nullptr);
	EXPECT_EQ(q.FrontLazy(), nullptr);
	EXPECT_TRUE(q.IsEmpty());
	EXPECT_EQ(q.GetApproxSize(), 0u);

	q.CommitWrite();
	EXPECT_FALSE(q.IsEmpty());
	EXPECT_EQ(q.GetApproxSize(), 3u);
	EXPECT_EQ(Drain(q), (std::vector< int >{1, 2, 3}));
}

TEST(lazy, can_fill_whole_capacity_without_commit)
{
	SPSCQueue< int, 8 > q;
	for (int i = 0; i < 8; ++i)
	{
		ASSERT_TRUE(q.TryPushLazy(i));
	}
	EXPECT_FALSE(q.TryPushLazy(99));
	EXPECT_FALSE(q.TryEmplaceLazy(99));
	q.CommitWrite();
	EXPECT_EQ(q.GetApproxSize(), 8u);
	EXPECT_EQ(Drain(q), Iota(8));
}

TEST(lazy, popped_slots_not_reusable_until_commit_read)
{
	SPSCQueue< int, 4 > q;
	for (int i = 0; i < 4; ++i)
	{
		ASSERT_TRUE(q.TryPush(i));
	}
	for (int i = 0; i < 3; ++i)
	{
		int *p = q.FrontLazy();
		ASSERT_NE(p, nullptr);
		EXPECT_EQ(*p, i); // FrontLazy advances locally even before commit
		q.PopLazy();
	}
	EXPECT_FALSE(q.TryPush(100));
	EXPECT_FALSE(q.TryPushLazy(100));
	EXPECT_EQ(q.GetApproxSize(), 4u); // published read index has not moved

	q.CommitRead();
	EXPECT_EQ(q.GetApproxSize(), 1u);
	EXPECT_TRUE(q.TryPush(4));
	EXPECT_TRUE(q.TryPush(5));
	EXPECT_TRUE(q.TryPush(6));
	EXPECT_FALSE(q.TryPush(7));
	EXPECT_EQ(Drain(q), (std::vector< int >{3, 4, 5, 6}));
}

TEST(lazy, commit_without_pending_is_noop_and_repeatable)
{
	SPSCQueue< int, 4 > q;
	q.CommitWrite();
	q.CommitRead();
	ASSERT_TRUE(q.TryPushLazy(1));
	q.CommitWrite();
	q.CommitWrite();
	EXPECT_EQ(q.GetApproxSize(), 1u);
	ASSERT_NE(q.FrontLazy(), nullptr);
	q.PopLazy();
	q.CommitRead();
	q.CommitRead();
	EXPECT_TRUE(q.IsEmpty());
}

TEST(lazy, modes_may_be_switched_at_commit_boundaries)
{
	SPSCQueue< int, 8 > q;
	ASSERT_TRUE(q.TryPush(0));
	ASSERT_TRUE(q.TryPushLazy(1));
	q.CommitWrite();
	ASSERT_TRUE(q.TryPush(2)); // immediate again after commit

	EXPECT_EQ(*q.Front(), 0);
	q.Pop();
	EXPECT_EQ(*q.FrontLazy(), 1);
	q.PopLazy();
	q.CommitRead();
	EXPECT_EQ(*q.Front(), 2);
	q.Pop();
	EXPECT_TRUE(q.IsEmpty());
}

// ---------------------------------------------------------------------------------------------
// Lifetime / destructor
// ---------------------------------------------------------------------------------------------
TEST(lifetime, destructor_destroys_published_and_unpublished)
{
	auto sp = std::make_shared< int >(1);
	{
		SPSCQueue< std::shared_ptr< int >, 16 > q;
		for (int i = 0; i < 5; ++i)
		{
			ASSERT_TRUE(q.TryPush(sp));
		}
		for (int i = 0; i < 3; ++i)
		{
			ASSERT_TRUE(q.TryPushLazy(sp)); // never committed
		}
		EXPECT_EQ(sp.use_count(), 9);
	}
	EXPECT_EQ(sp.use_count(), 1);
}

TEST(lifetime, destructor_with_uncommitted_lazy_pops)
{
	auto sp = std::make_shared< int >(1);
	{
		SPSCQueue< std::shared_ptr< int >, 16 > q;
		for (int i = 0; i < 6; ++i)
		{
			ASSERT_TRUE(q.TryPush(sp));
		}
		for (int i = 0; i < 3; ++i)
		{
			ASSERT_NE(q.FrontLazy(), nullptr);
			q.PopLazy(); // never committed
		}
	}
	EXPECT_EQ(sp.use_count(), 1) << "leak or double destroy of lazily popped elements";
}

TEST(lifetime, pop_destroys_immediately)
{
	auto sp = std::make_shared< int >(1);
	SPSCQueue< std::shared_ptr< int >, 4 > q;
	ASSERT_TRUE(q.TryPush(sp));
	EXPECT_EQ(sp.use_count(), 2);
	ASSERT_NE(q.Front(), nullptr);
	q.Pop();
	EXPECT_EQ(sp.use_count(), 1);
}

TEST(lifetime, full_queue_push_does_not_steal_from_argument)
{
	SPSCQueue< std::string, 2 > q;
	ASSERT_TRUE(q.TryPush(std::string(50, 'a')));
	ASSERT_TRUE(q.TryPush(std::string(50, 'b')));
	std::string s(50, 'z');
	EXPECT_FALSE(q.TryPush(std::move(s)));
	EXPECT_EQ(s, std::string(50, 'z')) << "rvalue was moved-from although push failed";
	EXPECT_FALSE(q.TryPushLazy(std::move(s)));
	EXPECT_EQ(s, std::string(50, 'z'));
}

// ---------------------------------------------------------------------------------------------
// Basic behaviour, re-expressed with Faulty<> (replacement for the old NoCopy / CountedOpObj tests)
// ---------------------------------------------------------------------------------------------
constexpr std::size_t kBigCap = 1024;

template<class Q>
class QueueTraits : public ::testing::Test {};

using TraitsQueues = ::testing::Types<
	SPSCQueue< std::size_t, kBigCap >,
	SPSCQueue< std::string, kBigCap >,
	SPSCQueue< Faulty<true>, kBigCap >,
	SPSCQueue< Faulty<false>, kBigCap > >;
TYPED_TEST_SUITE(QueueTraits, TraitsQueues);

TYPED_TEST(QueueTraits, special_members)
{
	using Q = TypeParam;
	EXPECT_TRUE(std::is_default_constructible_v< Q >);
	EXPECT_FALSE(std::is_copy_constructible_v< Q >);
	EXPECT_FALSE(std::is_move_constructible_v< Q >);
	EXPECT_FALSE(std::is_copy_assignable_v< Q >);
	EXPECT_FALSE(std::is_move_assignable_v< Q >);
	EXPECT_TRUE(std::is_destructible_v< Q >);
	EXPECT_TRUE(std::is_nothrow_destructible_v< Q >);
}

class FaultyQueue : public ::testing::Test
{
protected:
	using F = Faulty<true>;
	using Q = SPSCQueue< F, kBigCap >;

	void SetUp() override { F::Reset(); }
	void TearDown() override { EXPECT_EQ(F::alive, 0) << "leaked or double destroyed elements"; }

	static void Prepare(Q &q) // half-fill, then forget the operations spent on it
	{
		const std::size_t n = q.GetCapacity() / 2;
		for (std::size_t i = 0; i < n; ++i)
		{
			ASSERT_TRUE(q.TryEmplace(static_cast< int >(i)));
		}
		F::ResetCounts();
	}
	static void ExpectNoOps()
	{
		EXPECT_EQ(F::ctorCalls, 0);
		EXPECT_EQ(F::copyCalls, 0);
		EXPECT_EQ(F::moveCalls, 0);
		EXPECT_EQ(F::assignCalls, 0);
		EXPECT_EQ(F::dtorCalls, 0);
	}
};

TEST_F(FaultyQueue, constructing_queue_creates_no_elements)
{
	{
		Q a(true);
		Q b(false);
		EXPECT_EQ(a.GetApproxSize(), 0u);
		EXPECT_EQ(b.GetApproxSize(), 0u);
		EXPECT_EQ(a.GetCapacity(), kBigCap);
	}
	ExpectNoOps(); // no T is default-built, copied or destroyed by the queue itself
}

TEST_F(FaultyQueue, push_and_emplace_with_throwing_copy)
{
	Q q;
	EXPECT_EQ(q.GetApproxSize(), 0u);

	// rvalues only: copy ctor is armed but must never be used
	F::failCopyAt = 0;
	bool thrown = false;
	try
	{
		ASSERT_TRUE(q.TryPush(F(1)));
		F c(2);
		ASSERT_TRUE(q.TryPush(std::move(c)));
	} catch (const F::Error &)
	{
		thrown = true;
	}
	EXPECT_FALSE(thrown);
	EXPECT_EQ(F::failCopyAt, 0);
	EXPECT_EQ(q.GetApproxSize(), 2u);

	// lvalue: copy throws, queue unchanged
	thrown = false;
	try
	{
		F c(666);
		ASSERT_TRUE(q.TryPush(c));
	} catch (const F::Error &)
	{
		thrown = true;
	}
	EXPECT_TRUE(thrown);
	EXPECT_EQ(q.GetApproxSize(), 2u);

	ASSERT_TRUE(q.TryPush(F(0)));
	EXPECT_EQ(q.GetApproxSize(), 3u);

	thrown = false;
	try
	{
		ASSERT_TRUE(q.TryEmplace(3));
		int c = 4;
		ASSERT_TRUE(q.TryEmplace(c));
	} catch (const F::Error &)
	{
		thrown = true;
	}
	EXPECT_FALSE(thrown);
	EXPECT_EQ(q.GetApproxSize(), 5u);

	int seen[5] = {};
	while (F *p = q.Front())
	{
		ASSERT_TRUE(p->value >= 0 && p->value <= 4) << p->value;
		++seen[p->value];
		q.Pop();
	}
	for (int i = 0; i < 5; ++i)
	{
		EXPECT_EQ(seen[i], 1) << "value " << i;
	}
}

TEST_F(FaultyQueue, empty)
{
	Q q;
	ASSERT_TRUE(q.IsEmpty());
	ASSERT_EQ(q.GetApproxSize(), 0u);
	ASSERT_EQ(F::ctorCalls, 0);

	F el(1);
	ASSERT_EQ(F::ctorCalls, 1);
	ASSERT_TRUE(q.TryPush(el));
	ASSERT_FALSE(q.IsEmpty());
	ASSERT_EQ(q.GetApproxSize(), 1u);
	ASSERT_EQ(F::copyCalls, 1);
}

TEST_F(FaultyQueue, push_alternating_lvalue_rvalue_until_full)
{
	Q q;
	std::size_t i = 0;
	for (;; ++i)
	{
		F v(static_cast< int >(i));
		const bool ok = (i % 2) ? q.TryPush(v) : q.TryPush(std::move(v));
		if (!ok)
		{
			break;
		}
		ASSERT_EQ(q.GetApproxSize(), i + 1);
	}
	ASSERT_EQ(i, q.GetCapacity());
	ASSERT_EQ(q.GetApproxSize(), q.GetCapacity());

	F *p;
	for (i = 0; (p = q.Front()) != nullptr; ++i)
	{
		ASSERT_EQ(p->value, static_cast< int >(i));
		q.Pop();
		ASSERT_EQ(q.GetApproxSize(), q.GetCapacity() - i - 1);
	}
	ASSERT_EQ(i, q.GetCapacity());
}

TEST_F(FaultyQueue, pop_destroys_each_element_once)
{
	Q q;
	Prepare(q);
	const std::size_t n = q.GetApproxSize();
	ASSERT_GT(n, 0u);
	for (std::size_t i = 0; i < n; ++i)
	{
		ASSERT_EQ(q.GetApproxSize(), n - i);
		ASSERT_NE(q.Front(), nullptr);
		q.Pop();
	}
	EXPECT_EQ(q.GetApproxSize(), 0u);
	ASSERT_TRUE(q.IsEmpty());
	EXPECT_EQ(F::dtorCalls, static_cast< long >(n));
	EXPECT_EQ(F::alive, 0);
}

TEST_F(FaultyQueue, operation_counts_per_push_kind)
{
	Q a;
	Prepare(a);
	Q b;
	Prepare(b);
	ExpectNoOps(); // preparing queues leaves no residue after the reset

	const std::size_t n = a.GetApproxSize();

	Q emplaced;
	for (std::size_t i = 0; i < n / 2; ++i)
	{
		ASSERT_TRUE(emplaced.TryEmplace(static_cast< int >(i)));
	}
	EXPECT_EQ(F::ctorCalls, static_cast< long >(n / 2));
	EXPECT_EQ(F::copyCalls + F::moveCalls + F::assignCalls + F::dtorCalls, 0)
		<< "emplace must build in place: no temporaries, copies, moves";

	F::ResetCounts();
	Q pushed;
	for (std::size_t i = 0; i < n; ++i)
	{
		F tmp(static_cast< int >(i));
		ASSERT_TRUE(pushed.TryPush(std::move(tmp)));
	}
	EXPECT_EQ(F::ctorCalls, static_cast< long >(n));  // the temporaries
	EXPECT_EQ(F::moveCalls, static_cast< long >(n));  // exactly one move per push
	EXPECT_EQ(F::copyCalls, 0);
	EXPECT_EQ(F::assignCalls, 0);
	EXPECT_EQ(F::dtorCalls, static_cast< long >(n));  // temporaries only, queue elements still alive
}

// ---------------------------------------------------------------------------------------------
// Exception safety, parametrised by the guarantees of T
// ---------------------------------------------------------------------------------------------
template<class F>
class ExceptionSafety : public ::testing::Test
{
protected:
	void SetUp() override { F::Reset(); }
	void TearDown() override { EXPECT_EQ(F::alive, 0) << "leaked (>0) or double destroyed (<0) elements"; }
};

using FaultyTypes = ::testing::Types< Faulty<true>, Faulty<false> >;
TYPED_TEST_SUITE(ExceptionSafety, FaultyTypes);

TYPED_TEST(ExceptionSafety, emplace_throw_leaves_queue_unchanged)
{
	using F = TypeParam;
	SPSCQueue< F, 8 > q;
	ASSERT_TRUE(q.TryEmplace(1));
	ASSERT_TRUE(q.TryEmplace(2));

	F::failCtorAt = 0;
	EXPECT_THROW((void)q.TryEmplace(3), typename F::Error);
	EXPECT_EQ(q.GetApproxSize(), 2u);
	EXPECT_EQ(F::alive, 2);

	ASSERT_TRUE(q.TryEmplace(4));
	EXPECT_EQ(Drain(q), (std::vector< int >{1, 2, 4}));
}

TYPED_TEST(ExceptionSafety, push_copy_throw_leaves_queue_unchanged)
{
	using F = TypeParam;
	SPSCQueue< F, 8 > q;
	F v(7);
	ASSERT_TRUE(q.TryPush(v));

	F::failCopyAt = 0;
	EXPECT_THROW((void)q.TryPush(v), typename F::Error);
	EXPECT_EQ(q.GetApproxSize(), 1u);
	EXPECT_EQ(F::alive, 1 + 1); // queue element + v

	ASSERT_TRUE(q.TryPush(v));
	EXPECT_EQ(Drain(q), (std::vector< int >{7, 7}));
}

TYPED_TEST(ExceptionSafety, push_move_depends_on_noexcept_move)
{
	using F = TypeParam;
	SPSCQueue< F, 8 > q;
	ASSERT_TRUE(q.TryPush(F(1)));

	F::failMoveAt = 0;
	if constexpr (std::is_nothrow_move_constructible_v< F >)
	{
		// the "fault" can not fire: the queue must use the noexcept move, never fall back to copy
		F::failCopyAt = 0;
		EXPECT_NO_THROW(ASSERT_TRUE(q.TryPush(F(2))));
		EXPECT_EQ(q.GetApproxSize(), 2u);
		EXPECT_EQ(F::failMoveAt, 0) << "noexcept move ctor must not be ticked";
		EXPECT_EQ(F::copyCalls, 0);
		F::Disarm();
		EXPECT_EQ(Drain(q), (std::vector< int >{1, 2}));
	}
	else
	{
		EXPECT_THROW((void)q.TryPush(F(2)), typename F::Error);
		EXPECT_EQ(q.GetApproxSize(), 1u);
		EXPECT_EQ(F::alive, 1);
		ASSERT_TRUE(q.TryPush(F(3)));
		EXPECT_EQ(Drain(q), (std::vector< int >{1, 3}));
	}
}

TYPED_TEST(ExceptionSafety, lazy_throw_keeps_pending_batch_and_queue_usable)
{
	using F = TypeParam;
	SPSCQueue< F, 8 > q;
	ASSERT_TRUE(q.TryEmplaceLazy(1));
	ASSERT_TRUE(q.TryEmplaceLazy(2));

	F::failCtorAt = 0;
	EXPECT_THROW((void)q.TryEmplaceLazy(3), typename F::Error);

	F v(40);
	F::failCopyAt = 0;
	EXPECT_THROW((void)q.TryPushLazy(v), typename F::Error);

	EXPECT_EQ(q.Front(), nullptr) << "nothing may be published before CommitWrite";
	EXPECT_EQ(q.GetApproxSize(), 0u);

	ASSERT_TRUE(q.TryEmplaceLazy(5));
	q.CommitWrite();
	EXPECT_EQ(Drain(q), (std::vector< int >{1, 2, 5})); // earlier pending elements survive
	EXPECT_EQ(F::alive, 1); // only v
}

TYPED_TEST(ExceptionSafety, lazy_push_move_throw)
{
	using F = TypeParam;
	if constexpr (!std::is_nothrow_move_constructible_v< F >)
	{
		SPSCQueue< F, 4 > q;
		ASSERT_TRUE(q.TryPushLazy(F(1)));
		F::failMoveAt = 0;
		EXPECT_THROW((void)q.TryPushLazy(F(2)), typename F::Error);
		ASSERT_TRUE(q.TryPushLazy(F(3)));
		q.CommitWrite();
		EXPECT_EQ(Drain(q), (std::vector< int >{1, 3}));
	}
	else
	{
		SPSCQueue< F, 4 > q;
		F::failMoveAt = 0;
		F::failCopyAt = 0;
		EXPECT_NO_THROW(ASSERT_TRUE(q.TryPushLazy(F(1))));
		F::Disarm();
		q.CommitWrite();
		EXPECT_EQ(Drain(q), (std::vector< int >{1}));
	}
}

TYPED_TEST(ExceptionSafety, retry_after_throw_at_every_position)
{
	using F = TypeParam;
	constexpr int n = 6;
	for (int lazy = 0; lazy < 2; ++lazy)
	{
		for (int k = 0; k < n; ++k)
		{
			SCOPED_TRACE(::testing::Message() << "lazy=" << lazy << " throwing construction #" << k);
			SPSCQueue< F, 8 > q;
			F::Disarm();
			F::failCtorAt = k;
			int thrown = 0;
			for (int i = 0; i < n; ++i)
			{
				for (;;)
				{
					try
					{
						const bool ok = lazy ? q.TryEmplaceLazy(i) : q.TryEmplace(i);
						ASSERT_TRUE(ok);
						break;
					} catch (const typename F::Error &)
					{
						++thrown;
					}
				}
			}
			if (lazy)
			{
				q.CommitWrite();
			}
			EXPECT_EQ(thrown, 1);
			EXPECT_EQ(q.GetApproxSize(), static_cast< std::size_t >(n));
			EXPECT_EQ(Drain(q), Iota(n));
		}
	}
}

TYPED_TEST(ExceptionSafety, full_queue_push_does_not_construct_anything)
{
	using F = TypeParam;
	SPSCQueue< F, 8 > q;
	for (int i = 0; i < 8; ++i)
	{
		ASSERT_TRUE(q.TryEmplace(i));
	}
	F v(100);
	F::ResetCounts();
	F::failCtorAt = F::failCopyAt = F::failMoveAt = 0; // anything constructed would throw

	bool r = true;
	EXPECT_NO_THROW(r = q.TryEmplace(1));
	EXPECT_FALSE(r);
	EXPECT_NO_THROW(r = q.TryPush(v));
	EXPECT_FALSE(r);
	EXPECT_NO_THROW(r = q.TryPush(std::move(v)));
	EXPECT_FALSE(r);
	EXPECT_NO_THROW(r = q.TryEmplaceLazy(1));
	EXPECT_FALSE(r);
	EXPECT_NO_THROW(r = q.TryPushLazy(v));
	EXPECT_FALSE(r);
	EXPECT_NO_THROW(r = q.TryPushLazy(std::move(v)));
	EXPECT_FALSE(r);

	EXPECT_EQ(F::ctorCalls + F::copyCalls + F::moveCalls, 0) << "T was constructed in a full queue";
	EXPECT_EQ(F::failCtorAt, 0);

	// make room: now the very same call must reach the throwing constructor
	F::Disarm();
	ASSERT_NE(q.Front(), nullptr);
	q.Pop();
	F::failCtorAt = 0;
	EXPECT_THROW((void)q.TryEmplace(1), typename F::Error);
	EXPECT_EQ(q.GetApproxSize(), 7u);
}

TYPED_TEST(ExceptionSafety, front_pop_commit_never_touch_T_except_dtor)
{
	using F = TypeParam;
	SPSCQueue< F, 8 > q;
	for (int i = 0; i < 6; ++i)
	{
		ASSERT_TRUE(q.TryEmplace(i));
	}
	F::ResetCounts();
	F::failCtorAt = F::failCopyAt = F::failMoveAt = 0;

	EXPECT_NO_THROW({
		ASSERT_NE(q.Front(), nullptr);
		q.Pop();
		ASSERT_NE(q.FrontLazy(), nullptr);
		q.PopLazy();
		ASSERT_NE(q.FrontLazy(), nullptr);
		q.PopLazy();
		q.CommitRead();
		q.CommitWrite();
		ASSERT_NE(q.Front(), nullptr);
		q.Pop();
	});
	EXPECT_EQ(F::ctorCalls + F::copyCalls + F::moveCalls + F::assignCalls, 0);
	EXPECT_EQ(q.GetApproxSize(), 2u);
	F::Disarm();
}

TYPED_TEST(ExceptionSafety, operation_counts_no_hidden_copies)
{
	using F = TypeParam;
	SPSCQueue< F, 16 > q;
	F v(1);

	F::ResetCounts();
	ASSERT_TRUE(q.TryPush(std::move(v)));
	EXPECT_EQ(F::moveCalls, 1);
	EXPECT_EQ(F::copyCalls, 0);
	EXPECT_EQ(F::ctorCalls, 0);

	F::ResetCounts();
	ASSERT_TRUE(q.TryPush(v));
	EXPECT_EQ(F::copyCalls, 1);
	EXPECT_EQ(F::moveCalls, 0);

	F::ResetCounts();
	ASSERT_TRUE(q.TryEmplace(5));
	EXPECT_EQ(F::ctorCalls, 1);
	EXPECT_EQ(F::copyCalls + F::moveCalls, 0) << "emplace must construct in place";

	F::ResetCounts();
	ASSERT_TRUE(q.TryPushLazy(std::move(v)));
	EXPECT_EQ(F::moveCalls, 1);
	EXPECT_EQ(F::copyCalls, 0);

	F::ResetCounts();
	ASSERT_TRUE(q.TryPushLazy(v));
	EXPECT_EQ(F::copyCalls, 1);
	EXPECT_EQ(F::moveCalls, 0);

	F::ResetCounts();
	ASSERT_TRUE(q.TryEmplaceLazy(6));
	EXPECT_EQ(F::ctorCalls, 1);
	EXPECT_EQ(F::copyCalls + F::moveCalls, 0);

	q.CommitWrite();
	EXPECT_EQ(F::assignCalls, 0);
}

TYPED_TEST(ExceptionSafety, never_assigns_during_long_run)
{
	using F = TypeParam;
	SPSCQueue< F, 8 > q;
	F::ResetCounts();
	for (int i = 0; i < 1000; ++i)
	{
		ASSERT_TRUE(q.TryEmplace(i));
		if (i % 3 == 0)
		{
			ASSERT_TRUE(q.TryEmplace(-i));
		}
		while (auto *p = q.Front())
		{
			(void)p;
			q.Pop();
		}
	}
	EXPECT_EQ(F::assignCalls, 0);
}

TYPED_TEST(ExceptionSafety, destructor_cleans_up_after_throws)
{
	using F = TypeParam;
	{
		SPSCQueue< F, 8 > q;
		ASSERT_TRUE(q.TryEmplace(1));
		ASSERT_TRUE(q.TryEmplaceLazy(2));
		F::failCtorAt = 0;
		EXPECT_THROW((void)q.TryEmplaceLazy(3), typename F::Error);
		ASSERT_TRUE(q.TryEmplaceLazy(4));
		// leave 1 published + 2 uncommitted elements
	}
	EXPECT_EQ(F::alive, 0);
	EXPECT_EQ(F::dtorCalls, 3);
}

// ---------------------------------------------------------------------------------------------
// Existing NoCopy-like semantic with std::string (allocating element, throwing copy replaced by
// bad_alloc is not injectable, so just make sure heap elements round-trip in both modes)
// ---------------------------------------------------------------------------------------------
TEST(types, strings_both_modes)
{
	SPSCQueue< std::string, 8 > q;
	for (int i = 0; i < 20; ++i)
	{
		const std::string s = std::string(64, 'x') + std::to_string(i);
		if (i % 2)
		{
			ASSERT_TRUE(q.TryPush(s));
			ASSERT_EQ(*q.Front(), s);
			q.Pop();
		}
		else
		{
			ASSERT_TRUE(q.TryPushLazy(s));
			q.CommitWrite();
			ASSERT_EQ(*q.FrontLazy(), s);
			q.PopLazy();
			q.CommitRead();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Allocator: used for storage only, constructor exception safety
// ---------------------------------------------------------------------------------------------
template<class T>
struct FailAlloc
{
	using value_type = T;
	static inline long allocs = 0, deallocs = 0, constructs = 0, destroys = 0;
	static inline long bytesOut = 0;
	static inline long failAt = -1; // 0 = next allocate throws

	static void Reset() { allocs = deallocs = constructs = destroys = bytesOut = 0; failAt = -1; }

	FailAlloc() = default;
	template<class U>
	FailAlloc(const FailAlloc< U > &) noexcept {}

	T *allocate(std::size_t n)
	{
		if (failAt == 0)
		{
			failAt = -1;
			throw std::bad_alloc();
		}
		if (failAt > 0)
		{
			--failAt;
		}
		++allocs;
		bytesOut += static_cast< long >(n * sizeof(T));
		return std::allocator< T >{}.allocate(n);
	}
	void deallocate(T *p, std::size_t n) noexcept
	{
		++deallocs;
		bytesOut -= static_cast< long >(n * sizeof(T));
		std::allocator< T >{}.deallocate(p, n);
	}
	template<class U, class... Args>
	void construct(U *p, Args &&... args)
	{
		++constructs;
		::new (static_cast< void * >(p)) U(std::forward< Args >(args)...);
	}
	template<class U>
	void destroy(U *p)
	{
		++destroys;
		p->~U();
	}
	template<class U>
	bool operator==(const FailAlloc< U > &) const noexcept { return true; }
};

TEST(allocator, bad_alloc_in_ctor_does_not_leak)
{
	using A = FailAlloc< std::string >;
	using Q = SPSCQueue< std::string, 64, A >;
	for (int touch = 0; touch < 2; ++touch)
	{
		for (long failAt = 0; failAt < 3; ++failAt)
		{
			A::Reset();
			A::failAt = failAt;
			bool threw = false;
			try
			{
				Q q(touch != 0);
			} catch (const std::bad_alloc &)
			{
				threw = true;
			}
			if (failAt == 0)
			{
				EXPECT_TRUE(threw);
			}
			EXPECT_EQ(A::allocs, A::deallocs) << "touch=" << touch << " failAt=" << failAt;
			EXPECT_EQ(A::bytesOut, 0);
		}
	}
}

TEST(allocator, storage_released_and_construct_not_used)
{
	using A = FailAlloc< std::string >;
	using Q = SPSCQueue< std::string, 16, A >;
	A::Reset();
	{
		Q q;
		EXPECT_GE(A::allocs, 1);
		for (int i = 0; i < 100; ++i)
		{
			ASSERT_TRUE(q.TryEmplace(40, 'a'));
			ASSERT_TRUE(q.TryPushLazy(std::string(40, 'b')));
			q.CommitWrite();
			ASSERT_NE(q.Front(), nullptr);
			q.Pop();
			ASSERT_NE(q.FrontLazy(), nullptr);
			q.PopLazy();
			q.CommitRead();
		}
		ASSERT_TRUE(q.TryPush(std::string(40, 'c')));
	}
	EXPECT_EQ(A::allocs, A::deallocs);
	EXPECT_EQ(A::bytesOut, 0);
	EXPECT_EQ(A::constructs, 0) << "elements must be built with std::construct_at, not allocator_traits::construct";
	EXPECT_EQ(A::destroys, 0) << "elements must be destroyed with std::destroy_at";
}

// ---------------------------------------------------------------------------------------------
// Model-based randomized test (single thread): lazy/immediate ops, fault injection
// ---------------------------------------------------------------------------------------------
TEST(model, random_ops_match_deque_with_fault_injection)
{
	using F = Faulty<true>;
	F::Reset();
	constexpr std::size_t N = 16;
	{
		SPSCQueue< F, N > q;
		std::mt19937_64 rng(0xC0FFEE);
		std::deque< int > committed; // published and not yet popped (by Pop or PopLazy)
		std::deque< int > pending;   // written lazily, not committed
		std::size_t lazyFree = 0;    // popped lazily, slot not released yet
		int next = 0;

		auto push = [&](bool lazy, bool viaCopy, bool arm)
		{
			const std::size_t occupied = committed.size() + pending.size() + lazyFree;
			const bool expectOk = occupied < N;
			bool thrown = false, ok = false;
			if (arm)
			{
				(viaCopy ? F::failCopyAt : F::failCtorAt) = 0;
			}
			try
			{
				if (viaCopy)
				{
					const F tmp(next);
					ok = lazy ? q.TryPushLazy(tmp) : q.TryPush(tmp);
				}
				else
				{
					ok = lazy ? q.TryEmplaceLazy(next) : q.TryEmplace(next);
				}
			} catch (const F::Error &)
			{
				thrown = true;
			}
			F::Disarm();
			if (arm && expectOk)
			{
				ASSERT_TRUE(thrown);
				return; // strong guarantee: model unchanged
			}
			ASSERT_FALSE(thrown);
			ASSERT_EQ(ok, expectOk);
			if (ok)
			{
				(lazy ? pending : committed).push_back(next);
				++next;
			}
		};

		auto pop = [&](bool lazy)
		{
			F *p = lazy ? q.FrontLazy() : q.Front();
			if (committed.empty())
			{
				ASSERT_EQ(p, nullptr);
				return;
			}
			ASSERT_NE(p, nullptr);
			ASSERT_EQ(p->value, committed.front());
			if (lazy)
			{
				q.PopLazy();
				++lazyFree;
			}
			else
			{
				q.Pop();
			}
			committed.pop_front();
		};

		for (int step = 0; step < 300000; ++step)
		{
			const unsigned op = static_cast< unsigned >(rng() % 9);
			const bool arm = rng() % 8 == 0;
			switch (op)
			{
			case 0:
				if (pending.empty()) push(false, false, arm);
				break;
			case 1:
				if (pending.empty()) push(false, true, arm);
				break;
			case 2:
				push(true, false, arm);
				break;
			case 3:
				push(true, true, arm);
				break;
			case 4:
				q.CommitWrite();
				committed.insert(committed.end(), pending.begin(), pending.end());
				pending.clear();
				break;
			case 5:
				if (lazyFree == 0) pop(false);
				break;
			case 6:
				pop(true);
				break;
			case 7:
				q.CommitRead();
				lazyFree = 0;
				break;
			case 8:
				if (lazyFree == 0)
				{
					F *a = q.Front();
					F *b = q.Front();
					ASSERT_EQ(a, b);
				}
				break;
			}
			ASSERT_FALSE(::testing::Test::HasFailure()) << "step " << step << " op " << op;
			ASSERT_EQ(q.GetApproxSize(), committed.size() + lazyFree) << "step " << step;
			ASSERT_EQ(q.IsEmpty(), committed.size() + lazyFree == 0) << "step " << step;
		}
	}
	EXPECT_EQ(F::alive, 0);
	EXPECT_EQ(F::assignCalls, 0);
}

// ---------------------------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------------------------
enum class Mode { Immediate, Lazy };

struct Gate
{
	std::atomic< bool > open{false};
	void Wait() const
	{
		while (!open.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
	}
	void Open() { open.store(true, std::memory_order_release); }
};

struct Watchdog
{
	std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	std::atomic< bool > tripped{false};
	bool Check(unsigned &spins)
	{
		if ((++spins & 0x3FFu) == 0 && std::chrono::steady_clock::now() > deadline)
		{
			tripped.store(true, std::memory_order_relaxed);
		}
		return tripped.load(std::memory_order_relaxed);
	}
};

template<class T>
T Make(std::uint64_t i)
{
	if constexpr (std::is_integral_v< T >)
	{
		return static_cast< T >(i);
	}
	else if constexpr (std::is_same_v< T, std::string >)
	{
		return std::string(40, 'x') + std::to_string(i); // beyond SSO
	}
	else
	{
		T v;
		v.Fill(i);
		return v;
	}
}

template<class T>
bool Check(const T &v, std::uint64_t i)
{
	if constexpr (std::is_integral_v< T >)
	{
		return v == static_cast< T >(i);
	}
	else if constexpr (std::is_same_v< T, std::string >)
	{
		return v == std::string(40, 'x') + std::to_string(i);
	}
	else
	{
		return v.Is(i);
	}
}

constexpr std::uint64_t kN = 200000;

template<class T, std::size_t Cap>
void Transfer(std::uint64_t n, Mode pm, Mode cm, unsigned pb = 16, unsigned cb = 16)
{
	SPSCQueue< T, Cap > q;
	Gate gate;
	Watchdog wd;
	std::atomic< std::uint64_t > bad{0};

	std::thread prod([&]
	{
		gate.Wait();
		unsigned spins = 0, pending = 0;
		for (std::uint64_t i = 0; i < n;)
		{
			const bool ok = pm == Mode::Immediate ? q.TryPush(Make< T >(i)) : q.TryPushLazy(Make< T >(i));
			if (ok)
			{
				++i;
				if (pm == Mode::Lazy && ++pending >= pb)
				{
					q.CommitWrite();
					pending = 0;
				}
				continue;
			}
			if (pm == Mode::Lazy && pending)
			{
				q.CommitWrite();
				pending = 0;
			}
			if (wd.Check(spins))
			{
				return;
			}
			std::this_thread::yield();
		}
		if (pm == Mode::Lazy && pending)
		{
			q.CommitWrite();
		}
	});

	std::thread cons([&]
	{
		gate.Wait();
		unsigned spins = 0, pending = 0;
		for (std::uint64_t i = 0; i < n;)
		{
			T *p = cm == Mode::Immediate ? q.Front() : q.FrontLazy();
			if (p)
			{
				if (!Check< T >(*p, i))
				{
					bad.fetch_add(1, std::memory_order_relaxed);
				}
				if (cm == Mode::Immediate)
				{
					q.Pop();
				}
				else
				{
					q.PopLazy();
					if (++pending >= cb)
					{
						q.CommitRead();
						pending = 0;
					}
				}
				++i;
				continue;
			}
			if (cm == Mode::Lazy && pending)
			{
				q.CommitRead();
				pending = 0;
			}
			if (wd.Check(spins))
			{
				return;
			}
			std::this_thread::yield();
		}
		if (cm == Mode::Lazy && pending)
		{
			q.CommitRead();
		}
	});

	gate.Open();
	prod.join();
	cons.join();

	EXPECT_FALSE(wd.tripped.load()) << "deadlock / lost element (watchdog)";
	EXPECT_EQ(bad.load(), 0u) << "reordered, duplicated or torn elements";
	EXPECT_TRUE(q.IsEmpty());
	EXPECT_EQ(q.GetApproxSize(), 0u);
}

template<class T, std::size_t Cap>
void AllModes(std::uint64_t n = kN)
{
	for (Mode pm : {Mode::Immediate, Mode::Lazy})
	{
		for (Mode cm : {Mode::Immediate, Mode::Lazy})
		{
			SCOPED_TRACE(::testing::Message() << "Cap=" << Cap << " sizeof(T)=" << sizeof(T)
			                                  << " producer=" << (pm == Mode::Lazy ? "lazy" : "immediate")
			                                  << " consumer=" << (cm == Mode::Lazy ? "lazy" : "immediate"));
			Transfer< T, Cap >(n, pm, cm);
			if (::testing::Test::HasFailure())
			{
				return;
			}
		}
	}
}

TEST(concurrent, u64_various_capacities)
{
	AllModes< std::uint64_t, 1 >(50000);
	AllModes< std::uint64_t, 2 >(100000);
	AllModes< std::uint64_t, 4 >();
	AllModes< std::uint64_t, 64 >();
	AllModes< std::uint64_t, 1024 >();
}

TEST(concurrent, payload_sizes_detect_tearing)
{
	AllModes< Torn< 1 >, 8 >();
	AllModes< Torn< 3 >, 16 >();
	AllModes< Torn< 8 >, 4 >();   // 64 bytes
	AllModes< Torn< 16 >, 64 >(); // 128 bytes
	AllModes< Torn< 32 >, 2 >(100000); // 256 bytes
}

TEST(concurrent, strings)
{
	AllModes< std::string, 4 >(50000);
	AllModes< std::string, 256 >(50000);
}

TEST(concurrent, small_type_u8_wraps_value)
{
	AllModes< std::uint8_t, 256 >();
}

TEST(concurrent, batch_sizes)
{
	for (unsigned pb : {1u, 3u, 64u, 1000u})
	{
		for (unsigned cb : {1u, 5u, 64u, 1000u})
		{
			SCOPED_TRACE(::testing::Message() << "pb=" << pb << " cb=" << cb);
			Transfer< std::uint64_t, 64 >(50000, Mode::Lazy, Mode::Lazy, pb, cb);
			if (::testing::Test::HasFailure())
			{
				return;
			}
		}
	}
}

// Producer: element ctor throws every 5th call. Consumer must observe exactly 0..n-1 in order:
// a throwing push must never publish a half-built element and must never lose / duplicate a slot.
struct ThrowSeq
{
	struct Error {};
	static inline thread_local unsigned calls = 0;
	std::uint64_t a, b, c;
	explicit ThrowSeq(std::uint64_t s)
	{
		if (++calls % 5 == 0)
		{
			throw Error{};
		}
		a = b = c = s;
	}
	bool Is(std::uint64_t s) const { return a == s && b == s && c == s; }
};

void ThrowingProducerTransfer(Mode pm, Mode cm, std::uint64_t n)
{
	SPSCQueue< ThrowSeq, 16 > q;
	Gate gate;
	Watchdog wd;
	std::atomic< std::uint64_t > bad{0}, thrown{0};

	std::thread prod([&]
	{
		gate.Wait();
		unsigned spins = 0, pending = 0;
		std::uint64_t localThrown = 0;
		for (std::uint64_t i = 0; i < n;)
		{
			bool ok = false;
			try
			{
				ok = pm == Mode::Immediate ? q.TryEmplace(i) : q.TryEmplaceLazy(i);
			} catch (const ThrowSeq::Error &)
			{
				++localThrown;
				continue; // retry the same value
			}
			if (ok)
			{
				++i;
				if (pm == Mode::Lazy && ++pending >= 8)
				{
					q.CommitWrite();
					pending = 0;
				}
				continue;
			}
			if (pm == Mode::Lazy && pending)
			{
				q.CommitWrite();
				pending = 0;
			}
			if (wd.Check(spins))
			{
				break;
			}
			std::this_thread::yield();
		}
		if (pm == Mode::Lazy && pending)
		{
			q.CommitWrite();
		}
		thrown.store(localThrown);
	});

	std::thread cons([&]
	{
		gate.Wait();
		unsigned spins = 0, pending = 0;
		for (std::uint64_t i = 0; i < n;)
		{
			ThrowSeq *p = cm == Mode::Immediate ? q.Front() : q.FrontLazy();
			if (p)
			{
				if (!p->Is(i))
				{
					bad.fetch_add(1, std::memory_order_relaxed);
				}
				if (cm == Mode::Immediate)
				{
					q.Pop();
				}
				else
				{
					q.PopLazy();
					if (++pending >= 8)
					{
						q.CommitRead();
						pending = 0;
					}
				}
				++i;
				continue;
			}
			if (cm == Mode::Lazy && pending)
			{
				q.CommitRead();
				pending = 0;
			}
			if (wd.Check(spins))
			{
				return;
			}
			std::this_thread::yield();
		}
		if (cm == Mode::Lazy && pending)
		{
			q.CommitRead();
		}
	});

	gate.Open();
	prod.join();
	cons.join();

	EXPECT_FALSE(wd.tripped.load());
	EXPECT_EQ(bad.load(), 0u);
	EXPECT_GT(thrown.load(), 0u);
	EXPECT_TRUE(q.IsEmpty());
}

TEST(concurrent, throwing_producer_immediate)
{
	ThrowingProducerTransfer(Mode::Immediate, Mode::Immediate, 100000);
	ThrowingProducerTransfer(Mode::Immediate, Mode::Lazy, 100000);
}

TEST(concurrent, throwing_producer_lazy)
{
	ThrowingProducerTransfer(Mode::Lazy, Mode::Immediate, 100000);
	ThrowingProducerTransfer(Mode::Lazy, Mode::Lazy, 100000);
}

// Any thread may call GetApproxSize / IsEmpty. Invariant from the design notes: 0 <= write - read <= Capacity.
// NOTE: if the implementation loads the two indices without clamping, this may legitimately fail
// (torn pair of loads) - that would be a real finding, not a test bug.
TEST(concurrent, approx_size_stays_in_bounds_for_observer)
{
	constexpr std::size_t Cap = 8;
	SPSCQueue< std::uint64_t, Cap > q;
	Gate gate;
	Watchdog wd;
	std::atomic< bool > done{false};
	std::atomic< std::uint64_t > violations{0};
	constexpr std::uint64_t n = 200000;

	std::thread prod([&]
	{
		gate.Wait();
		unsigned spins = 0;
		for (std::uint64_t i = 0; i < n;)
		{
			if (q.TryPush(i))
			{
				++i;
			}
			else if (wd.Check(spins))
			{
				return;
			}
		}
	});
	std::thread cons([&]
	{
		gate.Wait();
		unsigned spins = 0;
		for (std::uint64_t i = 0; i < n;)
		{
			if (auto *p = q.Front())
			{
				(void)p;
				q.Pop();
				++i;
			}
			else if (wd.Check(spins))
			{
				return;
			}
		}
	});
	std::thread observer([&]
	{
		gate.Wait();
		while (!done.load(std::memory_order_acquire))
		{
			if (q.GetApproxSize() > Cap)
			{
				violations.fetch_add(1, std::memory_order_relaxed);
			}
			(void)q.IsEmpty();
		}
	});

	gate.Open();
	prod.join();
	cons.join();
	done.store(true, std::memory_order_release);
	observer.join();

	EXPECT_FALSE(wd.tripped.load());
	EXPECT_EQ(violations.load(), 0u);
}

} // namespace