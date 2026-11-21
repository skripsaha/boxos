# painter::tui::keyboard

## Назначение

Заголовок: [`painter/tui/keyboard.h`](../../../../src/userspace/painter/include/painter/tui/keyboard.h).
Реализация: [`src/tui/keyboard.cpp`](../../../../src/userspace/painter/src/tui/keyboard.cpp).

`Keyboard` — только подписка на клавиши текущего потока. Экран она не
занимает и ничего не очищает. Её используют вместе с [Surface](surface.md)
или [ConsoleSurface](console_surface.md), у которых своей клавиатуры нет,
а также передают в `painter::gui::Area::Wait`, чтобы ждать клавиши и
события дисплея одним вызовом. [Stage](stage.md) содержит такую же
клавиатуру внутри.

События — те же `Event` и `EventKind`, что у `Stage` (см. [stage](stage.md)),
клавиша — `KeyEvent` (см. [key](key.md)).

## Класс Keyboard

### `Keyboard`

**Краткое описание:** Владеет подпиской текущего потока на клавиши консоли.

**Параметры:** Создаётся только через `Keyboard::Open`.

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Пока объект открыт, клавиши потока приходят ему, а не строковому редактору консоли.

**Как работает:** `Open` получает Touch-тег, на который приходят клавиши потока (`console_listen`; при работающем дисплее это собственный тег потока, и дисплею сообщается, что поток слушает). На том же теге приходят уведомления дисплея (`ConsoleNotice`); клавиша отличается по длине — ровно 6 байт. `Close` возвращает слух дисплею (`console_unlisten`): клавиша, нажатая, пока никто не слушает, ждёт у дисплея следующего слушателя.

**Примечания:** Копировать нельзя, перемещать можно; перемещающее присваивание сначала закрывает прежнюю подписку получателя. Деструктор вызывает `Close()`. Подписка относится к потоку: две открытые клавиатуры в одном потоке слушают один тег — держите одну.

**Типичное применение:** `Surface` + `Keyboard` для интерактивной панели поверх консоли.

**Пример:** См. [README](README.md), «Фигура поверх консоли».

### `Keyboard::Open`

**Краткое описание:** Подписывает текущий поток на клавиши.

**Параметры:** Нет.

**Возвращает:** (`std::expected<Keyboard, StageError>`) открытая клавиатура; `KeyboardUnavailable` с `errc::device_not_ready`, если слушать нечем; `FrameUnavailable` с `errc::no_memory` при нехватке памяти (подписка тогда снимается).

**Побочные эффекты:** Вызывает `console_listen`.

**Пример:**
```cpp
#include "painter/tui/keyboard.h"

std::expected<painter::tui::Keyboard, painter::tui::StageError> Listen() { return painter::tui::Keyboard::Open(); }
```

### `Keyboard::IsOpen`

**Краткое описание:** Сообщает, открыта ли подписка.

**Параметры:** Нет.

**Возвращает:** (`bool`) `false` после `Close()` и у перемещённого объекта.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/tui/keyboard.h"

bool Ready(const painter::tui::Keyboard &Ear) { return Ear.IsOpen(); }
```

### `Keyboard::Poll`

**Краткое описание:** Возвращает следующее событие, не ожидая.

**Параметры:** Нет.

**Возвращает:** (`Event`) `Key`, `EndAsked`, `Gone` или `Quiet`, если ждущих клавиш нет. На закрытой клавиатуре — `Gone` с `errc::invalid_state`. Если ввод процесса уже закрыт (`console_input_ended`) и `HearEnd` не вызывался — `Gone` без причины.

**Побочные эффекты:** Забирает сообщения с тега до первого события; сообщения, не являющиеся событиями, отбрасываются.

**Как работает:** Читает тег без ожидания (`touch_try_pop_tag`). Уведомление `CONSOLE_NOTICE_END` даёт `EndAsked` при `HearEnd()`, иначе закрывает ввод процесса (`console_input_ends`) и даёт `Gone`.

**Пример:**
```cpp
#include "painter/tui/keyboard.h"

bool AnyKey(painter::tui::Keyboard &Ear) { return Ear.Poll().Kind == painter::tui::EventKind::Key; }
```

### `Keyboard::Wait`

**Краткое описание:** Ждёт следующее событие.

**Параметры:**
- `TimeoutMs` (`std::uint32_t`): предел в миллисекундах; 0 — без предела.

**Возвращает:** (`Event`) `Key`, `EndAsked`, `Gone` (с причиной при ошибке ожидания) или `Quiet` по таймауту.

**Побочные эффекты:** Блокирует поток (`touch_await`).

**Как работает:** Если пришло сообщение, которое не является событием, ограниченное ожидание сразу возвращает `Quiet`, а неограниченное ждёт дальше.

**Примечания:** `Quiet` при ненулевом таймауте может прийти раньше срока.

**Пример:**
```cpp
#include "painter/tui/keyboard.h"

bool Confirmed(painter::tui::Keyboard &Ear)
{
    const painter::tui::Event Heard = Ear.Wait();
    return Heard.Kind == painter::tui::EventKind::Key && (Heard.Key.Character == U'y' || Heard.Key.Character == U'д');
}
```

### `Keyboard::HearEnd`

**Краткое описание:** Объявляет, что программа сама отвечает на просьбу завершиться.

**Параметры:** Нет.

**Возвращает:** `void`.

**Побочные эффекты:** Просьба завершиться приходит событием `EndAsked`; ввод процесса не закрывается.

**Примечания:** Подробности — `Stage::HearEnd` в [stage](stage.md).

**Пример:**
```cpp
#include "painter/tui/keyboard.h"

void Prepare(painter::tui::Keyboard &Ear) { Ear.HearEnd(); }
```

### `Keyboard::Close`

**Краткое описание:** Снимает подписку на клавиши.

**Параметры:** Нет.

**Возвращает:** (`box::status`) всегда успех; повторный вызов ничего не делает.

**Побочные эффекты:** Вызывает `console_unlisten`; клавиши снова получает дисплей.

**Пример:**
```cpp
#include "painter/tui/keyboard.h"

void Done(painter::tui::Keyboard &Ear) { (void)Ear.Close(); }
```
