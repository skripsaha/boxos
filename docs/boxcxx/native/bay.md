# box::bay

## Назначение

Заголовок: [`box/cxx/bay.h`](../../../src/userspace/boxcxx/include/box/cxx/bay.h).
C-основа: `box/bay.h`.

**Bay** — общая память между кабинами: область физических страниц с
именем-тегом и счётчиком ссылок, которую ядро отображает в каждую кабину,
открывшую тот же тег. Это ответ BoxOS на разделяемую память, а не копия
`mmap` Unix.

- `box::bay<T>` — типизированный непрерывный вид на область: создание и открытие, доступ к элементам, настоящий `std::ranges::contiguous_range` (алгоритмы и виды работают прямо по общей памяти), `bay<const T>` — отображение только для чтения, шифрованные области TME-MK;
- `box::shared_object<T>` — один общий объект `T` в Bay.

Элементы тривиально копируемые: страницы — сырые общие байты, никакие
конструкторы и деструкторы над ними не выполняются. Выравнивание — не
больше 4 КиБ. Заявка снимается, когда вид уничтожается; страницы
возвращаются системе, когда уходит последняя заявка во всех кабинах.

## bay

### `box::bay<T>`

**Краткое описание:** Типизированный вид на общую область памяти.

**Параметры:** Создаётся фабриками; `bay()` — пустой.

**Возвращает:** Неприменимо: класс; только перемещение.

**Побочные эффекты:** Деструктор снимает заявку.

**Пример:** См. `bay::create`.

### `bay::create` / `bay::open`

**Краткое описание:** Создают новую область под тегом или открывают существующую.

**Параметры:**
- `tag` (`const char *`).
- `count` (`size_type`): число элементов — для `create`.

**Возвращает:** (`bay`) пустой при неудаче (в том числе при переполнении размера). `create` есть только у записываемого типа; `bay<const T>::open` отображает страницы только для чтения. Размер открытой области наследуется от создателя.

**Побочные эффекты:** Отображают страницы в кабину.

**Пример:**
```cpp
#include <algorithm>

#include "box/cxx/bay.h"

struct Cell {
    std::uint32_t Colour;
};

void Paint()
{
    auto Grid = box::bay<Cell>::create("demo:grid", 64 * 64);
    if (!Grid) return;
    std::ranges::fill(Grid, Cell{0x2040A0});
}

std::size_t Blue()
{
    auto View = box::bay<const Cell>::open("demo:grid");
    return static_cast<std::size_t>(std::ranges::count_if(View, [](const Cell &C) { return C.Colour == 0x2040A0; }));
}
```

### `bay::create_for`

**Краткое описание:** Создаёт область для одной конкретной программы.

**Параметры:**
- `tag` (`const char *`), `count` (`size_type`).
- `program_pid`, `program_generation` (`std::uint32_t`): точная идентичность программы, которой разрешено открыть область.

**Возвращает:** (`std::expected<bay, box::error>`); ошибка — `invalid_argument` (нулевой размер), `already_exists` (тег занят — эта фабрика никогда не открывает существующую), другая причина.

**Побочные эффекты:** Создаёт область.

**Типичное применение:** Сервер, который делает память для программы и сообщает ей имя (так дисплей создаёт области и блоки `painter::gui`).

**Пример:**
```cpp
#include "box/cxx/bay.h"

bool ForClient(std::uint32_t Pid, std::uint32_t Generation)
{
    return box::bay<std::byte>::create_for("demo:client", 4096, Pid, Generation).has_value();
}
```

### `bay::create_encrypted` / `bay::open_encrypted`

**Краткое описание:** Создают или открывают область, страницы которой зашифрованы в памяти ключом TME-MK.

**Параметры:** Как у `create` / `open`.

**Возвращает:** (`bay`); без активного TME-MK — пустой вид: тихой замены на незашифрованную память нет, решение о запасном варианте — за вызывающим. Зашифрованную область можно открыть только `open_encrypted`.

**Побочные эффекты:** Как у `create` / `open`.

**Пример:**
```cpp
#include "box/cxx/bay.h"

struct Key {
    std::uint8_t Bytes[32];
};

box::bay<Key> Vault()
{
    auto Secure = box::bay<Key>::create_encrypted("vault:keys", 16);
    if (!Secure) Secure = box::bay<Key>::create("vault:keys", 16);   // осознанный запасной вариант
    return Secure;
}
```

### `operator bool` / `encrypted` / `read_only` / `size` / `size_bytes` / `empty`

**Краткое описание:** Сообщают, открыт ли вид, зашифрован ли, только ли для чтения, и его размер.

**Параметры:** Нет.

**Возвращает:** `bool`; `size` — логическое число элементов, которое запросил создатель (не округлённое до страниц); `size_bytes` — байты.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/bay.h"

std::size_t Bytes(const box::bay<std::byte> &Region) { return Region ? Region.size_bytes() : 0; }
```

### `data` / `operator[]` / `as_span` / `begin` / `end`

**Краткое описание:** Дают доступ к элементам общей памяти.

**Параметры:** `i` (`size_type`) для `operator[]`, без проверки границ.

**Возвращает:** указатель, ссылку, `std::span<T>`, итераторы-указатели.

**Побочные эффекты:** Запись видна всем кабинам, открывшим тег. Синхронизацию (атомарные операции, [strand](strand.md) `park`/`wake`) обеспечивает программа.

**Пример:** См. `bay::create`.

## shared_object

### `box::shared_object<T>`

**Краткое описание:** Один общий объект `T` в Bay с доступом как через указатель.

**Параметры:** Фабрики `create(tag)`, `open(tag)`, `create_encrypted(tag)`, `open_encrypted(tag)`; `const T` — только чтение.

**Возвращает:** Неприменимо: класс; только перемещение. `operator bool`, `encrypted()`, `read_only()`, `get()`, `operator*`, `operator->`.

**Побочные эффекты:** Как у `bay`.

**Пример:**
```cpp
#include <atomic>

#include "box/cxx/bay.h"

struct Counter {
    std::atomic<std::uint64_t> Hits;
};

void Hit()
{
    static auto Shared = box::shared_object<Counter>::open("demo:hits");
    if (Shared) Shared->Hits.fetch_add(1);
}
```
