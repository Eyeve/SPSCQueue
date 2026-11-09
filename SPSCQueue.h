#pragma once

#include <atomic>
#include <bit>
#include <memory>

#if defined(__has_cpp_attribute) && __has_cpp_attribute(nodiscard)
#define EYE_NODISCARD [[nodiscard]]
#else
#define EYE_NODISCARD
#endif

// ------------------------------------------
// START OF SPCS-QUEUE INTERFACE
// ------------------------------------------

template<typename T, std::size_t Capacity, typename Allocator = std::allocator< T > >
class SPSCQueue
{
	using alloc_type = Allocator;
	using alloc_traits = std::allocator_traits< Allocator >;

public:
	using value_type = T;
	using pointer = alloc_traits::pointer;
	using const_pointer = alloc_traits::const_pointer;
	using reference = T&;
	using const_reference = const T&;
	using difference_type = std::ptrdiff_t;
	using size_type = std::size_t;

private:
	static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");
	static constexpr std::size_t kMask = Capacity - 1;
#ifdef __cpp_lib_hardware_interference_size
	static constexpr size_t kCacheLineSize = std::hardware_destructive_interference_size;
#else
	static constexpr size_t kCacheLineSize = 64;
#endif

public:
	explicit SPSCQueue(const Allocator &allocator = Allocator());

	~SPSCQueue();

	/* non-copyable and non-movable */
	SPSCQueue(const SPSCQueue &) = delete;
	SPSCQueue &operator=(const SPSCQueue &) = delete;


	/* producer thread only */
	template<typename... Args>
	EYE_NODISCARD bool TryEmplace(Args &&... args);
	EYE_NODISCARD bool TryPush(const T &value);
	EYE_NODISCARD bool TryPush(T &&value);

	/* consumer thread only */
	EYE_NODISCARD T* Front();
	void Pop();

	/* info */
	EYE_NODISCARD static constexpr size_type GetCapacity() noexcept;
	EYE_NODISCARD size_type GetApproxSize() const noexcept;

private:
	pointer data_;
	struct WriterSide
	{
		std::atomic< size_type > writeIdx{ 0ULL };
		size_type readCacheIdx{ 0ULL };
	} writer_;
	struct ReaderSide
	{
		std::atomic< size_type > readIdx{ 0ULL };
		size_type writeCacheIdx{ 0ULL };
	} reader_;
	Allocator allocator_;
};

// ------------------------------------------
// START OF SPCS-QUEUE IMPLEMENTATION
// ------------------------------------------

template<typename T, std::size_t Capacity, typename Allocator>
SPSCQueue< T, Capacity, Allocator >::SPSCQueue(const Allocator &allocator)
	: allocator_(allocator)
{
	data_ = std::allocator_traits< Allocator >::allocate(allocator_, Capacity);
}

template<typename T, std::size_t Capacity, typename Allocator>
SPSCQueue< T, Capacity, Allocator >::~SPSCQueue()
{
	const size_type readIdx = reader_.readIdx.load(std::memory_order_relaxed);
	const size_type writeIdx = writer_.writeIdx.load(std::memory_order_relaxed);

	for (size_type i = readIdx; i < writeIdx; ++i)
		alloc_traits::destroy(allocator_, &data_[i & kMask]);

	std::allocator_traits< Allocator >::deallocate(allocator_, data_, Capacity);
}

template<typename T, std::size_t Capacity, typename Allocator>
template<typename... Args>
bool SPSCQueue< T, Capacity, Allocator >::TryEmplace(Args &&... args)
{
	const size_type writeIdx = writer_.writeIdx.load(std::memory_order_relaxed);
	if (writeIdx - writer_.readCacheIdx == Capacity)
	{
		writer_.readCacheIdx = reader_.readIdx.load(std::memory_order_acquire);
		if (writeIdx - writer_.readCacheIdx == Capacity)
			return false;
	}

	alloc_traits::construct(allocator_, &data_[writeIdx & kMask], std::forward< Args >(args)...);
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
	if (readIdx == reader_.writeCacheIdx)
	{
		reader_.writeCacheIdx = writer_.writeIdx.load(std::memory_order_acquire);
		if (readIdx == reader_.writeCacheIdx)
			return nullptr;
	}

	return &data_[readIdx & kMask];
}

template<typename T, std::size_t Capacity, typename Allocator>
void SPSCQueue< T, Capacity, Allocator >::Pop()
{
	const size_type readIdx = reader_.readIdx.load(std::memory_order_relaxed);
	assert(
		writer_.writeIdx.load(std::memory_order_relaxed) != readIdx &&
		".Pop() can only be used after non-nullptr .Front()"
	);
	alloc_traits::destroy(allocator_, &data_[readIdx & kMask]);
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
	return writeIdx - readIdx;
}
