# box::console

## Назначение

Заголовок: [`box/cxx/console.h`](../../../src/userspace/boxcxx/include/box/cxx/console.h).
C-основа: `box/color.h`.

Цвет текста в консоли. В BoxOS цвет — не escape-последовательности ANSI, а
структурные метаданные, прикреплённые к отрезку текста (один `printf` —
один системный вызов). Поэтому здесь нет управляющих байтов: есть значение
цвета и состояние «текущий цвет текста процесса», которое наследует весь
дальнейший вывод (`std::print`, `printf`, потоки C++).

Как цвет выглядит на экране, решает дисплей: на framebuffer — 24 бита, на
текстовой странице VGA — ближайший из 16 цветов.

## Типы

### `box::color`

**Краткое описание:** Хранит цвет 0x00RRGGBB или особое значение.

**Параметры:**
- `color()` — цвет по умолчанию (`COLOR_DEFAULT`).
- `color(Color raw)` (`explicit`), `color(r, g, b)`.
- `color::rgb(r, g, b)`, `color::from_raw(raw)`, `color::use_default()` — цвет экрана, `color::inherit()` — унаследованный.

**Возвращает:** Неприменимо: класс; `==`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/console.h"

constexpr box::color Orange = box::color::rgb(0xFF, 0x80, 0x00);
```

### `raw` / `r` / `g` / `b` / `is_default` / `is_inherit` / `is_rgb`

**Краткое описание:** Возвращают сырое значение, каналы и вид цвета.

**Параметры:** Нет.

**Возвращает:** `raw` — (`Color`); `r`, `g`, `b` — (`std::uint8_t`); остальные — (`bool`); `is_rgb` — настоящий цвет, не особое значение.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/console.h"

bool Bright(box::color C) { return C.is_rgb() && C.r() + C.g() + C.b() > 500; }
```

### `box::colors`

**Краткое описание:** Называет стандартную палитру BoxOS.

**Параметры:** `black`, `white`, `dark_gray`, `light_gray`, `red`, `green`, `blue`, `yellow`, `cyan`, `magenta`, `dark_red`, `dark_green`, `dark_blue`, `brown`, `dark_cyan`, `dark_magenta`; акценты BoxOS `berry`, `ocean`, `leaf`, `amber`, `violet`, `teal`, `coral`, `slate`; `use_default`, `inherit`. Значения RGB — как у `painter::core::Colors` ([color](../painter/core/color.md)).

**Возвращает:** Неприменимо: `inline constexpr color`.

**Побочные эффекты:** Нет.

**Пример:** См. `box::styled`.

### `box::styled`

**Краткое описание:** Меняет цвет текста процесса на время области видимости и восстанавливает его на выходе.

**Параметры:**
- `styled(color fg)` — цвет текста (`explicit`).
- `styled(color fg, color bg)` — цвет текста и фона.

**Возвращает:** Неприменимо: класс RAII; копировать нельзя.

**Побочные эффекты:** Конструктор меняет цвет текста (и фона); деструктор возвращает прежние.

**Пример:**
```cpp
#include <print>

#include "box/cxx/console.h"

void Warn(std::string_view What)
{
    {
        box::styled Red(box::colors::red);
        std::print("ошибка: ");
    }
    std::println("{}", What);
}
```

## Функции

### `current_color` / `current_background` / `set_color` / `set_background`

**Краткое описание:** Читают и задают цвет текста и фона процесса напрямую.

**Параметры:**
- `fg`, `bg` (`color`): для установщиков.

**Возвращает:** `current_*` — (`color`); установщики — `void`.

**Побочные эффекты:** Установщики меняют состояние процесса до следующей смены (`styled` — ограниченная областью форма).

**Пример:**
```cpp
#include "box/cxx/console.h"

void Plain() { box::set_color(box::colors::use_default); }
```

### `std::formatter<box::color>`

**Краткое описание:** Печатает значение цвета как `#RRGGBB` (особые значения — `default` и `inherit`).

**Параметры:** Спецификация как у `std::string_view`.

**Возвращает:** Неприменимо: специализация.

**Побочные эффекты:** Нет. Это представление значения, а не управляющая последовательность: цвет вывода оно не меняет.

**Пример:**
```cpp
#include <format>

#include "box/cxx/console.h"

std::string Hex() { return std::format("{}", box::colors::amber); }   // "#FFB040"
```
