# painter::core::mask

## Назначение

Заголовок: [`painter/core/mask.h`](../../../../src/userspace/painter/include/painter/core/mask.h).
Реализация: [`src/core/mask.cpp`](../../../../src/userspace/painter/src/core/mask.cpp).

Маска — сколько покрывает что-то каждый пиксель прямоугольника, 0–255:
глиф, тень, обрезка. Делается один раз, хранится и накладывается с любой
заливкой сколько угодно раз (`Canvas::FillMask`). Это выгодно, когда одна и
та же фигура рисуется многократно.

## Mask

### `Mask`

**Краткое описание:** Хранит прямоугольник покрытия.

**Параметры:**
- `Mask()` — пустая маска.
- `Mask(Rect Area)` — маска над `Area`, пока ничего не покрывающая.

**Возвращает:** Неприменимо: класс; копируется и перемещается.

**Побочные эффекты:** Выделяет `Width × Height` байт.

**Пример:**
```cpp
#include "painter/core/mask.h"

painter::core::Mask Box() { return painter::core::Mask({0, 0, 16, 16}); }
```

### `Mask::Of`

**Краткое описание:** Возвращает покрытие фигуры так, как его покрыл бы холст.

**Параметры:**
- `Shape` (`const Path &`), `Place` (`const Transform &`), `Rule` (`FillRule`).
- `Within` (`Rect`): прямоугольник, которым ограничена маска.

**Возвращает:** (`std::optional<Mask>`) маска; пусто, если фигура отвергнута (дальше `BOX_SHAPE_REACH` или преобразование растягивает слишком сильно).

**Побочные эффекты:** Выделяет память.

**Пример:**
```cpp
#include "painter/core/canvas.h"

void Stamp(painter::core::Canvas &Out)
{
    namespace core = painter::core;
    const auto Star = core::Mask::Of(core::Path::Circle({8, 8}, 6), {}, core::FillRule::NonZero, {0, 0, 16, 16});
    if (!Star) return;
    for (int I = 0; I < 10; I++) (void)Out.FillMask(Star->Moved({I * 20, 0}), core::Colors::Amber);
}
```

### `Area` / `Empty` / `At` / `Row`

**Краткое описание:** Возвращают прямоугольник маски, признак пустоты, покрытие пикселя и строку значений.

**Параметры:**
- `X`, `Y` (`std::int32_t`): для `At` — пиксель; для `Row` — строка внутри маски.

**Возвращает:** `Area` — (`Rect`); `Empty` — (`bool`); `At` — (`std::uint8_t`), 0 вне маски; `Row` — указатель на `Area().Width` значений строки (изменяемый и константный варианты).

**Побочные эффекты:** Нет.

**Примечания:** `Row` с `Y` вне маски — неопределённое поведение (проверки нет).

**Пример:**
```cpp
#include "painter/core/mask.h"

int Covered(const painter::core::Mask &Of)
{
    int Count = 0;
    for (std::int32_t Y = Of.Area().Y; Y < Of.Area().Y + Of.Area().Height; Y++)
        for (std::int32_t X = 0; X < Of.Area().Width; X++) Count += Of.Row(Y)[X] != 0;
    return Count;
}
```

### `Mask::Blur`

**Краткое описание:** Размывает маску, как размывается тень.

**Параметры:**
- `Radius` (`float`): край расплывается на `Radius` пикселей в каждую сторону (гауссово размытие с σ = `Radius / 2`, как в CSS).

**Возвращает:** `void`.

**Побочные эффекты:** Увеличивает прямоугольник маски на дальность размытия.

**Пример:**
```cpp
#include "painter/core/mask.h"

void Soften(painter::core::Mask &Glow) { Glow.Blur(8); }
```

### `Mask::Moved`

**Краткое описание:** Возвращает то же покрытие, сдвинутое на целое число пикселей.

**Параметры:**
- `By` (`Point`): сдвиг.

**Возвращает:** (`Mask`) копия со сдвинутым прямоугольником.

**Побочные эффекты:** Выделяет память под копию.

**Пример:** См. `Mask::Of`.
