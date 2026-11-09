#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <memory>

// ------------------------------------------
// START OF SPCS-QUEUE INTERFACE
// ------------------------------------------

template<typename T, std::size_t Capacity, typename Allocator = std::allocator<T>>
class SPSCQueue
{
public:
	// using value_type = T;
	// using reference = T&;
	// using const_reference = const T&;
	// using difference_type = std::ptrdiff_t;
	using size_type = std::size_t;

private:
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");
	static constexpr std::size_t kMask = Capacity - 1;
	// static constexpr std::size_t kCacheLineSize = 64;

public:
    explicit SPSCQueue(const Allocator& allocator = Allocator());
	~SPSCQueue();

    /* non-copyable and non-movable */
    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    /* producer thread only */
    [[nodiscard]] bool TryPush(const T& value);
    // [[nodiscard]] bool try_push(T&& value);
    // [[nodiscard]] std::size_t try_push_n(const T *src, std::size_t n);

    /* consumer thread only */
    [[nodiscard]] bool TryPop(T& out);
    // [[nodiscard]] std::size_t try_pop_n(T *dst, std::size_t n);

    /* info */
    [[nodiscard]] static constexpr size_type GetCapacity() noexcept;
    // [[nodiscard]] std::size_t size_approx() const noexcept;

private:
    T* data_;
	struct WriterSide
	{
		std::atomic<size_type> writeIdx{0ULL};
		size_type readCacheIdx{0ULL};
	} writer_;
	struct ReaderSide
	{
		std::atomic<size_type> readIdx{0ULL};
		size_type writeCacheIdx{0ULL};
	} reader_;
	Allocator allocator_;
};

// ------------------------------------------
// START OF SPCS-QUEUE IMPLEMENTATION
// ------------------------------------------

template<typename T, std::size_t Capacity, typename Allocator>
SPSCQueue<T, Capacity, Allocator>::SPSCQueue(const Allocator& allocator)
    : allocator_(allocator)
{
	data_ = std::allocator_traits<Allocator>::allocate(allocator_, Capacity);
}

template<typename T, std::size_t Capacity, typename Allocator>
SPSCQueue< T, Capacity, Allocator >::~SPSCQueue()
{
	std::allocator_traits<Allocator>::deallocate(allocator_, data_, Capacity);
}

template<typename T, std::size_t Capacity, typename Allocator>
bool SPSCQueue<T, Capacity, Allocator>::TryPush(const T& value)
{
	const size_type writeIdx = writer_.writeIdx.load(std::memory_order_relaxed);
	if (writeIdx - writer_.readCacheIdx == Capacity)
	{
		writer_.readCacheIdx = reader_.readIdx.load(std::memory_order_acquire);
		if (writeIdx - writer_.readCacheIdx == Capacity)
			return false;
	}

	new (&data_[writeIdx & kMask]) T(value);
	writer_.writeIdx.store(writeIdx + 1, std::memory_order_release);
	return true;
}

template<typename T, std::size_t Capacity, typename Allocator>
bool SPSCQueue<T, Capacity, Allocator>::TryPop(T& out)
{
	const size_type readIdx = reader_.readIdx.load(std::memory_order_relaxed);
	if (readIdx == reader_.writeCacheIdx)
	{
		reader_.writeCacheIdx = writer_.writeIdx.load(std::memory_order_acquire);
		if (readIdx == reader_.writeCacheIdx)
			return false;
	}

	T& temp = data_[readIdx & kMask];
	out = temp;
	temp.~T();
	reader_.readIdx.store(readIdx + 1, std::memory_order_release);
	return true;
}

template<typename T, std::size_t Capacity, typename Allocator>
constexpr auto SPSCQueue<T, Capacity, Allocator>::GetCapacity() noexcept -> size_type
{
	return Capacity;
}
