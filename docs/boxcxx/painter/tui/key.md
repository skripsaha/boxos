# painter::tui::key

## Назначение

Заголовок: [`painter/tui/key.h`](../../../../src/userspace/painter/include/painter/tui/key.h).
Реализация `KeyEvent::Spelling`: [`src/tui/keyboard.cpp`](../../../../src/userspace/painter/src/tui/keyboard.cpp).

Одна клавиша так, как её сообщает клавиатура: символ Unicode, который она
печатает, скан-код и удерживаемые модификаторы. `KeyEvent` приходит в
`painter::tui::Event` (от [Stage](stage.md) и [Keyboard](keyboard.md)) и в
событии `painter::gui::Event` с видом `Key`.

На проводе клавиша — 6 байт `kb_event_t` ([`kb_event.h`](../../../../src/include/kb_event.h)).
Раскладку применяет драйвер: программа получает уже готовый символ.

Два вида клавиш различаются так:

- клавиша-символ — `Character` не 0: буква, цифра, пробел, а также Escape (27), Backspace (`'\b'`), Enter (`'\n'`), Tab (`'\t'`);
- клавиша-действие — `Character == 0`, установлен `Modifiers::Extended`, а `Scancode` называет клавишу: стрелки, Home, End, PageUp, PageDown, Insert, Delete.

Ctrl — модификатор при букве: Ctrl+S приходит как `{Character 's', Mods Ctrl}`,
а не как байт 0x13. F1–F12 и цифровая клавиатура драйвером не публикуются.

## Константы

### `Modifiers`

**Краткое описание:** Называет биты модификаторов поля `KeyEvent::Mods`.

**Параметры:**
- `Shift` (= `KB_MOD_SHIFT`, 0x01).
- `Ctrl` (= `KB_MOD_CTRL`, 0x02).
- `Alt` (= `KB_MOD_ALT`, 0x04).
- `Extended` (= `KB_MOD_EXTENDED`, 0x08): клавиша из набора с префиксом 0xE0 — стрелки, Home/End, Delete и т. п.

**Возвращает:** Неприменимо: константы `std::uint8_t`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/tui/key.h"

bool CtrlShift(const painter::tui::KeyEvent &Key)
{
    using namespace painter::tui;
    return (Key.Mods & (Modifiers::Ctrl | Modifiers::Shift)) == (Modifiers::Ctrl | Modifiers::Shift);
}
```

### `KeyCodes`

**Краткое описание:** Называет коды клавиш, на которые обычно отвечают программы.

**Параметры:**
- Символы (`char`, сравниваются с `KeyEvent::Character`): `Escape` (27), `Backspace` (`'\b'`), `Enter` (`'\n'`), `Tab` (`'\t'`).
- Скан-коды (`std::uint8_t`, сравниваются с `KeyEvent::Scancode` при `IsExtended()`): `Home` (0x47), `Up` (0x48), `PageUp` (0x49), `Left` (0x4B), `Right` (0x4D), `End` (0x4F), `Down` (0x50), `PageDown` (0x51), `Insert` (0x52), `Delete` (0x53).

**Возвращает:** Неприменимо: константы.

**Побочные эффекты:** Нет.

**Примечания:** Значения — это set-1 скан-коды из `kb_event.h`, общие для ядра и всех программ. Сравнивайте скан-код стрелки вместе с `IsExtended()`: так клавиша-действие не спутается с другой клавишей того же скан-кода.

**Пример:**
```cpp
#include "painter/tui/key.h"

int Move(const painter::tui::KeyEvent &Key)
{
    namespace tui = painter::tui;
    if (!Key.IsExtended()) return 0;
    if (Key.Scancode == tui::KeyCodes::Left) return -1;
    if (Key.Scancode == tui::KeyCodes::Right) return 1;
    return 0;
}
```

## KeyEvent

### `KeyEvent`

**Краткое описание:** Описывает одну нажатую клавишу.

**Параметры:**
- `Character` (`char32_t`): символ Unicode, который печатает клавиша; 0 — клавиша без символа.
- `Scancode` (`std::uint8_t`): скан-код клавиши.
- `Mods` (`std::uint8_t`): биты `Modifiers`.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. методы ниже.

### `KeyEvent::HasCharacter`

**Краткое описание:** Сообщает, печатает ли клавиша символ.

**Параметры:** Нет.

**Возвращает:** (`bool`) `Character != 0`.

**Побочные эффекты:** Нет.

**Примечания:** Escape, Enter, Backspace и Tab тоже имеют символ; для ввода текста используйте `IsPrintable`.

**Пример:**
```cpp
#include "painter/tui/key.h"

bool Action(const painter::tui::KeyEvent &Key) { return !Key.HasCharacter(); }
```

### `KeyEvent::IsPrintable`

**Краткое описание:** Сообщает, видим ли символ клавиши.

**Параметры:** Нет.

**Возвращает:** (`bool`) `true` для символа от U+0020, кроме DEL (U+007F), управляющих C1 (U+0080–U+009F), суррогатов и значений больше U+10FFFF.

**Побочные эффекты:** Нет.

**Типичное применение:** Решить, добавлять ли клавишу в строку ввода.

**Пример:**
```cpp
#include <string>

#include "painter/tui/key.h"

void Type(std::string &Line, const painter::tui::KeyEvent &Key)
{
    if (Key.IsPrintable() && !Key.IsCtrl()) Line += Key.Spelling();
}
```

### `KeyEvent::Spelling`

**Краткое описание:** Возвращает символ клавиши в UTF-8.

**Параметры:** Нет.

**Возвращает:** (`std::string`) 1–4 байта UTF-8; пустая строка для клавиши без символа или для значения, которое нельзя закодировать.

**Побочные эффекты:** Выделяет память под строку; может бросить `std::bad_alloc`.

**Как работает:** Кодирует `Character` функцией `std::c32rtomb`.

**Типичное применение:** Добавить введённый символ к строке UTF-8, которую затем пишет `Frame::Write`.

**Пример:** См. `IsPrintable`.

### `KeyEvent::IsExtended` / `IsShift` / `IsCtrl` / `IsAlt`

**Краткое описание:** Проверяют отдельные биты модификаторов.

**Параметры:** Нет.

**Возвращает:** (`bool`) установлен ли соответствующий бит `Mods`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/tui/key.h"

bool Save(const painter::tui::KeyEvent &Key) { return Key.IsCtrl() && (Key.Character == U's' || Key.Character == U'S'); }
```
