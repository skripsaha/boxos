# painter::tui::CellsArea (внутренний)

## Назначение

Заголовок: [`src/tui/cells_area.h`](../../../../../src/userspace/painter/src/tui/cells_area.h).
Реализация: [`src/tui/cells_area.cpp`](../../../../../src/userspace/painter/src/tui/cells_area.cpp).

Внутренний класс библиотеки; программы его не подключают. Страница — для
тех, кто сопровождает Painter.

`CellsArea` — ячейки программы на экране на языке элементов
(`box/items.h`): область дисплея (`painter::gui::Area`) с единственным
элементом-сеткой (`Kind::Cells`, номер 1) размером с консоль и на её сетке,
поэтому кадр ячеек выглядит так же, как консоль. Все проверки, общие для
поверхностей `painter::tui` (размер консоли, минимум, предел сетки),
сделаны здесь один раз. [Stage](../stage.md) держит `CellsArea` напрямую,
[Surface](../surface.md) и [ConsoleSurface](../console_surface.md) — через
[KeptCells](kept_cells.md).

## Типы

### `CellsArea::Where`

**Краткое описание:** Выбирает, где и когда видны ячейки.

**Параметры:**
- `Screen`: весь экран, пока программа впереди (сцена) — `Placement::Whole`.
- `Over`: поверх всего, пока программа впереди (накладка) — `Placement::Over`.
- `Always`: поверх всего всегда, впереди программа или позади (уведомление) — `Placement::Over` с `Always = true`.

**Возвращает:** Неприменимо: перечисление `std::uint8_t`.

**Побочные эффекты:** Нет.

**Пример:** См. `CellsArea::Open`.

## Класс CellsArea

### `CellsArea::Open`

**Краткое описание:** Открывает область с сеткой ячеек размером с консоль.

**Параметры:**
- `LeastCols`, `LeastRows` (`std::int32_t`): минимальный размер консоли.
- `Shown` (`Where`): где видны ячейки.

**Возвращает:** (`std::expected<CellsArea, StageError>`) область или ошибка: `TooSmall` (отрицательный минимум — `invalid_argument`, консоль меньше минимума — `out_of_range`), `DimensionsUnavailable`, `NoConsole`, `FrameUnavailable` (консоль больше `ITEM_GRID_SIDE_MOST` или нет памяти), `InvalidState` (дисплей отказал в области).

**Побочные эффекты:** Вызывает `display_size`, открывает область, создаёт элемент-сетку и задаёт её размер.

**Как работает:** Для `Over` и `Always` область запрашивается с наибольшим допустимым размером (`(ITEM_REACH − 1) / ITEM_POINT` точек), и дисплей ограничивает её экраном. Сразу после открытия вызывается `Listen(false)`: программа ячеек ждёт клавиш, а не слов области, и неразобранный Touch-сигнал дисплея не мешал бы потоку спать. Вид стекла, мозаики и курсор консоли берутся из ответа дисплея при открытии. Буфер строки `ItemCell` выделяется один раз.

**Пример:**
```cpp
#include "cells_area.h"   // внутренний заголовок painter/src/tui

bool Overlay()
{
    auto Area = painter::tui::CellsArea::Open(0, 0, painter::tui::CellsArea::Where::Over);
    return Area.has_value();
}
```

### `CellsArea::Size` / `IsOpen` / `Glass` / `Mosaics` / `Cursor`

**Краткое описание:** Возвращают размер сетки, признак открытости, вид стекла, точные мозаики и курсор консоли на момент открытия.

**Параметры:** Нет.

**Возвращает:** `core::Size`, `bool`, `GlassKind`, `std::uint8_t`, `core::Point` соответственно. `Cursor` — где стояла каретка консоли при открытии, после всего, что поток напечатал раньше.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "cells_area.h"   // внутренний заголовок painter/src/tui

painter::core::Point Start(const painter::tui::CellsArea &Area) { return Area.Cursor(); }
```

### `CellsArea::Present`

**Краткое описание:** Передаёт дисплею кадр как один шаг: изменившиеся ячейки и каретку.

**Параметры:**
- `Picture` (`const Frame &`): кадр ровно размером с сетку.

**Возвращает:** (`box::status`) успех; `invalid_state` — область закрыта; `invalid_argument` — размер кадра не совпадает; `no_memory`.

**Побочные эффекты:** Изменяет запомненные ячейки в `Changes`, кладёт шаг в кольцо области; может ждать места в кольце.

**Как работает:** Каждая строка кадра переводится в `ItemCell`: прозрачная ячейка — `item_cell_empty()`, остальные — символ, цвета в виде `Color::Raw()` и отметка. `Changes::Cells` сравнивает строку с запомненной и отмечает отрезок изменившихся ячеек. Затем передаётся каретка, разбираются все ответы дисплея (`Next()`), чтобы у него было место говорить, и шаг уходит целиком (`PresentAll`) — половина кадра не показывается никогда.

**Пример:**
```cpp
#include "cells_area.h"   // внутренний заголовок painter/src/tui

box::status Show(painter::tui::CellsArea &Area, const painter::tui::Frame &Picture)
{
    if (auto Shown = Area.Present(Picture); !Shown) return Shown;
    return {};
}
```

### `CellsArea::Commit`

**Краткое описание:** Вкладывает ячейки области в текст консоли и закрывает область.

**Параметры:**
- `ScrollRows` (`std::uint32_t`): на сколько строк прокрутить консоль перед вкладыванием; не больше высоты.

**Возвращает:** (`box::status`) успех; `invalid_state`; `out_of_range` — прокрутка больше высоты; `no_memory`; ошибка дисплея.

**Побочные эффекты:** `gui::Area::Commit` — команда `DISP_CMD_AREA_COMMIT`; при успехе область закрыта.

**Пример:**
```cpp
#include "cells_area.h"   // внутренний заголовок painter/src/tui

box::status Keep(painter::tui::CellsArea &Area, std::uint32_t Scrolled)
{
    if (auto Done = Area.Commit(Scrolled); !Done) return Done;
    return {};
}
```

### `CellsArea::Close`

**Краткое описание:** Закрывает область; её ячейки исчезают с экрана.

**Параметры:** Нет.

**Возвращает:** (`box::status`) всегда успех.

**Побочные эффекты:** Уничтожает `gui::Area`.

**Примечания:** Перемещающее присваивание запрещено, перемещающий конструктор есть; деструктор по умолчанию закрывает область через `gui::Area`.

**Пример:**
```cpp
#include "cells_area.h"   // внутренний заголовок painter/src/tui

void Drop(painter::tui::CellsArea &Area) { (void)Area.Close(); }
```
