# box::current

## Назначение

Заголовок: [`box/cxx/current.h`](../../../src/userspace/boxcxx/include/box/cxx/current.h).
C-основа: `box/current.h`.

**Current** — «спина» ввода-вывода BoxOS: единый способ открыть канал по
тегу — экран, журнал, клавиатуру, файл, типизированный поток между
программами. Вместо stdin/stdout/дескрипторов — именованные каналы с
ролью (чтение или запись).

Две формы под одним именем:

- `box::current<std::byte>` (`box::byte_current`) — байтовый/текстовый канал: `write`, `read`, `read_line`, `seek`; `box::screen()`, `box::log()`, `box::keyboard()`, `box::file()`; `box::print`/`println` форматируют в канал через `<format>`;
- `box::current<T>` — типизированный поток кадров (на основе Brook) из тривиально копируемых `T`: `put`, `take`, `close`, `co_await next()`.

`std::print` тоже идёт через консольный Current.

Конец потока — честное значение «писатель закрыл и всё прочитано»
(`CURRENT_CLOSED`), а не EOF Unix.

## Перечисления

### `box::role`

**Краткое описание:** Называет роль канала.

**Параметры:** `read` (= `CURRENT_READ`), `write` (= `CURRENT_WRITE`).

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Пример:** См. `box::file`.

### `box::opening`

**Краткое описание:** Называет режим открытия канала; значения складываются `|`.

**Параметры:**
- `none` — блокирующий, источник должен уже существовать.
- `create` — создать источник, если его нет (поток или файл для записи).
- `nonblock` — `put`/`take` не блокируют: `would_block`, если полно или пусто.
- `fresh` — файл для записи обрезается до нуля при открытии.

**Возвращает:** Неприменимо: перечисление; `operator|`, `operator&`, `any(opening)`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/current.h"

constexpr box::opening Mode = box::opening::create | box::opening::fresh;
```

## Типизированный поток

### `box::current<T>`

**Краткое описание:** Канал кадров типа `T` между программами.

**Параметры:**
- `current()` — пустой.
- `current(const char *tag, role r, opening o = opening::none)` — открывает канал по тегу.
- `T` — тривиально копируемый, не больше 64 КиБ.

**Возвращает:** Неприменимо: класс; только перемещение. `operator bool` — открыт; `caps()` — возможности; `handle()` — `Current *` boxlib.

**Побочные эффекты:** Деструктор отпускает канал.

**Пример:**
```cpp
#include "box/cxx/current.h"

struct Sample {
    float Celsius;
};

bool Report(float Value)
{
    box::current<Sample> Out("sensor:temp", box::role::write, box::opening::create);
    return Out && Out.put(Sample{Value}).has_value();
}
```

### `put`

**Краткое описание:** Записывает один кадр.

**Параметры:** `v` (`const T &`).

**Возвращает:** (`box::status`); ошибка — `process_terminated` (читателя нет), `would_block` (неблокирующий и полный), `invalid_argument` (канал закрыт), `io`.

**Побочные эффекты:** Пишет в поток; блокирует, если поток полон и открыт без `nonblock`.

**Пример:** См. `box::current<T>`.

### `take`

**Краткое описание:** Читает один кадр.

**Параметры:** `out` (`T &`, out).

**Возвращает:** (`box::result<bool>`): `true` — кадр прочитан; `false` — писатель закрыл поток и всё прочитано (это не ошибка); ошибка — `would_block` или другая причина.

**Побочные эффекты:** Забирает кадр.

**Пример:**
```cpp
#include "box/cxx/current.h"

struct Sample {
    float Celsius;
};

float Average()
{
    box::current<Sample> In("sensor:temp", box::role::read);
    float Sum = 0;
    int   Count = 0;
    Sample Got{};
    while (auto R = In.take(Got)) {
        if (!*R) break;   // писатель закрыл поток
        Sum += Got.Celsius;
        Count++;
    }
    return Count ? Sum / static_cast<float>(Count) : 0;
}
```

### `close` / `next`

**Краткое описание:** Объявляет читателю конец потока и ждёт кадр в сопрограмме.

**Параметры:** Нет.

**Возвращает:** `close` — `void`; `next` — объект ожидания, дающий `std::optional<T>` (пусто — конец потока или ошибка).

**Побочные эффекты:** `close` не отпускает дескриптор; `next` засыпает на [executor](executor.md).

**Примечания:** Не больше одного `co_await next()` на канал одновременно; канал должен пережить ожидание.

**Пример:**
```cpp
#include "box/cxx/current.h"

