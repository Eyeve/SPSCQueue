/*
 * Bounded lock-free SPSC queue: exactly one producer thread and one consumer thread.
 *
 * Each side has two modes. They must not be mixed while anything is uncommitted:
 *
 *   immediate  producer: TryEmplace / TryPush              consumer: Front / Pop
 *              every operation is published to the other thread right away.
 *
 *   lazy       producer: TryEmplaceLazy / TryPushLazy ... CommitWrite()
 *              consumer: FrontLazy / PopLazy ......... CommitRead()
 *              a batch is published by one release store in Commit*().
 *              Lazily written elements are invisible to the consumer until CommitWrite();
 *              lazily popped slots are not reusable by the producer until CommitRead().
 *
 * Calling an immediate operation while lazy ones are uncommitted breaks the queue.
 * The contract is checked by assert() only, so it costs nothing in release builds.
 * Elements that were never published or never released are destroyed by the destructor.
 *
 * Design notes:
 *  - Indices are monotonically increasing counters, never reduced modulo Capacity.
 *    size_t arithmetic wraps modulo 2^N and Capacity (a power of two) divides 2^N, so
 *    (write - read) is the exact element count and (idx & kMask) the slot, also across wrap-around.
 *    Invariant: 0 <= write - read <= Capacity. Never compare indices with < or >, only use differences.
 *  - Each side caches the other side's index and re-reads the shared atomic only when the cache
 *    says "full" / "empty", which keeps cache-line ping-pong between the cores low.
 *  - Objects are built with std::construct_at and destroyed with std::destroy_at;
 *    Allocator is used for storage only.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#ifndef EYE_NODISCARD
#define EYE_NODISCARD [[nodiscard]]
#endif

#ifndef EYE_ALWAYS_INLINE
#if defined(__GNUC__) || defined(__clang__)
#define EYE_ALWAYS_INLINE [[gnu::always_inline]] inline
#elif defined(_MSC_VER)
#define EYE_ALWAYS_INLINE __forceinline
#else
#define EYE_ALWAYS_INLINE inline
#endif
#endif

// ------------------------------------------
// START OF SPSC-QUEUE INTERFACE
// ------------------------------------------

template<typename T, std::size_t Capacity, typename Allocator = std::allocator< T > >
class SPSCQueue
{
	using alloc_type = Allocator;
	using alloc_traits = std::allocator_traits< alloc_type >;

public:
	using value_type = T;
	using pointer = T*;
	using const_pointer = const T*;
	using reference = T&;
	using const_reference = const T&;
	using difference_type = std::ptrdiff_t;
	using size_type = std::size_t;

private:
	static constexpr size_type kMask = Capacity - 1;
	static constexpr size_type kDataBytes = Capacity * sizeof(T);
	// 128 bytes: avoids false sharing through the adjacent-line prefetcher (x86-64)
	// and covers 128-byte cache lines (Apple ARM64)
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
	static constexpr size_type kCacheLineSize = 128;
#else
	static constexpr size_type kCacheLineSize = 64;
#endif
	// Padding (in elements) on each side of the data. It keeps the buffer away from neighbouring
	// allocations and gives std::align room to move the data to a cache line boundary.
	static constexpr size_type kPadding = (kCacheLineSize - 1) / sizeof(T) + 1;
	static constexpr size_type kBaseCapacity = Capacity + kPadding * 2;

	static_assert(std::has_single_bit(Capacity), "Capacity must be a (non-zero) power of two");
	static_assert(std::has_single_bit(kCacheLineSize), "Cache line size must be a power of two");
	static_assert(std::is_unsigned_v< size_type >, "Index wrap-around relies on unsigned arithmetic");
	static_assert(std::is_nothrow_destructible_v< T >, "T must be nothrow destructible");
	static_assert(kCacheLineSize % alignof(T) == 0, "T can't be aligned to start of cache line");
	static_assert(kPadding * sizeof(T) >= kCacheLineSize, "Padding must leave room to align the data");
	static_assert(Capacity <= std::numeric_limits<size_t>::max() / sizeof(T) - 2 * kPadding, "No space for padding");
	// nothrow ctr, exception guarantee
	// have ctr
	// readIdx <= writeIdx (or overflow)
	// what if writeIdx overflow hmmm

public:
	explicit SPSCQueue(bool touch = true, const Allocator &allocator = Allocator());
	~SPSCQueue();

	/* non-copyable and non-movable */
	SPSCQueue(const SPSCQueue &) = delete;
	SPSCQueue &operator=(const SPSCQueue &) = delete;

	/* producer thread only, immediate mode: published right away */
	template<typename... Args>
	EYE_NODISCARD EYE_ALWAYS_INLINE bool TryEmplace(Args &&... args);
	EYE_NODISCARD EYE_ALWAYS_INLINE bool TryPush(const T &value);
	EYE_NODISCARD EYE_ALWAYS_INLINE bool TryPush(T &&value);

	/* producer thread only, lazy mode: invisible to the consumer until CommitWrite() */
	template<typename... Args>
	EYE_NODISCARD EYE_ALWAYS_INLINE bool TryEmplaceLazy(Args &&... args);
	EYE_NODISCARD EYE_ALWAYS_INLINE bool TryPushLazy(const T &value);
	EYE_NODISCARD EYE_ALWAYS_INLINE bool TryPushLazy(T &&value);
	EYE_ALWAYS_INLINE void CommitWrite();

	/* consumer thread only, immediate mode: slot is released right away */
	EYE_NODISCARD EYE_ALWAYS_INLINE T* Front();
	EYE_ALWAYS_INLINE void Pop();

	/* consumer thread only, lazy mode: slots are released by CommitRead() */
	EYE_NODISCARD EYE_ALWAYS_INLINE T* FrontLazy();
	EYE_ALWAYS_INLINE void PopLazy();
	EYE_ALWAYS_INLINE void CommitRead();

	/* info, any thread */
	EYE_NODISCARD static constexpr size_type GetCapacity() noexcept;
	EYE_NODISCARD size_type GetApproxSize() const noexcept;
	EYE_NODISCARD bool IsEmpty() const noexcept;

