#pragma once

#include <atomic>
#include <bit>
#include <memory>
#include <cstring>

#if defined(__has_cpp_attribute) && __has_cpp_attribute(nodiscard)
#define EYE_NODISCARD [[nodiscard]]
#else
#define EYE_NODISCARD
#endif

#if defined(__GNUC__) || defined(__clang__)
#define EYE_ALWAYS_INLINE [[gnu::always_inline]] inline
#elif defined(_MSC_VER)
#define EYE_ALWAYS_INLINE __forceinline
#else
#define EYE_ALWAYS_INLINE inline
#endif

// ------------------------------------------
// START OF SPCS-QUEUE INTERFACE
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
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
	static constexpr size_type kCacheLineSize = 128;
#else
	static constexpr size_type kCacheLineSize = 64;
#endif

	/*
	 * The allocator does not guarantee data alignment,
	 * so need to use padding on both sides of the buffer
	 */
	static constexpr size_type kPadding = (kCacheLineSize - 1) / sizeof(T) + 1;
	static constexpr size_type kBaseCapacity = Capacity + kPadding * 2;

	static_assert(std::has_single_bit(Capacity), "Capacity must be power of two");
	static_assert(std::has_single_bit(kCacheLineSize), "Cache line size must be power of two");
	static_assert(std::is_nothrow_destructible_v< T >, "T must be nothrow destructible");
	static_assert(kCacheLineSize % alignof(T) == 0, "T can't be aligned to start of cache line");

public:
	explicit SPSCQueue(bool touch = true, const Allocator &allocator = Allocator());
	~SPSCQueue();

	/* non-copyable and non-movable */
	SPSCQueue(const SPSCQueue &) = delete;
	SPSCQueue &operator=(const SPSCQueue &) = delete;

	/* producer thread only */
	template<typename... Args>
	EYE_NODISCARD bool TryEmplace(Args &&... args);
	EYE_NODISCARD
	EYE_NODISCARD bool TryPush(const T &value);
	EYE_NODISCARD bool TryPush(T &&value);

	/* consumer thread only */
	EYE_NODISCARD T* Front();
	void Pop();

	/* info */
	EYE_NODISCARD static constexpr size_type GetCapacity() noexcept;
	EYE_NODISCARD size_type GetApproxSize() const noexcept;
	EYE_NODISCARD bool IsEmpty() const noexcept;

private:
	struct alignas(kCacheLineSize) WriterSide
	{
		std::atomic< size_type > writeIdx{ 0ULL };
		size_type readCacheIdx{ 0ULL };
		pointer data{ nullptr };
	} writer_;
	struct alignas(kCacheLineSize) ReaderSide
	{
		std::atomic< size_type > readIdx{ 0ULL };
		size_type writeCacheIdx{ 0ULL };
		pointer data{ nullptr };
	} reader_;
	/* cold section */
	Allocator allocator_;
	pointer base_;
};

// ------------------------------------------
// START OF SPCS-QUEUE IMPLEMENTATION
// ------------------------------------------

template<typename T, std::size_t Capacity, typename Allocator>
SPSCQueue< T, Capacity, Allocator >::SPSCQueue(bool touch, const Allocator &allocator)
	: allocator_(allocator),
	  base_(alloc_traits::allocate(allocator_, kBaseCapacity))
{
	static constexpr size_type kDataSize = Capacity * sizeof(T);
	void* ptr = base_;
	size_type space = kBaseCapacity * sizeof(T);
	pointer data = static_cast<pointer>(std::align(kCacheLineSize, kDataSize, ptr, space));

	assert(data != nullptr && "Failed to align data: space provided is too small");

	if (touch) // touch every page with future objects
		std::memset(static_cast< void * >(data), 0, Capacity * sizeof(T));

	writer_.data = data;
	reader_.data = data;
}

template<typename T, std::size_t Capacity, typename Allocator>
SPSCQueue< T, Capacity, Allocator >::~SPSCQueue()
{
	if constexpr (!std::is_trivially_destructible_v< T >)
	{
		// TODO: maybe add assert that reader_.data == writer_.data
		const size_type readIdx = reader_.readIdx.load(std::memory_order_acquire);
		const size_type writeIdx = writer_.writeIdx.load(std::memory_order_acquire);

		for (size_type i = readIdx; i != writeIdx; ++i)
			std::destroy_at(reader_.data + (i & kMask));
	}
	std::allocator_traits< Allocator >::deallocate(allocator_, base_, kBaseCapacity);
}

template<typename T, std::size_t Capacity, typename Allocator>
template<typename... Args>
bool SPSCQueue< T, Capacity, Allocator >::TryEmplace(Args &&... args)
{
	const size_type writeIdx = writer_.writeIdx.load(std::memory_order_relaxed);
	if (writeIdx - writer_.readCacheIdx == Capacity) [[unlikely]]
	{
		writer_.readCacheIdx = reader_.readIdx.load(std::memory_order_acquire);
		if (writeIdx - writer_.readCacheIdx == Capacity) [[unlikely]]
			return false;
	}

	alloc_traits::construct(allocator_, writer_.data + (writeIdx & kMask), std::forward< Args >(args)...);
	writer_.writeIdx.store(writeIdx + 1, std::memory_order_release);
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
	return TryEmplace(std::forward< T >(value));
}

template<typename T, std::size_t Capacity, typename Allocator>
T* SPSCQueue< T, Capacity, Allocator >::Front()
{
	const size_type readIdx = reader_.readIdx.load(std::memory_order_relaxed);
	if (readIdx == reader_.writeCacheIdx) [[unlikely]]
	{
		reader_.writeCacheIdx = writer_.writeIdx.load(std::memory_order_acquire);
		if (readIdx == reader_.writeCacheIdx) [[unlikely]]
			return nullptr;
	}

	return reader_.data + (readIdx & kMask);
}

template<typename T, std::size_t Capacity, typename Allocator>
void SPSCQueue< T, Capacity, Allocator >::Pop()
{
	const size_type readIdx = reader_.readIdx.load(std::memory_order_relaxed);
	assert(
		writer_.writeIdx.load(std::memory_order_relaxed) != readIdx &&
		".Pop() can only be used after non-nullptr .Front()"
	);
	alloc_traits::destroy(allocator_, &reader_.data[readIdx & kMask]);
	reader_.readIdx.store(readIdx + 1, std::memory_order_release);
}

template<typename T, std::size_t Capacity, typename Allocator>
constexpr auto SPSCQueue< T, Capacity, Allocator >::GetCapacity() noexcept -> size_type
{
	return Capacity;
}

template<typename T, std::size_t Capacity, typename Allocator>
auto SPSCQueue< T, Capacity, Allocator >::GetApproxSize() const noexcept -> size_type
{
	const size_type readIdx = reader_.readIdx.load(std::memory_order_acquire);
	const size_type writeIdx = writer_.writeIdx.load(std::memory_order_acquire);
	// TODO: test branch prediction
	return std::min(writeIdx - readIdx, Capacity);
}

template<typename T, std::size_t Capacity, typename Allocator>
bool SPSCQueue< T, Capacity, Allocator >::IsEmpty() const noexcept
{
	return GetApproxSize() == 0;
}