struct Sample {
    float Celsius;
};

box::task<int> Count(box::current<Sample> &In)
{
    int N = 0;
    while (co_await In.next()) N++;
    co_return N;
}
```

## Байтовый канал

### `box::current<std::byte>` / `box::byte_current`

**Краткое описание:** Канал байтов или текста.

**Параметры:** Как у `current<T>`.

**Возвращает:** Неприменимо: класс; только перемещение.

**Побочные эффекты:** Деструктор отпускает канал.

**Пример:** См. `box::screen`.

### `write`

**Краткое описание:** Пишет байты.

**Параметры:** `p` (`const void *`), `n` (`std::size_t`); или `s` (`std::string_view`).

**Возвращает:** (`std::size_t`) принятые байты; 0 при ошибке.

**Побочные эффекты:** Ввод-вывод.

**Пример:**
```cpp
#include "box/cxx/current.h"

void Hello() { box::screen().write("привет\n"); }
```

### `read` / `read_some` / `read_line`

**Краткое описание:** Читают байты или строку.

**Параметры:** `p` (`void *`), `n` (`std::size_t`); `max` (`std::size_t`) для `read_line`, по умолчанию 1024.

**Возвращает:** `read` — (`int`) байты (> 0), 0 — конец, отрицательное — `-ERR_*`; `read_some` — (`box::result<std::size_t>`), значение 0 — конец, ошибка — `would_block`, `invalid_argument` и т. п.; `read_line` — (`std::string`) у клавиатуры — одна отредактированная строка, у файла — до `max` байт; пусто в конце или при ошибке.

**Побочные эффекты:** Забирают данные; блокируют без `nonblock`.

**Примечания:** `read_some` отличает «сейчас нет данных» (`would_block`) от «поток кончился» (0) в неблокирующем режиме.

**Пример:**
```cpp
#include "box/cxx/current.h"

std::string Ask()
{
    box::print(box::screen(), "имя: ");
    return box::keyboard().read_line();
}
```

### `flush` / `seek` / `tell` / `size` / `span`

**Краткое описание:** Сбрасывают буфер, двигают и сообщают позицию, возвращают и задают длину канала.

**Параметры:** `off` (`std::uint64_t`) для `seek`; `n` (`std::uint64_t`) для `span`.

**Возвращает:** `flush` — `void`; `seek`, `span` — (`box::status`), ошибка `invalid_operation`, если у источника нет позиции или длины; `tell` — (`std::uint64_t`); `size` — (`box::result<std::uint64_t>`).

**Побочные эффекты:** `span` обрезает или удлиняет файл (новые байты — нули), позиция за новым концом возвращается к концу.

**Пример:**
```cpp
#include "box/cxx/current.h"

std::uint64_t Length(const char *Name)
{
    box::byte_current In = box::file(Name, box::role::read, box::opening::none);
    return In.size().value_or(0);
}
```

## Стандартные каналы

### `box::screen` / `box::log` / `box::keyboard` / `box::file`

**Краткое описание:** Открывают экран, журнал, клавиатуру и файл.

**Параметры:**
- `screen()` — консоль, запись.
- `log()` — `log:serial`, запись в журнал (последовательный порт).
- `keyboard(opening o = none)` — клавиатура, чтение.
- `file(const char *path, role r, opening o = opening::create)` — файл TagFS по имени (префикс `file:` добавляется сам).

**Возвращает:** (`byte_current`).

**Побочные эффекты:** Открывают канал.

**Пример:**
```cpp
#include "box/cxx/current.h"

void Note(std::string_view Text)
{
    box::byte_current Out = box::file("notes.txt", box::role::write);
    Out.write(Text);
}
```

### `box::print` / `box::println`

**Краткое описание:** Форматируют текст и пишут его в канал.

**Параметры:**
- `ch` (`byte_current &` или временный `byte_current &&`).
- `fmt` (`std::format_string<Args...>`), `args`.

**Возвращает:** `void`.

**Побочные эффекты:** Выделяют строку и пишут её; `println` добавляет `\n`; `println(ch)` — только перевод строки.

**Пример:**
```cpp
#include "box/cxx/current.h"

void Debug(int Value) { box::println(box::log(), "значение = {}", Value); }
```