private:
	EYE_NODISCARD EYE_ALWAYS_INLINE static pointer SlotAt(pointer data, size_type idx) noexcept
	{
		return data + (idx & kMask);
	}

	/* Own-side indices. Relaxed loads are enough: only the owning thread writes them. */
	EYE_NODISCARD EYE_ALWAYS_INLINE size_type PublishedWrite() const noexcept
	{
		return writer_.publishedWriteIdx.load(std::memory_order_relaxed);
	}
	EYE_NODISCARD EYE_ALWAYS_INLINE size_type PublishedRead() const noexcept
	{
		return reader_.publishedReadIdx.load(std::memory_order_relaxed);
	}
	/* Own-side position including uncommitted lazy operations. */
	EYE_NODISCARD EYE_ALWAYS_INLINE size_type WritePos() const noexcept { return PublishedWrite() + writer_.pending; }
	EYE_NODISCARD EYE_ALWAYS_INLINE size_type ReadPos() const noexcept { return PublishedRead() + reader_.pending; }

	/* Producer: is slot `write` free? Re-reads the shared read index only if the cache says "full". */
	EYE_NODISCARD EYE_ALWAYS_INLINE bool HasRoom(size_type write) noexcept
	{
		if (write - writer_.readCacheIdx == Capacity) [[unlikely]]
		{
			writer_.readCacheIdx = reader_.publishedReadIdx.load(std::memory_order_acquire);
			if (write - writer_.readCacheIdx == Capacity) [[unlikely]]
				return false;
		}
		return true;
	}

	/* Producer: construct an element in slot `write` if there is room. Does not publish. */
	template<typename... Args>
	EYE_NODISCARD EYE_ALWAYS_INLINE bool EmplaceAt(size_type write, Args &&... args)
	{
		if (!HasRoom(write)) [[unlikely]]
			return false;
		std::construct_at(SlotAt(writer_.data, write), std::forward< Args >(args)...);
		return true;
	}

	/* Consumer: element at `read`, or nullptr if it is not published yet. */
	EYE_NODISCARD EYE_ALWAYS_INLINE T* FrontAt(size_type read) noexcept
	{
		if (read == reader_.writeCacheIdx) [[unlikely]]
		{
			reader_.writeCacheIdx = writer_.publishedWriteIdx.load(std::memory_order_acquire);
			if (read == reader_.writeCacheIdx) [[unlikely]]
				return nullptr;
		}
		return SlotAt(reader_.data, read);
	}

	/* Consumer: destroy the element at `read`. Does not release the slot. */
	EYE_ALWAYS_INLINE void DestroyAt(size_type read) noexcept
	{
		assert(reader_.writeCacheIdx != read && "Pop can only be used after a non-nullptr Front");
		std::destroy_at(SlotAt(reader_.data, read));
	}

private:
	struct alignas(kCacheLineSize) WriterSide
	{
		std::atomic< size_type > publishedWriteIdx{ 0ULL };
		size_type pending{ 0ULL };		// constructed, not yet published (lazy API only)
		size_type readCacheIdx{ 0ULL };
		pointer data{ nullptr };		// copy of the buffer pointer: keeps the hot path on its own line
	} writer_;
	struct alignas(kCacheLineSize) ReaderSide
	{
		std::atomic< size_type > publishedReadIdx{ 0ULL };
		size_type pending{ 0ULL };		// popped, not yet released (lazy API only)
		size_type writeCacheIdx{ 0ULL };
		pointer data{ nullptr };
	} reader_;
	/* cold section */
	Allocator allocator_;
	pointer base_;
};

// ------------------------------------------
// START OF SPSC-QUEUE IMPLEMENTATION
// ------------------------------------------

template<typename T, std::size_t Capacity, typename Allocator>
SPSCQueue< T, Capacity, Allocator >::SPSCQueue(bool touch, const Allocator &allocator)
	: allocator_(allocator),
	  base_(alloc_traits::allocate(allocator_, kBaseCapacity))
{
	void *ptr = base_;
	size_type space = kBaseCapacity * sizeof(T);
	auto data = static_cast< pointer >(std::align(kCacheLineSize, kDataBytes, ptr, space));
	assert(data != nullptr && "Failed to align data: space provided is too small");

	if (touch) // touch every page with future objects
		std::memset(static_cast< void * >(data), 0, kDataBytes);

	writer_.data = data;
	reader_.data = data;
}

