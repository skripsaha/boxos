# painter::tui::KeptCells (внутренний)

## Назначение

Заголовок: [`src/tui/kept_cells.h`](../../../../../src/userspace/painter/src/tui/kept_cells.h).
Реализация: [`src/tui/kept_cells.cpp`](../../../../../src/userspace/painter/src/tui/kept_cells.cpp).

Внутренний класс библиотеки; программы его не подключают.

`KeptCells` — ячейки, которые программа держит поверх консоли, прозрачные,
пока в них ничего не положено: общая часть [Surface](../surface.md) и
[ConsoleSurface](../console_surface.md). Перенесённая часть кадра (`Paint`,
`Composite`) показывается сразу; ячейки `ConsoleSurface` в конце
вкладываются в текст консоли (`Commit`). Внутри — [CellsArea](cells_area.md),
кадр размером с консоль (его каретка — каретка области) и запомненный курсор
консоли.

## Класс KeptCells

### `KeptCells::Open`

**Краткое описание:** Открывает область ячеек и прозрачный кадр размером с консоль.

**Параметры:**
- `LeastCols`, `LeastRows` (`std::int32_t`): минимальный размер консоли.
- `Shown` (`CellsArea::Where`): `Over` для `Overlay` и `ConsoleSurface`, `Always` для `Notice`.

**Возвращает:** (`std::expected<KeptCells, StageError>`) ячейки или ошибка `CellsArea::Open` / `FrameUnavailable`.

**Побочные эффекты:** Открывает область, выделяет кадр, очищает его `ClearTransparent`, запоминает курсор консоли из области.

**Пример:**
```cpp
#include "kept_cells.h"   // внутренний заголовок painter/src/tui

bool Open()
{
    auto Kept = painter::tui::KeptCells::Open(0, 0, painter::tui::CellsArea::Where::Over);
    return Kept.has_value();
}
```

### `KeptCells::Paint` / `KeptCells::Composite`

**Краткое описание:** Переносят прямоугольник кадра в сохраняемые ячейки и показывают их.

**Параметры:**
- `Origin` (`core::Point`): место в ячейках.
- `FrameBuffer` (`const Frame &`): источник.
- `Region` (`core::Rect`): прямоугольник источника; без него — весь кадр.

**Возвращает:** (`box::status`) успех; `invalid_state`; `out_of_range` — прямоугольник пуст или не помещается в источник либо в ячейки от `Origin`; ошибка `CellsArea::Present`.

**Побочные эффекты:** Заменяют ячейки прямоугольника; переносят каретку; показывают все ячейки.

**Как работает:** Обе операции копируют ячейки одну за другой. `Paint` принудительно ставит каждой видимость `Opaque`, `Composite` копирует как есть (прозрачная ячейка снимает покрытие). Затем `CarryCaret`: каретка, стоявшая в заменённом прямоугольнике, снимается; каретка источника внутри `Region` переносится на место своей ячейки.

**Пример:**
```cpp
#include "kept_cells.h"   // внутренний заголовок painter/src/tui

box::status Lay(painter::tui::KeptCells &Kept, painter::core::Point Origin, const painter::tui::Frame &Picture)
{
    return Kept.Composite(Origin, Picture);
}
```

### `KeptCells::Clear`

**Краткое описание:** Делает все ячейки прозрачными, ничего не показывая.

**Параметры:** Нет.

**Возвращает:** (`box::status`) успех или `invalid_state`.

**Побочные эффекты:** `ClearTransparent` внутреннего кадра (каретка тоже снимается).

**Пример:**
```cpp
#include "kept_cells.h"   // внутренний заголовок painter/src/tui

void Wipe(painter::tui::KeptCells &Kept) { (void)Kept.Clear(); }
```

### `KeptCells::SetCursor` / `KeptCells::Cursor`

**Краткое описание:** Задают и возвращают место, откуда консоль продолжит вывод после вкладывания.

**Параметры:**
- `At` (`core::Point`): для `SetCursor`.

**Возвращает:** `SetCursor` — (`box::status`) успех, `invalid_state` или `out_of_range` (вне ячеек); `Cursor` — (`std::expected<core::Point, box::error>`) запомненное место или `invalid_state`.

**Побочные эффекты:** `SetCursor` ставит каретку кадра, запоминает курсор и показывает ячейки.

**Примечания:** При вкладывании дисплей ставит курсор консоли в каретку области, поэтому каретка и курсор здесь — одно и то же.

**Пример:**
```cpp
#include "kept_cells.h"   // внутренний заголовок painter/src/tui

void Below(painter::tui::KeptCells &Kept, std::int32_t Row) { (void)Kept.SetCursor({0, Row + 1}); }
```

### `KeptCells::Commit`

**Краткое описание:** Вкладывает ячейки в текст консоли, прокрутив её на заданное число строк.

**Параметры:**
- `ScrollRows` (`std::uint32_t`): прокрутка, по умолчанию 0.

**Возвращает:** (`box::status`) результат `CellsArea::Commit`; `invalid_state`, если уже закрыто.

**Побочные эффекты:** При успехе объект закрыт.

**Пример:**
```cpp
#include "kept_cells.h"   // внутренний заголовок painter/src/tui

box::status Fold(painter::tui::KeptCells &Kept, std::uint32_t ScrollRows) { return Kept.Commit(ScrollRows); }
```

### `KeptCells::Close` и свойства

**Краткое описание:** Закрывает область; `Size`, `IsOpen`, `Glass`, `Mosaics` передают значения области.

**Параметры:** Нет.

**Возвращает:** `Close` — всегда успех; свойства у перемещённого объекта — `{0, 0}`, `false`, `Pixels`, 0.

**Побочные эффекты:** `Close` закрывает область; ячейки исчезают с экрана.

**Примечания:** Копировать нельзя; перемещающее присваивание сначала закрывает прежнее состояние; деструктор вызывает `Close()`.

**Пример:**
```cpp
#include "kept_cells.h"   // внутренний заголовок painter/src/tui

void Drop(painter::tui::KeptCells &Kept) { (void)Kept.Close(); }
```
