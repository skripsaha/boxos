# box::brook

## Назначение

Заголовок: [`box/cxx/brook.h`](../../../src/userspace/boxcxx/include/box/cxx/brook.h).
C-основа: `box/brook.h`.

**Brook** — упорядоченный поток кадров от одного писателя к одному
читателю через общее кольцо памяти. `box::current<T>` ([current](current.md))
оборачивает такой поток в общий слой Current; `box::brook<T>` — сам Brook
со всеми его возможностями: явные роли, форма кольца и счётчики,
блокирующие / неблокирующие / ограниченные по времени `push` и `pop`,
диапазон для `for`, ожидания сопрограмм.

Кадры — тривиально копируемые, с конструктором по умолчанию, от 8 до 65536
байт. Конец потока — писатель ушёл, и кольцо опустело, а не EOF Unix.

## Класс brook

### `box::brook<T>`

**Краткое описание:** Поток кадров типа `T`.

**Параметры:** Создаётся через `writer` или `reader`; `brook()` — пустой.

**Возвращает:** Неприменимо: класс; только перемещение. `operator bool` — открыт; `handle()` — `Brook *`.

**Побочные эффекты:** Деструктор отпускает поток.

**Пример:** См. `writer`.

### `brook::writer` / `brook::reader`

**Краткое описание:** Создают поток как писатель или подключаются к нему как читатель.

**Параметры:**
- `tag` (`const char *`): имя потока.
- `capacity` (`std::uint32_t`): для писателя — число кадров, степень двойки от 2 до 16384.
- `stream` (`bool`): долгоживущий поток демона, переживающий смену собеседника (без окончания при уходе писателя); по умолчанию `false` — один сеанс с чистым окончанием.

**Возвращает:** (`brook`) пустой при неудаче. Читатель отвергается, если размер кадра не совпадает с размером у создателя.

**Побочные эффекты:** Писатель создаёт кольцо; читатель подключается.

**Пример:**
```cpp
#include "box/cxx/brook.h"

struct Tick {
    std::uint64_t Count;
};

void Producer()
{
    auto Out = box::brook<Tick>::writer("metric:tick", 1024);
    for (std::uint64_t I = 0; Out && I < 100; I++)
        if (!Out.push(Tick{I})) break;   // читатель ушёл
}

std::uint64_t Consumer()
{
    auto In = box::brook<Tick>::reader("metric:tick");
    std::uint64_t Last = 0;
    for (const Tick &Each : In) Last = Each.Count;   // до ухода писателя
    return Last;
}
```

### `frame_size` / `capacity` / `available` / `free`

**Краткое описание:** Возвращают размер кадра, ёмкость, число готовых кадров и свободных мест.

**Параметры:** Нет.

**Возвращает:** (`std::uint32_t`); у пустого потока — 0.

**Побочные эффекты:** Нет (читаются из общего заголовка без системных вызовов).

**Пример:**
```cpp
#include "box/cxx/brook.h"

struct Tick {
    std::uint64_t Count;
};

bool Backlog(const box::brook<Tick> &In) { return In.available() > In.capacity() / 2; }
```

### `push` / `try_push` / `push_for`

**Краткое описание:** Записывают кадр с ожиданием места, без ожидания или с пределом времени.

**Параметры:** `v` (`const T &`); `timeout_ms` (`std::uint32_t`) для `push_for`.

**Возвращает:** `push` — (`bool`) `false`, когда читатель ушёл; `try_push`, `push_for` — (`int`) `OK`, `-ERR_WOULD_BLOCK` (полно), `-ERR_TIMEOUT`, `-ERR_PROCESS_TERMINATED` (читатель ушёл), `-ERR_INVALID_ARGUMENT` (пустой поток).

**Побочные эффекты:** Пишут в кольцо; `push` блокирует при полном кольце (обратное давление).

**Пример:** См. `writer`.

### `pop` / `try_pop` / `pop_for`

**Краткое описание:** Читают кадр с ожиданием, без ожидания или с пределом времени.

**Параметры:** `out` (`T &`, out); `timeout_ms` для `pop_for`.

**Возвращает:** `pop` — (`bool`) `false`, когда писатель ушёл и кольцо пусто; `try_pop`, `pop_for` — (`int`) коды как у `try_push`.

**Побочные эффекты:** Забирают кадр.

**Пример:**
```cpp
#include "box/cxx/brook.h"

struct Tick {
    std::uint64_t Count;
};

int Peek(box::brook<Tick> &In, Tick &Out) { return In.try_pop(Out); }
```

### `next` / `send`

**Краткое описание:** Ждут кадр или место для записи в сопрограмме.

**Параметры:** `v` (`const T &`) для `send`.

**Возвращает:** `next` — объект ожидания, дающий `std::optional<T>` (пусто — конец потока); `send` — объект ожидания, дающий `bool` (`false` — читатель ушёл).

**Побочные эффекты:** Засыпают на [executor](executor.md); вне его вызывать нельзя.

**Пример:**
```cpp
#include "box/cxx/brook.h"

struct Tick {
    std::uint64_t Count;
};

box::task<void> Relay(box::brook<Tick> &In, box::brook<Tick> &Out)
{
    while (auto Got = co_await In.next())
        if (!co_await Out.send(*Got)) break;
}
```

### `begin` / `end`

**Краткое описание:** Делают читателя входным диапазоном.

**Параметры:** Нет.

**Возвращает:** Итератор и `std::default_sentinel_t`; каждый шаг ждёт кадр, диапазон кончается с уходом писателя.

**Побочные эффекты:** Блокирует на каждом шаге.

**Пример:** См. `writer`.