template<typename T, std::size_t Capacity, typename Allocator>
SPSCQueue< T, Capacity, Allocator >::~SPSCQueue()
{
	if constexpr (!std::is_trivially_destructible_v< T >)
	{
		const size_type last = WritePos();
		for (size_type i = ReadPos(); i != last; ++i)
			std::destroy_at(SlotAt(reader_.data, i));
	}
	alloc_traits::deallocate(allocator_, base_, kBaseCapacity);
}

/* ---------------------------- producer ---------------------------- */

template<typename T, std::size_t Capacity, typename Allocator>
template<typename... Args>
bool SPSCQueue< T, Capacity, Allocator >::TryEmplace(Args &&... args)
{
	assert(writer_.pending == 0 && "TryEmplace() with uncommitted lazy writes: call CommitWrite() first");
	const size_type write = PublishedWrite();
	if (!EmplaceAt(write, std::forward< Args >(args)...)) [[unlikely]]
		return false;
	writer_.publishedWriteIdx.store(write + 1, std::memory_order_release);
	return true;
}

template<typename T, std::size_t Capacity, typename Allocator>
template<typename... Args>
bool SPSCQueue< T, Capacity, Allocator >::TryEmplaceLazy(Args &&... args)
{
	if (!EmplaceAt(WritePos(), std::forward< Args >(args)...)) [[unlikely]]
		return false;
	++writer_.pending;
	return true;
}

template<typename T, std::size_t Capacity, typename Allocator>
bool SPSCQueue< T, Capacity, Allocator >::TryPush(const T &value)
{
	return TryEmplace(value);
}

template<typename T, std::size_t Capacity, typename Allocator>
bool SPSCQueue< T, Capacity, Allocator >::TryPush(T &&value)
{
	return TryEmplace(std::move(value));
}

template<typename T, std::size_t Capacity, typename Allocator>
bool SPSCQueue< T, Capacity, Allocator >::TryPushLazy(const T &value)
{
	return TryEmplaceLazy(value);
}

template<typename T, std::size_t Capacity, typename Allocator>
bool SPSCQueue< T, Capacity, Allocator >::TryPushLazy(T &&value)
{
	return TryEmplaceLazy(std::move(value));
}

template<typename T, std::size_t Capacity, typename Allocator>
void SPSCQueue< T, Capacity, Allocator >::CommitWrite()
{
	writer_.publishedWriteIdx.store(WritePos(), std::memory_order_release);
	writer_.pending = 0;
}

/* ---------------------------- consumer ---------------------------- */

template<typename T, std::size_t Capacity, typename Allocator>
T* SPSCQueue< T, Capacity, Allocator >::Front()
{
	assert(reader_.pending == 0 && "Front() with uncommitted lazy pops: use FrontLazy() or call CommitRead() first");
	return FrontAt(PublishedRead());
}

template<typename T, std::size_t Capacity, typename Allocator>
T* SPSCQueue< T, Capacity, Allocator >::FrontLazy()
{
	return FrontAt(ReadPos());
}

template<typename T, std::size_t Capacity, typename Allocator>
void SPSCQueue< T, Capacity, Allocator >::Pop()
{
	assert(reader_.pending == 0 && "Pop() with uncommitted lazy pops: call CommitRead() first");
	const size_type read = PublishedRead();
	DestroyAt(read);
	reader_.publishedReadIdx.store(read + 1, std::memory_order_release);
}

template<typename T, std::size_t Capacity, typename Allocator>
void SPSCQueue< T, Capacity, Allocator >::PopLazy()
{
	DestroyAt(ReadPos());
	++reader_.pending;
}

template<typename T, std::size_t Capacity, typename Allocator>
void SPSCQueue< T, Capacity, Allocator >::CommitRead()
{
	reader_.publishedReadIdx.store(ReadPos(), std::memory_order_release);
	reader_.pending = 0;
}

/* ------------------------------ info ------------------------------ */

template<typename T, std::size_t Capacity, typename Allocator>
constexpr auto SPSCQueue< T, Capacity, Allocator >::GetCapacity() noexcept -> size_type
{
	return Capacity;
}

template<typename T, std::size_t Capacity, typename Allocator>
auto SPSCQueue< T, Capacity, Allocator >::GetApproxSize() const noexcept -> size_type
{
	// read must be loaded before write: otherwise the consumer may advance in between
	// and (write - read) wraps around to a huge value
	const size_type read = reader_.publishedReadIdx.load(std::memory_order_acquire);
	const size_type write = writer_.publishedWriteIdx.load(std::memory_order_acquire);
	return std::min(write - read, Capacity);
}

template<typename T, std::size_t Capacity, typename Allocator>
bool SPSCQueue< T, Capacity, Allocator >::IsEmpty() const noexcept
{
	return GetApproxSize() == 0;
}