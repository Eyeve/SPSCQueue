# SPSCQueue

Ограниченная (bounded) lock-free очередь типа **single-producer / single-consumer**: ровно один поток-производитель и ровно один поток-потребитель.

```cpp
template<typename T, std::size_t Capacity, typename Allocator = std::allocator<T>>
class SPSCQueue;
```

## Кратко

- `Capacity` — обязана быть степенью двойки (проверяется `static_assert`).
- Индексы — монотонно растущие счётчики, работают через переполнение `size_t`.
- Каждая сторона кэширует индекс противоположной стороны, чтобы лишний раз не трогать общий атомик.
- `writer_` и `reader_` выровнены по кэш-линии (128 байт на x86-64/ARM64, иначе 64), чтобы избежать false sharing.
- `T` должен быть **nothrow-destructible** и выровнен под начало кэш-линии.

## Два режима

Обе стороны имеют два режима, и **смешивать их, пока что-то не закоммичено, нельзя**.

### Immediate
Каждая операция сразу же публикуется второй стороне.

```cpp
// producer
queue.TryEmplace(...);   // или TryPush(lvalue), TryPush(rvalue)
// consumer
T* p = queue.Front();    // nullptr если пусто
if (p) queue.Pop();
```

### Lazy
Публикация батча делается одним `release`-store в `Commit*()`.

```cpp
// producer
queue.TryEmplaceLazy(a);
queue.TryEmplaceLazy(b);
queue.CommitWrite();     // всё вышеописанное становится видимым потребителю

// consumer
while (T* p = queue.FrontLazy()) {
    consume(*p);
    queue.PopLazy();
}
queue.CommitRead();      // слоты становятся доступны производителю
```

- Лениво записанные элементы **невидимы** потребителю до `CommitWrite()`.
- Лениво освобождённые слоты **не переиспользуются** производителем до `CommitRead()`.

## Контракты

### Потоки

| Метод | Кто может вызывать |
|---|---|
| `TryEmplace`, `TryPush`, `TryEmplaceLazy`, `TryPushLazy`, `CommitWrite` | только producer |
| `Front`, `Pop`, `FrontLazy`, `PopLazy`, `CommitRead` | только consumer |
| `GetCapacity`, `GetApproxSize`, `IsEmpty` | любой поток |
| `~SPSCQueue` | никто не должен пользоваться очередью |

### Режимы

- Нельзя вызывать `TryEmplace*` / `TryPush*`, пока `writer_.pending != 0` (то есть пока не вызван `CommitWrite()`).
- Нельзя вызывать `Front()` / `Pop()` пока `reader_.pending != 0` (то есть пока не вызван `CommitRead()`).
- `Pop()` / `PopLazy()` допустимо вызывать только после успешного (`!= nullptr`) `Front()` / `FrontLazy()`.

Всё это проверяется через `assert`, то есть в release-сборке — **UB при нарушении**.

### Параметры шаблона

- `Capacity` — степень двойки, не 0.
- `T` — `nothrow_destructible`.
- `alignof(T)` делит размер кэш-линии.

## Гарантии исключений

| Операция | Спецификация | Гарантия |
|---|---|---|
| `SPSCQueue(bool, const Allocator&)` | может бросить (`bad_alloc`, исключение из копии аллокатора) | **strong**: при исключении ничего не утекло, объект не создан |
| `~SPSCQueue()` | `noexcept` | не бросает |
| `TryEmplace`, `TryEmplaceLazy`, `TryPush*` | `noexcept(is_nothrow_constructible_v<T, Args&&...>)` | **strong**: если конструктор `T` бросит, состояние очереди не меняется (индексы не двигаются, слот не тронут) |
| `CommitWrite`, `CommitRead` | `noexcept` | не бросает |
| `Front`, `FrontLazy` | `noexcept` | не бросает, возвращает `nullptr` если пусто |
| `Pop`, `PopLazy` | `noexcept` | не бросает (требуется nothrow-destructible `T`) |
| `GetCapacity`, `GetApproxSize`, `IsEmpty` | `noexcept` | не бросает |

Итого:

- **Единственное место, где может вылететь исключение** — конструктор очереди (выделение памяти) и конструктор самого `T` внутри `Try*`.
- В `Try*`-операциях обеспечена **strong**-гарантия: при броске исключения очередь остаётся в точности в том же состоянии.
- Всё, что делает `Pop*`, `Commit*`, `Front*`, — `noexcept`.

## Пример

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

## Замечания по дизайну

- `touch = true` (по умолчанию) зануляет буфер через `memset`, чтобы сразу выделить страницы и избежать page fault на горячем пути.
- Память под объекты выделяется аллокатором, но сами объекты конструируются через `std::construct_at` и разрушаются через `std::destroy_at` — аллокатор используется только как источник сырой памяти.
- Деструктор очереди уничтожает всё, что осталось «в полёте» — как неопубликованное (`pending` у producer), так и неосвобождённое (`pending` у consumer).
- `GetApproxSize()` сначала читает `read`, потом `write` — иначе при одновременном прогрессе потребителя разность может уйти в минус и обернуться в огромное число.