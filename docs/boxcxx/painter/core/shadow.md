# painter::core::shadow

## Назначение

Заголовок: [`painter/core/shadow.h`](../../../../src/userspace/painter/include/painter/core/shadow.h).
Реализация: [`src/core/shadow.cpp`](../../../../src/userspace/painter/src/core/shadow.cpp).

Тень любой фигуры — как `box-shadow` в CSS. Внешняя тень показывает вещь,
поднятую над тем, что под ней; внутренняя (`Inner`) — вдавленную.

## Shadow

### `Shadow`

**Краткое описание:** Описывает тень: сдвиг, размытие, расширение, цвет и сторону.

**Параметры:**
- `Offset` (`PointF`): сдвиг.
- `Blur` (`float`): край расплывается на столько пикселей в каждую сторону.
- `Spread` (`float`): расширение фигуры тени (отрицательное — сжатие).
- `Color` (`core::Color`): по умолчанию чёрный с непрозрачностью 96.
- `Inner` (`bool`): внутренняя тень.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `Shadow::MaskFor`.

### `Shadow::MaskFor`

**Краткое описание:** Возвращает маску тени для фигуры.

**Параметры:**
- `Shape` (`const Path &`): фигура.
- `Place` (`const Transform &`): куда она ложится, по умолчанию тождественно.

**Возвращает:** (`std::optional<Mask>`) маска тени; пусто, если фигура отвергнута.

**Побочные эффекты:** Выделяет память; размытие — самая дорогая часть.

**Как работает:** Внешняя: фигура, сдвинутая на `Offset` и расширенная на `Spread`, затем размытая. Внутренняя: всё вокруг фигуры, кроме сдвинутой и сжатой на `Spread` фигуры, размытое и ограниченное самой фигурой.

**Примечания:** Делайте маску один раз и храните, пока фигура не меняет размер: перемещение на целые пиксели передаётся в `DrawShadow` параметром `By`.

**Пример:**
```cpp
#include <optional>

#include "painter/core/canvas.h"

struct Card {
    painter::core::Path            Shape = painter::core::Path::RoundedRect({0, 0, 160, 90}, painter::core::Corners::All(12));
    painter::core::Shadow          Cast{{0, 8}, 18, 0, painter::core::Color::Rgba(0, 0, 0, 100), false};
    std::optional<painter::core::Mask> Kept = Cast.MaskFor(Shape);
};

void Draw(painter::core::Canvas &Out, const Card &Of, painter::core::Point At)
{
    namespace core = painter::core;
    if (Of.Kept) Out.DrawShadow(*Of.Kept, Of.Cast, 1.0f, At);
    (void)Out.Fill(Of.Shape, core::Colors::White, core::Transform::Translate(static_cast<float>(At.X), static_cast<float>(At.Y)));
}
```

### `Shadow::Reach`

**Краткое описание:** Возвращает, на сколько целых пикселей размытие выходит за расширенную и сдвинутую фигуру.

**Параметры:** Нет.

**Возвращает:** (`std::int32_t`) дальность в пикселях.

**Побочные эффекты:** Нет.

**Типичное применение:** Рассчитать прямоугольник, который тень может закрасить (для обрезки или пометки изменившейся области).

**Пример:**
```cpp
#include <algorithm>
#include <cmath>

#include "painter/core/shadow.h"

painter::core::Rect Dirty(const painter::core::Shadow &Cast, painter::core::Rect Body)
{
    const std::int32_t R = Cast.Reach() + static_cast<std::int32_t>(std::ceil(std::max(Cast.Spread, 0.0f)));
    const auto DX = static_cast<std::int32_t>(std::lround(Cast.Offset.X)), DY = static_cast<std::int32_t>(std::lround(Cast.Offset.Y));
    return {Body.X + DX - R, Body.Y + DY - R, Body.Width + 2 * R, Body.Height + 2 * R};
}
```
