# painter::items::look (внутренний)

## Назначение

Заголовок: [`painter/items/look.h`](../../../../src/userspace/painter/include/painter/items/look.h).
Реализация: [`src/items/look.cpp`](../../../../src/userspace/painter/src/items/look.cpp).

Как выглядит элемент: каждое свойство так, как его сказала программа, или
— несказанное — как тема рисует его роль, или иначе — как рисует система.
Если человек настаивает (`theme insist`), вид темы стоит выше сказанного
программой для всех элементов с ролью. Порядок: система < тема < программа
< человек. Каждое свойство ищется здесь и больше нигде, поэтому откуда
берётся вид, решается в одном месте.

## Theme

### `Theme`

**Краткое описание:** Хранит тему экрана.

**Параметры:**
- `Text` (#C0C0C0), `Page` (#000000): цвета текста и страницы экрана (их сообщает хранитель программы впереди).
- `Panel` (#1C1C1C), `Quiet` (#808080): панель и тихий текст, выведенные из двух первых.
- `Accent` (#4E8CFF), `OnAccent` (#FFFFFF): акцент и цвет на акценте.
- `TextFamily` ("Inter"), `CodeFamily` ("JetBrains Mono NL").
- `Round` (8 точек): скругление кнопки; поле — ¾, панель — 1½, выделение — ⅜ от него.
- `Shadows` (`true`): панели, кнопки и поля приподняты мягкой тенью.
- `Insist` (`false`): тема поверх программы для элементов с ролью.
- `Faint` (0,4): какая доля недоступного элемента видна.
- `FocusWidth` (2), `FocusGap` (3): кольцо вокруг того, до чего дошла клавиатура, в точках.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `ThemeOf`.

### `ThemeSaid`

**Краткое описание:** Хранит то, что человек задал в теме.

**Параметры:**
- `Accent` (`core::Color`), `Round` (`std::int32_t`, единицы `ITEM_POINT`), `Shadows` (`bool`), `Insist` (`bool`).

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `ThemeOf`.

## Функции

### `ThemeOf`

**Краткое описание:** Строит тему из цветов текста и страницы экрана и того, что задал человек.

**Параметры:**
- `TextColor`, `PageColor` (`std::uint32_t`): #RRGGBB.
- `Person` (`const ThemeSaid &`): по умолчанию — значения по умолчанию.

**Возвращает:** (`Theme`).

**Побочные эффекты:** Нет.

**Как работает:** Панель — страница с 20/255 текста, тихий текст — со 150/255 (смешивание как свет). Цвет на акценте — тёмный (#141414), если светлота акцента в OKLab больше 0,72, иначе белый.

**Пример:**
```cpp
#include "painter/items/look.h"

painter::items::Theme Light() { return painter::items::ThemeOf(0x202020, 0xF4F4F4); }
```

### `FillsOf` / `CornersOf` / `ShadowsOf` / `StrokeOf` / `StyleOf`

**Краткое описание:** Возвращают заливки, скругления, тени, обводку и стиль текста элемента — сказанные или от роли.

**Параметры:**
- `Said` (`const Properties &`), `Kind` (`std::uint8_t`, для заливок), `Role` (`std::uint16_t`), `With` (`const Theme &`); `Into` (`Stroke &`, out) для `StrokeOf`.

**Возвращает:** `FillsOf` — `std::vector<Fill>`; `CornersOf` — `std::array<std::int32_t, 4>`; `ShadowsOf` — `std::vector<Shadow>`; `StrokeOf` — `bool` (есть ли обводка); `StyleOf` — `TextStyle` с каждой частью сказанной.

**Побочные эффекты:** Нет.

**Как работает:** Значения ролей по умолчанию:
- заливки текста: тихий цвет для `Hint`, цвет на акценте для `Button`, иначе цвет текста;
- заливки фигур: `Page` — страница; `Panel` и `Field` — панель; `Button` — акцент; `Selection` — акцент на ~⅓ непрозрачности; `Icon` — текст; `Separator` — тихий на ~½; остальные — без заливки;
- тени (если `Shadows`): `Panel` — (0, 4), размытие 18, ~40 %; `Button` — (0, 1), размытие 4, ~30 %; `Field` — внутренняя (0, 1), размытие 3, ~46 %;
- обводка: только `Field` — 1 точка внутрь, тихий цвет на ~60 %;
- стиль: семейство кода для `Code`, иначе текста; кегль 22 у `Heading`, 14 у `Code` и `Label`, 13 у `Hint`, иначе 15; насыщенность 600 у `Heading` и `Button`, 500 у `Label`, иначе 400.

**Пример:**
```cpp
#include "painter/items/look.h"

std::size_t PanelFills(const painter::items::Theme &Look)
{
    return painter::items::FillsOf({}, ITEM_SHAPE, ITEM_ROLE_PANEL, Look).size();
}
```

### `Plated` / `PlateOf` / `InsetOf`

**Краткое описание:** Сообщают, носит ли текст свою плашку, чем она залита и насколько слова отступают от её угла.

**Параметры:**
- `Kind` (`std::uint8_t`), `Role` (`std::uint16_t`).

**Возвращает:** `Plated` — (`bool`) текст в роли `Button` или `Field`; `PlateOf` — заливки плашки (акцент или панель); `InsetOf` — (`ItemPoint`) 14×7 точек у кнопки, 10×7 у поля, иначе 0.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/items/look.h"

bool IsButtonLike(std::uint16_t Role) { return painter::items::Plated(ITEM_TEXT, Role); }
```

### `FocusableOf` / `DisabledOf` / `OpacityOf` / `HiddenOf` / `ColorOf` / `ThemeColor`

**Краткое описание:** Возвращают доступность клавиатуре, неактивность, непрозрачность, скрытость, цвет языка как цвет движка и цвет темы по номеру.

**Параметры:**
- `Said` (`const Properties &`), `Kind`, `Role`; `Of` (`const ItemColor &` или `const Theme &`), `Which` (`ITEM_THEME_*`).

**Возвращает:** `FocusableOf` — сказанное, иначе `true` для `Button` и `Field`; `DisabledOf`, `HiddenOf` — `bool`; `OpacityOf` — `std::uint16_t` (по умолчанию `ITEM_OPAQUE`); `ColorOf`, `ThemeColor` — `core::Color`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/items/look.h"

painter::core::Color Accent(const painter::items::Theme &Look) { return painter::items::ThemeColor(Look, ITEM_THEME_ACCENT); }
```
