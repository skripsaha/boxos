# box::keyboard

## Назначение

Заголовок: [`box/cxx/keyboard.h`](../../../src/userspace/boxcxx/include/box/cxx/keyboard.h).
C-основа: `box/keyboard.h`, раскладка нагрузки — `kb_event.h`.

Клавиатура в BoxOS — **события**, а не кольцо для опроса: драйвер
публикует каждую клавишу событием Touch на теге `keyboard`.

- `box::key` — одна клавиша: символ, скан-код, модификаторы;
- `box::key_stream` — типизированный вид потока Touch `keyboard`: `poll`, `wait`, `co_await next()`.

Какой путь выбрать:

- программе в консоли под дисплеем клавиши приходят по её линии консоли — `readline`/`getchar` (boxlib), `box::keyboard()` ([current](current.md)) или `painter::tui::Keyboard` ([key](../painter/tui/key.md));
- `box::key_stream` слушает сам сырой тег `keyboard` — для служебных программ, которым нужен весь поток клавиш системы.

## key

### `box::key`

**Краткое описание:** Одно событие клавиши.

**Параметры:** `key()`; `key(char32_t ch, std::uint8_t scancode, std::uint8_t mods)`.

**Возвращает:** Неприменимо: класс. `ch()` — символ Unicode (0, если клавиша символа не печатает); `scancode()`; `modifiers()` (`KB_MOD_*`); `shift()`, `ctrl()`, `alt()`; `has_char()`; `printable()` — видимый символ (не управляющий C0/C1, не DEL).

**Побочные эффекты:** Нет.

**Пример:** См. `key_stream`.

### `key::from_event` / `key::from_touch`

**Краткое описание:** Разбирают нагрузку события клавиатуры.

**Параметры:** `e` (`const kb_event_t &`) или `ev` (`const touch &`).

**Возвращает:** (`key`) / (`std::optional<key>`), пусто при короткой нагрузке.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/keyboard.h"

bool IsEnter(const box::touch &Event)
{
    auto Key = box::key::from_touch(Event);
    return Key && Key->ch() == U'\n';
}
```

## key_stream

### `box::key_stream`

**Краткое описание:** Поток клавиш с тега `keyboard`.

**Параметры:** Конструктор по умолчанию заявляет тег.

**Возвращает:** Неприменимо: класс; только перемещение. `events()` — нижележащая подписка ([touch](touch.md)).

**Побочные эффекты:** Заявка Touch на время жизни.

**Примечания:** Имя `key_stream`, а не `keyboard`: `box::keyboard()` — это канал готовых строк текста в [current](current.md).

**Пример:**
```cpp
#include <print>

#include "box/cxx/keyboard.h"

void Spy()
{
    box::key_stream Keys;
    while (auto Key = Keys.wait(5000)) {
        if (Key->printable()) std::println("клавиша {:#x}", static_cast<unsigned>(Key->ch()));
        if (Key->ch() == 27) break;   // Escape
    }
}
```

### `poll` / `wait` / `next`

**Краткое описание:** Забирают следующую клавишу без ожидания, ждут её или ждут в сопрограмме.

**Параметры:** `timeout_ms` (`std::uint32_t`) для `wait`, 0 — без предела.

**Возвращает:** `poll`, `wait` — (`std::optional<key>`); `next` — объект ожидания, дающий `std::optional<key>`.

**Побочные эффекты:** `poll` — взгляд на уже доставленное событие (не опрос железа); `wait` спит в ядре до клавиши; `next` засыпает на [executor](executor.md).

**Пример:**
```cpp
#include "box/cxx/keyboard.h"

box::task<char32_t> NextChar(box::key_stream &Keys)
{
    for (;;)
        if (auto Key = co_await Keys.next(); Key && Key->has_char()) co_return Key->ch();
}
```
