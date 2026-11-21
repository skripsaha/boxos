# painter::tui::glyph

## Назначение

Заголовок: [`painter/tui/glyph.h`](../../../../src/userspace/painter/include/painter/tui/glyph.h).
Реализации нет: только константы.

Пространство имён `painter::tui::Glyphs` даёт имена символам, которыми
рисуют псевдографику: блоки, штриховки, стрелки, линии рамок. Это обычные
символы Unicode (`char32_t`), а не номера глифов шрифта. Имя делает код
понятнее, чем литерал `U'▀'`, и собирает символы, на которые опираются
`Frame::PutPixel`, [mosaic](mosaic.md) и [window](window.md).

Любой другой символ Unicode можно класть в ячейку напрямую — список ниже
не ограничивает выбор. На framebuffer линии рамок, блоки и штриховки
рисуются геометрией Typecase точно по ячейке, поэтому соседние ячейки
стыкуются без щелей при любом шрифте.

## Блоки и штриховки

### `UpperHalf`, `LowerHalf`, `LeftHalf`, `RightHalf`, `Full`

**Краткое описание:** Называют символы закрашенных половин и целой ячейки.

**Параметры:**
- `UpperHalf` = U+2580 ▀ — верхняя половина цветом символа, нижняя цветом фона.
- `LowerHalf` = U+2584 ▄.
- `LeftHalf` = U+258C ▌.
- `RightHalf` = U+2590 ▐.
- `Full` = U+2588 █.

**Возвращает:** Неприменимо: константы `char32_t`.

**Побочные эффекты:** Нет.

**Примечания:** `UpperHalf` — основа полублочных точек: `Frame::PutPixel` всегда пишет его, и два цвета ячейки дают две точки. `ResolveMosaic` выбирает из `UpperHalf` и `LeftHalf`; `LowerHalf` и `RightHalf` дают те же картинки с переставленными цветами.

**Пример:**
```cpp
#include "painter/tui/frame.h"
#include "painter/tui/glyph.h"

void Bar(painter::tui::Frame &Out, std::int32_t Row, std::int32_t Length)
{
    (void)Out.Fill({0, Row, Length, 1}, painter::tui::Glyphs::Full, {painter::core::Colors::Leaf});
}
```

### `LightShade`, `MidShade`, `DenseShade`

**Краткое описание:** Называют символы штриховки на четверть, половину и три четверти.

**Параметры:**
- `LightShade` = U+2591 ░ — четверть ячейки цветом символа.
- `MidShade` = U+2592 ▒ — половина.
- `DenseShade` = U+2593 ▓ — три четверти.

**Возвращает:** Неприменимо: константы `char32_t`.

**Побочные эффекты:** Нет.

**Как работает:** Доли — это контракт символа по Unicode, а не свойство конкретного шрифта: так их рисуют и дисплей на framebuffer, и ПЗУ VGA. `MosaicOf` видит штриховку как смесь двух цветов в этой доле, `ResolveMosaic` использует её как текстуру только на пиксельном стекле.

**Пример:**
```cpp
#include "painter/tui/frame.h"
#include "painter/tui/glyph.h"

void Fog(painter::tui::Frame &Out, painter::core::Rect Area)
{
    (void)Out.Fill(Area, painter::tui::Glyphs::LightShade, {painter::core::Colors::DarkGray, painter::core::Colors::Black});
}
```

## Знаки

### `Dot`, `Bullet`, `Tick`, `Pointer`, `Left`, `Right`, `Up`, `Down`

**Краткое описание:** Называют знаки для списков, отметок и подсказок.

**Параметры:**
- `Dot` = U+00B7 ·, `Bullet` = U+2022 •, `Tick` = U+221A √, `Pointer` = U+25BA ►.
- `Left` = U+2190 ←, `Right` = U+2192 →, `Up` = U+2191 ↑, `Down` = U+2193 ↓.

**Возвращает:** Неприменимо: константы `char32_t`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/tui/frame.h"
#include "painter/tui/glyph.h"

void Chosen(painter::tui::Frame &Out, std::int32_t Row) { (void)Out.Put({1, Row}, painter::tui::Glyphs::Pointer, {painter::core::Colors::Amber}); }
```

## Рамки

### `RuleAcross`, `DoubleRule`, `RuleDown`, `CornerTL`, `CornerTR`, `CornerBL`, `CornerBR`, `TeeLeft`, `TeeRight`

**Краткое описание:** Называют тонкие линии рамок, углы и примыкания.

**Параметры:**
- `RuleAcross` = U+2500 ─, `DoubleRule` = U+2550 ═, `RuleDown` = U+2502 │.
- `CornerTL` = U+250C ┌, `CornerTR` = U+2510 ┐, `CornerBL` = U+2514 └, `CornerBR` = U+2518 ┘.
- `TeeLeft` = U+251C ├ (примыкание к левой стенке), `TeeRight` = U+2524 ┤ (к правой).

**Возвращает:** Неприменимо: константы `char32_t`.

**Побочные эффекты:** Нет.

**Типичное применение:** Готовое окно рисует `DrawWindow` из [window](window.md); константы нужны для собственных рамок и таблиц.

**Пример:**
```cpp
#include "painter/tui/frame.h"
#include "painter/tui/glyph.h"

void Underline(painter::tui::Frame &Out, std::int32_t Row)
{
    (void)Out.Fill({0, Row, Out.Size().Width, 1}, painter::tui::Glyphs::RuleAcross, {painter::core::Colors::DarkGray});
}
```

### `MoreLeft`, `MoreRight`

**Краткое описание:** Называют знаки того, что строка продолжается за краем.

**Параметры:**
- `MoreLeft` = U+00AB « — строка продолжается влево.
- `MoreRight` = U+00BB » — вправо.

**Возвращает:** Неприменимо: константы `char32_t`.

**Побочные эффекты:** Нет.

**Типичное применение:** Горизонтальная прокрутка длинной строки в редакторе или списке.

**Пример:**
```cpp
#include "painter/tui/frame.h"
#include "painter/tui/glyph.h"

void Clipped(painter::tui::Frame &Out, std::int32_t Row, bool Before, bool After)
{
    if (Before) (void)Out.Put({0, Row}, painter::tui::Glyphs::MoreLeft, {painter::core::Colors::DarkGray});
    if (After) (void)Out.Put({Out.Size().Width - 1, Row}, painter::tui::Glyphs::MoreRight, {painter::core::Colors::DarkGray});
}
```
