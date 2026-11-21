# box::error

## Назначение

Заголовок: [`box/cxx/error.h`](../../../src/userspace/boxcxx/include/box/cxx/error.h).
Реализация таблицы сообщений: [`src/runtime/box_error.cpp`](../../../src/userspace/boxcxx/src/runtime/box_error.cpp).
Источник кодов: `box/error.h` (`BOX_ERROR_LIST`).

Модель ошибок BoxOS для C++. BoxOS — не Unix и не берёт словарь ошибок
Unix: ядро сообщает о неудаче категоризированным `error_t` (коды `ERR_*`,
сгруппированные по диапазонам: ядро-«core», память 100-е, ввод-вывод
200-е, хранилище 300-е, процессы 400-е, TagFS, IPC и т. д.), а не таблицей
errno Linux. Этот заголовок даёт C++-лицо этой модели: каждая
`box::`-операция, которая может не удаться, возвращает настоящую причину из
ядра, а не `bool`, `-1` или `nullopt`.

Цепочка: ядро `error_t` → заглушка boxlib возвращает `-error_t` → `box::error`.
Регистра errno на этом пути нет.

Все типы — расширение `box::`, не часть стандарта. `std::errc` остаётся
тонкой прослойкой для стандартной машинерии и не является языком ошибок
BoxOS.

## Типы

### `box::errc`

**Краткое описание:** Перечисляет все коды ошибок ядра как строго типизированные значения.

**Параметры:** Перечисление с базовым типом `error_t`; `ok = OK`, далее по одному значению на каждый `ERR_*` из `BOX_ERROR_LIST` (`no_memory`, `invalid_argument`, `timeout`, `file_not_found`, `object_not_found`, `out_of_range`, `invalid_state`, `device_not_ready`, `tag_not_found`, `corrupted`…). Значение равно коду `ERR_*`.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Как работает:** Перечисление разворачивается из той же таблицы, что и коды C, поэтому разойтись с ними не может: новый код в `box/error.h` сразу появляется здесь вместе с сообщением и категорией.

**Пример:**
```cpp
#include "box/cxx/error.h"

bool Missing(box::error E) { return E.code() == box::errc::file_not_found; }
```

### `box::error`

**Краткое описание:** Хранит одну причину ошибки ядра — 4-байтовое тривиально копируемое значение.

**Параметры:**
- `error()` — «нет ошибки» (`OK`).
- `error(error_t)` — из сырого кода (`explicit`).
- `error(errc)` — из перечисления (неявно).

**Возвращает:** Неприменимо: класс; сравнение `==` и `<=>`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/error.h"

constexpr box::error Busy = box::errc::timeout;
static_assert(Busy && !Busy.ok());
```

### `raw` / `code` / `ok` / `operator bool`

**Краткое описание:** Возвращают сырой код, перечисление, признак успеха и признак неудачи.

**Параметры:** Нет.

**Возвращает:** `raw` — (`error_t`); `code` — (`errc`); `ok` — (`bool`) `true`, если `OK`; `operator bool` — (`bool`) `true`, если это **неудача** (как у `std::error_code`).

**Побочные эффекты:** Нет.

**Примечания:** Полярность противоположна `box::result`: у результата `true` — успех, у голой ошибки `true` — неудача. Проверяйте результат, а не ошибку.

**Пример:**
```cpp
#include "box/cxx/error.h"

int Code(box::error E) { return E ? static_cast<int>(E.raw()) : 0; }
```

### `message` / `category_name`

**Краткое описание:** Возвращают текст ошибки и имя её категории.

**Параметры:** Нет.

**Возвращает:** (`std::string_view`) из статической таблицы — `"out of memory"`, `"memory"`. Никогда не `nullptr`-строка.

**Побочные эффекты:** Нет; без выделения памяти, безопасны в `noexcept`-путях и до или после статической инициализации.

**Как работает:** Категории: `core`, `memory`, `io`, `storage`, `process`, `ipc`, `tagfs`, `braid`, `security`, `routing`, `scheduler`, `hardware`, `boot`.

**Пример:**
```cpp
#include <print>

