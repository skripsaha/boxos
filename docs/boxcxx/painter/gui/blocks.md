# painter::gui::blocks

## Назначение

Заголовок: [`painter/gui/blocks.h`](../../../../src/userspace/painter/include/painter/gui/blocks.h).
Реализация: [`src/gui/area.cpp`](../../../../src/userspace/painter/src/gui/area.cpp).

Блоки — общая память, которую дисплей создаёт для одной программы: всё
большое (пиксели картинок, страницы холстов, файлы шрифтов) лежит в
блоках, а не копируется через кольцо. Элементы называют блок номером, и
один блок могут показывать сколько угодно элементов.

Блоки открывает область: `Area::OpenPicture`, `Area::OpenCanvas`,
`Area::OpenFont` ([area](area.md)). Пределы: до 1024 блоков, до 64 МиБ на
блок, до 256 МиБ всего на программу.

## PictureBlock

### `PictureBlock`

**Краткое описание:** Хранит блок картинки: `Width × Height` пикселей по 4 байта R, G, B, A.

**Параметры:**
- `Id` (`std::uint32_t`): номер блока.
- `Width`, `Height` (`std::uint32_t`): размер в пикселях.
- `Memory` (`box::bay<std::byte>`): общая память блока.

**Возвращает:** Неприменимо: агрегат. Перемещается вместе с памятью.

**Побочные эффекты:** Нет.

**Примечания:** Непрозрачность в пикселях не умножена на цвет (как отдаёт PNG- или JPEG-декодер), цвета — числа sRGB. После того как картинка показана, изменение пикселей нужно объявить: `Area::BlockChanged(Id, прямоугольник)`. Уничтожение объекта не закрывает блок у дисплея — для этого `Area::CloseBlock(Id)`.

**Пример:**
```cpp
#include <cstdint>

#include "painter/gui/area.h"
#include "painter/gui/items.h"

// Шахматная доска 8×8 пикселей, показанная в 160×160 точек квадратными пикселями.
painter::gui::Picture Checker(painter::gui::Area &Screen)
{
    auto Block = Screen.OpenPicture(8, 8);
    if (!Block) return {};
    const std::span<std::uint8_t> Pixels = Block->Pixels();
    for (std::uint32_t Y = 0; Y < 8; Y++)
        for (std::uint32_t X = 0; X < 8; X++) {
            const std::uint8_t Shade = (X + Y) % 2 ? 0x30 : 0xE0;
            std::uint8_t *At = &Pixels[(Y * 8 + X) * 4];
            At[0] = At[1] = At[2] = Shade;
            At[3] = 255;
        }
    painter::gui::Picture Made = Screen.Picture(*Block, ITEM_FIT_STRETCH, true);
    Made.SetSize({160, 160});
    return Made;
}
```

### `PictureBlock::Pixels`

**Краткое описание:** Возвращает пиксели картинки для записи.

**Параметры:** Нет.

**Возвращает:** (`std::span<std::uint8_t>`) `Width × Height × 4` байт построчно; пусто, если памяти нет.

**Побочные эффекты:** Нет.

**Пример:** См. `PictureBlock`.

## CanvasBlock

### `CanvasBlock`

**Краткое описание:** Хранит блок холста: две страницы пикселей движка (`core::Pixel`, 8 байт на пиксель).

**Параметры:**
- `Id`, `Width`, `Height`, `Memory`: как у `PictureBlock`.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Как работает:** Пока одна страница у дисплея, на другой рисует программа. В заголовке блока у каждой страницы есть признак «свободна»: программа рисует только на свободной, отдаёт её (`Taken`) и сообщает `BlockChanged` с номером страницы; дисплей показывает её прямо из блока и держит, пока не получит другую, — тогда возвращает прежнюю (событие `PageFree`).

**Примечания:** Объект `gui::Canvas` ([items](items.md)) ведёт страницы сам; `CanvasBlock` напрямую нужен для собственных схем показа.

**Пример:**
```cpp
#include "painter/gui/blocks.h"

bool Ready(const painter::gui::CanvasBlock &Pages) { return Pages.Free(0) || Pages.Free(1); }
```

### `CanvasBlock::Free`

**Краткое описание:** Сообщает, свободна ли страница для рисования.

**Параметры:**
- `Page` (`std::uint32_t`): 0 или 1.

**Возвращает:** (`bool`) `false` и для неверного номера, и без памяти.

**Побочные эффекты:** Нет (атомарное чтение с семантикой acquire).

**Пример:** См. `CanvasBlock`.

### `CanvasBlock::Page`

**Краткое описание:** Возвращает холст движка поверх свободной страницы.

**Параметры:**
- `Page` (`std::uint32_t`): 0 или 1.

**Возвращает:** (`std::optional<core::Canvas>`) холст, или пусто, если страница не свободна.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/gui/blocks.h"

void Clear(painter::gui::CanvasBlock &Pages)
{
    if (auto Page = Pages.Page(0)) Page->Clear(painter::core::Colors::Black);
}
```

### `CanvasBlock::Taken`

**Краткое описание:** Отмечает страницу как отданную дисплею.

**Параметры:**
- `Page` (`std::uint32_t`): 0 или 1.

**Возвращает:** `void`.

**Побочные эффекты:** Сбрасывает признак «свободна» (атомарная запись с семантикой release). После этого программа должна сообщить `BlockChanged(Id, область, Page)`.

**Пример:**
```cpp
#include "painter/gui/area.h"

void Give(painter::gui::Area &Screen, painter::gui::CanvasBlock &Pages, std::uint32_t Page)
{
    Pages.Taken(Page);
    Screen.BlockChanged(Pages.Id, {0, 0, static_cast<std::int32_t>(Pages.Width), static_cast<std::int32_t>(Pages.Height)}, Page);
    (void)Screen.Present();
}
```

### `CanvasBlock::PageAt` / `CanvasBlock::PageBytes`

**Краткое описание:** Возвращают пиксели страницы независимо от её состояния и размер страницы в байтах.

**Параметры:**
- `Page` (`std::uint32_t`): 0 или 1 — для `PageAt`.

**Возвращает:** `PageAt` — (`core::Pixel *`) или `nullptr`; `PageBytes` — (`std::size_t`).

**Побочные эффекты:** Нет.

**Примечания:** Писать в страницу, которую держит дисплей, нельзя — это испортит показываемую картинку. Чтение разрешено (так `gui::Canvas` копирует неизменившуюся часть).

**Пример:**
```cpp
#include "painter/gui/blocks.h"

std::size_t Total(const painter::gui::CanvasBlock &Pages) { return Pages.PageBytes() * 2; }
```
