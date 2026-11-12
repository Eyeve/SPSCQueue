# SPSCQueue

A bounded lock-free queue of **single-producer / single-consumer** type: exactly one producer thread and exactly one consumer thread.

```cpp
template<typename T, std::size_t Capacity, typename Allocator = std::allocator<T>>
class SPSCQueue;
```

## Overview

- `Capacity` must be a power of two (enforced by `static_assert`).
- Indices are monotonically increasing counters that work through `size_t` wraparound.
- Each side caches the opposite side's index to avoid touching the shared atomic unnecessarily.
- `writer_` and `reader_` are aligned to a cache line (128 bytes on x86-64/ARM64, otherwise 64) to avoid false sharing.
- `T` must be **nothrow-destructible** and aligned to the start of a cache line.

## Two modes

Both sides have two modes, and **you cannot mix them while something is uncommitted**.

### Immediate
Each operation is published to the other side immediately.

```cpp
// producer
queue.TryEmplace(...);   // or TryPush(lvalue), TryPush(rvalue)
// consumer
T* p = queue.Front();    // nullptr if empty
if (p) queue.Pop();
```

### Lazy
A batch is published with a single `release`-store in `Commit*()`.

```cpp
// producer
queue.TryEmplaceLazy(a);
queue.TryEmplaceLazy(b);
queue.CommitWrite();     // everything above becomes visible to the consumer

// consumer
while (T* p = queue.FrontLazy()) {
    consume(*p);
    queue.PopLazy();
}
queue.CommitRead();      // slots become available to the producer
```

- Lazily written elements are **invisible** to the consumer until `CommitWrite()`.
- Lazily freed slots are **not reused** by the producer until `CommitRead()`.

## Contracts

### Threads

| Method | Who may call it |
|---|---|
| `TryEmplace`, `TryPush`, `TryEmplaceLazy`, `TryPushLazy`, `CommitWrite` | producer only |
| `Front`, `Pop`, `FrontLazy`, `PopLazy`, `CommitRead` | consumer only |
| `GetCapacity`, `GetApproxSize`, `IsEmpty` | any thread |
| `~SPSCQueue` | no one must be using the queue |

### Modes

- You must not call `TryEmplace*` / `TryPush*` while `writer_.pending != 0` (that is, until `CommitWrite()` has been called).
- You must not call `Front()` / `Pop()` while `reader_.pending != 0` (that is, until `CommitRead()` has been called).
- `Pop()` / `PopLazy()` may only be called after a successful (`!= nullptr`) `Front()` / `FrontLazy()`.

All of this is checked via `assert`, meaning that in a release build it is **UB to violate these rules**.

### Template parameters

- `Capacity` — a power of two, not 0.
- `T` — `nothrow_destructible`.
- `alignof(T)` divides the cache-line size.

## Exception guarantees

| Operation | Specification | Guarantee |
|---|---|---|
| `SPSCQueue(bool, const Allocator&)` | may throw (`bad_alloc`, exception from the allocator copy) | **strong**: on exception nothing leaked, the object was not created |
| `~SPSCQueue()` | `noexcept` | does not throw |
| `TryEmplace`, `TryEmplaceLazy`, `TryPush*` | `noexcept(is_nothrow_constructible_v<T, Args&&...>)` | **strong**: if `T`'s constructor throws, the queue state is unchanged (indices do not move, the slot is untouched) |
| `CommitWrite`, `CommitRead` | `noexcept` | does not throw |
| `Front`, `FrontLazy` | `noexcept` | does not throw, returns `nullptr` if empty |
| `Pop`, `PopLazy` | `noexcept` | does not throw (requires nothrow-destructible `T`) |
| `GetCapacity`, `GetApproxSize`, `IsEmpty` | `noexcept` | does not throw |

Summary:

- **The only places where an exception can escape** are the queue constructor (memory allocation) and `T`'s constructor inside `Try*`.
- The `Try*` operations provide the **strong** guarantee: if an exception is thrown, the queue remains in exactly the same state.
- Everything `Pop*`, `Commit*`, `Front*` do is `noexcept`.

## Example

```cpp
SPSCQueue<Message, 1024> q;

// producer thread
q.TryPushLazy(Message{1});
q.TryPushLazy(Message{2});
q.CommitWrite();

// consumer thread
while (auto* m = q.FrontLazy()) {
    handle(*m);
    q.PopLazy();
}
q.CommitRead();
```

## Design notes

- `touch = true` (the default) zeroes the buffer via `memset` to fault in pages immediately and avoid page faults on the hot path.
- Memory for the objects is allocated by the allocator, but the objects themselves are constructed via `std::construct_at` and destroyed via `std::destroy_at` — the allocator is used only as a source of raw memory.
- The queue destructor destroys everything still "in flight" — both unpublished (producer's `pending`) and unreleased (consumer's `pending`).
- `GetApproxSize()` reads `read` first, then `write` — otherwise, with the consumer progressing concurrently, the difference could go negative and wrap around to a huge number.