#include "box/cxx/error.h"

void Say(box::error E) { std::println("{} ({})", E.message(), E.category_name()); }
```

### `box::result<T>` / `box::status`

**Краткое описание:** Канонический возврат операции, которая может не удаться.

**Параметры:**
- `result<T>` = `std::expected<T, box::error>`.
- `status` = `std::expected<void, box::error>`.

**Возвращает:** Неприменимо: псевдонимы типов.

**Побочные эффекты:** Нет.

**Как работает:** `operator bool` результата — `true` при **успехе**, поэтому `if (auto R = f())` читается естественно, а `R.error().code()` — точная причина.

**Примечания:** В реализации `std::expected` boxcxx перемещение `expected<T, E>` требует, чтобы `T` имел конструктор по умолчанию: функция, возвращающая локальный `box::result<T>` с типом без такого конструктора (`return Local;`), не компилируется. Возвращайте значение или `std::unexpected` напрямую.

**Пример:**
```cpp
#include "box/cxx/error.h"

box::result<int> Half(int Value)
{
    if (Value % 2) return std::unexpected(box::error{box::errc::invalid_argument});
    return Value / 2;
}

int Use()
{
    if (auto R = Half(10)) return *R;
    else return -static_cast<int>(R.error().raw());
}
```

### `std::formatter<box::error>`

**Краткое описание:** Печатает ошибку как `категория:код сообщение`.

**Параметры:** Спецификация как у `std::string_view` (ширина, заполнение, выравнивание, точность).

**Возвращает:** Неприменимо: специализация.

**Побочные эффекты:** Выделяет строку при форматировании.

**Пример:**
```cpp
#include <format>

#include "box/cxx/error.h"

std::string Line() { return std::format("ошибка: {}", box::error{box::errc::no_memory}); }   // "ошибка: memory:100 out of memory"
```

## Совместимость со std::error_code

### `error_category` / `make_error_code` / `to_error_code`

**Краткое описание:** Переводят ошибки BoxOS в `std::error_code` без подмены смысла.

**Параметры:**
- `make_error_code(errc)`; `to_error_code(error)`.

**Возвращает:** `error_category` — (`const std::error_category &`) категория `"box"` со своей нумерацией и сообщениями; остальные — (`std::error_code`) с тем же значением и категорией `"box"`.

**Побочные эффекты:** Нет.

**Как работает:** `default_error_condition` категории отображает каждый код сам в себя — никогда в `generic_category` Linux, у которой нет честных соответствий. `std::is_error_code_enum<box::errc>` истинно, поэтому `std::error_code Ec = box::errc::no_memory;` работает и даёт значение 100 категории `"box"`, а не POSIX `ENOMEM`.

**Пример:**
```cpp
#include <system_error>

#include "box/cxx/error.h"

std::error_code Ec = box::errc::no_memory;
```

## Мост к boxlib (внутренний)

### `_detail::from_ret` / `from_ret64` / `from_status` / `from_err`

**Краткое описание:** Превращают возврат заглушки boxlib в `box::result`.

**Параметры:**
- `from_ret<T>(int r)`: `r ≥ 0` — значение, `r < 0` — `-error_t`; полезная нагрузка должна помещаться в `0…INT_MAX`.
- `from_ret64<T>(std::int64_t r)`: то же для больших счётчиков байтов.
- `from_status(int r)`: 0 — успех, иначе причина.
- `from_err(error_t e)`: код, переданный отдельно.

**Возвращает:** (`result<T>`) или (`status`).

**Побочные эффекты:** Нет.

**Примечания:** Нужны авторам обёрток над C-функциями boxlib.

**Пример:**
```cpp
#include "box/cxx/error.h"
#include "box/display.h"

box::status Size(std::uint32_t &Cols, std::uint32_t &Rows) { return box::_detail::from_status(display_size(&Cols, &Rows)); }
```
