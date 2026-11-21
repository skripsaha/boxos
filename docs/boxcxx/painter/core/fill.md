# painter::core::fill

## Назначение

Заголовок: [`painter/core/fill.h`](../../../../src/userspace/painter/include/painter/core/fill.h).
Реализация: [`src/core/fill.cpp`](../../../../src/userspace/painter/src/core/fill.cpp).

Чем заливается фигура: один цвет, градиент или картинка, с собственной
непрозрачностью. У фигуры может быть несколько заливок, одна поверх
другой; покрытие фигуры при этом считается один раз.

## Типы

### `FillRule`

**Краткое описание:** Называет правило, по которому пиксель принадлежит фигуре.

**Параметры:**
- `NonZero`: контуры вообще обходят пиксель (контур в обратную сторону вырезает дыру).
- `EvenOdd`: контуры обходят пиксель нечётное число раз.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/canvas.h"

bool Donut(painter::core::Canvas &Out)
{
    namespace core = painter::core;
    const core::Path Two = core::Path().AddCircle({50, 50}, 40).AddCircle({50, 50}, 20);
    return Out.Fill(Two, core::Colors::Leaf, {}, core::FillRule::EvenOdd);
}
```

## Fill

### `Fill`

**Краткое описание:** Хранит одну заливку: цвет, градиент или картинку и её непрозрачность.

**Параметры:**
- `Fill(core::Color Solid)` — цвет; конструктор неявный, поэтому цвет можно передавать прямо туда, где ждут `Fill`.
- `Fill(core::Gradient Colors)` — градиент; тоже неявный.
- `Fill::Picture(...)` — картинка (ниже).

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/fill.h"

const painter::core::Fill Ink = painter::core::Colors::White;
```

### `Fill::Picture`

**Краткое описание:** Создаёт заливку картинкой.

**Параметры:**
- `Source` (`std::shared_ptr<const core::Picture>`): картинка.
- `Place` (`const Transform &`): куда ложится картинка; пиксель (I, J) картинки занимает от I до I+1 и от J до J+1 в пространстве контура после `Place`. По умолчанию тождественно.
- `Past` (`Extend`): что за краем картинки, по умолчанию `None`.
- `Sharp` (`bool`): пиксели квадратами при увеличении (пиксель-арт), по умолчанию `false` — сглаженно.

**Возвращает:** (`Fill`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/canvas.h"

bool Tile(painter::core::Canvas &Out, std::shared_ptr<const painter::core::Picture> Brick)
{
    namespace core = painter::core;
    const core::Fill Wall = core::Fill::Picture(Brick, core::Transform::Scale(4), core::Extend::Repeat, true);
    return Out.Fill(core::Path::Rect({0, 0, 256, 128}), Wall);
}
```

### `Fill::WithOpacity`

**Краткое описание:** Задаёт непрозрачность заливки.

**Параметры:**
- `Of` (`float`): 0–1.

**Возвращает:** (`Fill &`) эта заливка.

**Побочные эффекты:** Меняет заливку.

**Пример:**
```cpp
#include "painter/core/fill.h"

painter::core::Fill Glaze() { return painter::core::Fill(painter::core::Colors::White).WithOpacity(0.25f); }
```

### `Fill::What` / `Fill::Seen`

**Краткое описание:** Возвращают содержимое заливки и её непрозрачность.

**Параметры:** Нет.

**Возвращает:** `What` — (`const std::variant<core::Color, core::Gradient, PictureFill> &`); `Seen` — (`float`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include <variant>

#include "painter/core/fill.h"

bool Solid(const painter::core::Fill &Of) { return std::holds_alternative<painter::core::Color>(Of.What()); }
```
