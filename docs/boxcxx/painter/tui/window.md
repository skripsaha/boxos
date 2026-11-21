# painter::tui::window

## Назначение

Заголовок: [`painter/tui/window.h`](../../../../src/userspace/painter/include/painter/tui/window.h).
Реализация: [`src/tui/window.cpp`](../../../../src/userspace/painter/src/tui/window.cpp).

Единый вид текстового окна для всех программ: тёмная «бумага», рамка из
тонких линий, заголовок в верхней кромке и примечание справа от него,
подсказка в нижней кромке и тень снизу-справа, которая отделяет окно от
того, что под ним. Так выглядят окна `edit`, `read` и хаба. Используйте
эти функции, чтобы окна новой программы не отличались от системных.

Все функции рисуют в [Frame](frame.md) и ничего не показывают сами.

## Цвета

### `WindowPaper`, `WindowRule`, `WindowInk`, `TitleInk`, `HintInk`, `ChosenPaper`

**Краткое описание:** Задают цвета окна.

**Параметры:**
- `WindowPaper` = #242424 — фон окна.
- `WindowRule` = #9A9A9A — линии рамки.
- `WindowInk` = #D4D4D4 — обычный текст.
- `TitleInk` = #F4F4F4 — заголовок.
- `HintInk` = #8C8C8C — примечание и подсказка.
- `ChosenPaper` = #2E4A6A — фон выбранной строки списка.

**Возвращает:** Неприменимо: константы `core::Color`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/tui/text.h"
#include "painter/tui/window.h"

void Item(painter::tui::Frame &Out, painter::core::Rect Inside, std::int32_t Row, std::string_view Name, bool Chosen)
{
    namespace tui = painter::tui;
    const painter::core::Color Paper = Chosen ? tui::ChosenPaper : tui::WindowPaper;
    (void)Out.Fill({Inside.X, Inside.Y + Row, Inside.Width, 1}, U' ', {tui::WindowInk, Paper});
    (void)tui::PutText(Out, {Inside.X, Inside.Y + Row}, Name, Inside.Width, tui::WindowInk, Paper);
}
```

## Функции

### `Centred`

**Краткое описание:** Возвращает прямоугольник заданного размера по центру экрана.

**Параметры:**
- `Screen` (`core::Size`): размер экрана или кадра.
- `Width` (`std::int32_t`): желаемая ширина.
- `Height` (`std::int32_t`): желаемая высота.

**Возвращает:** (`core::Rect`) прямоугольник по центру; ширина и высота ограничены диапазоном от 1 до размера экрана.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/tui/window.h"

painter::core::Rect Dialog(const painter::tui::Frame &Out) { return painter::tui::Centred(Out.Size(), 50, 12); }
```

### `DrawWindow`

**Краткое описание:** Рисует окно с рамкой, заголовком, примечанием, подсказкой и тенью и возвращает его внутреннюю область.

**Параметры:**
- `Out` (`Frame &`, out): кадр.
- `Area` (`core::Rect`): внешний прямоугольник окна, включая рамку.
- `Title` (`std::string_view`): заголовок слева в верхней кромке; пустая строка — без заголовка.
- `Note` (`std::string_view`): примечание справа в верхней кромке; пустая — без него.
- `Hint` (`std::string_view`): подсказка слева в нижней кромке; пустая — без неё.

**Возвращает:** (`core::Rect`) внутренняя область: `{Area.X + 2, Area.Y + 1, Area.Width − 4, Area.Height − 2}` — по одной ячейке отступа от вертикальных линий.

**Побочные эффекты:** Затемняет ячейки тени, заливает `Area` фоном, рисует рамку и подписи.

**Как работает:**
- Тень — строка под окном (сдвинутая на ячейку вправо) и столбец справа от него: цвета символа и фона существующих ячеек смешиваются с чёрным на 103/255 как свет, поэтому текст под тенью остаётся читаемым.
- Подписи обрамляются пробелами (`" Заголовок "`) и не выходят за `Area.Width − 4` ячеек; примечание рисуется, только если помещается справа от заголовка с промежутком в ячейку.
- Подписи пишутся через `PutText`: длинные обрезаются без многоточия.

**Примечания:** Окно должно быть не меньше 4×3 ячеек, иначе внутренняя область пуста или отрицательна. Тень рисуется за пределами `Area` — оставьте для неё строку и столбец, если окно у края кадра (вне кадра тень просто обрезается).

**Типичное применение:** Диалоги, списки выбора, окна помощи поверх сцены.

**Пример:**
```cpp
#include "painter/tui/text.h"
#include "painter/tui/window.h"

void Help(painter::tui::Frame &Out)
{
    namespace tui = painter::tui;
    const painter::core::Rect Inside = tui::DrawWindow(Out, tui::Centred(Out.Size(), 40, 8), "Помощь", "1/2", "Esc — закрыть");
    (void)tui::PutText(Out, {Inside.X, Inside.Y}, "Стрелки — двигать", Inside.Width, tui::WindowInk, tui::WindowPaper);
    tui::DrawRule(Out, Inside, Inside.Y + 2);
}
```

### `DrawRule`

**Краткое описание:** Рисует горизонтальный разделитель внутри окна, соединённый с его рамкой.

**Параметры:**
- `Out` (`Frame &`, out): кадр.
- `Inside` (`core::Rect`): внутренняя область, которую вернул `DrawWindow`.
- `Row` (`std::int32_t`): строка кадра (абсолютная, не относительно `Inside`).

**Возвращает:** `void`.

**Побочные эффекты:** Пишет линию `─` от левой до правой стенки и примыкания `├` и `┤` на месте вертикальных линий рамки.

**Пример:** См. `DrawWindow`.
