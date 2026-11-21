# painter::core::gradient

## Назначение

Заголовок: [`painter/core/gradient.h`](../../../../src/userspace/painter/include/painter/core/gradient.h).
Реализация: [`src/core/gradient.cpp`](../../../../src/userspace/painter/src/core/gradient.cpp).

Градиенты: цвета вдоль прямой, от центра наружу или по кругу. Цвета между
остановками идут прямо по числам (`Color::Between`), вычисления — в 16
битах, поэтому широкий тёмный градиент не даёт полос.

## Типы

### `Extend`

**Краткое описание:** Называет, что лежит за концами градиента или за краем картинки.

**Параметры:**
- `Pad`: крайние цвета (пиксели) продолжаются.
- `Repeat`: начинается заново.
- `Reflect`: идёт обратно.
- `None`: ничего (прозрачно).

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Пример:** См. `Gradient::Linear`.

### `GradientStop`

**Краткое описание:** Хранит цвет в заданном месте градиента.

**Параметры:**
- `Offset` (`float`): 0 — начало, 1 — конец.
- `Color` (`core::Color`).

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `Gradient::Linear`.

## Gradient

### `Gradient::Linear`

**Краткое описание:** Создаёт линейный градиент от одной точки к другой.

**Параметры:**
- `From`, `To` (`PointF`): начало и конец; линии равного цвета перпендикулярны отрезку между ними.
- `Stops` (`std::vector<GradientStop>`): остановки.
- `Past` (`Extend`): по умолчанию `Pad`.

**Возвращает:** (`Gradient`).

**Побочные эффекты:** Нет.

**Как работает:** Остановки упорядочиваются по позиции, позиции ограничиваются диапазоном 0–1.

**Примечания:** Градиент без остановок отвергается при рисовании.

**Пример:**
```cpp
#include "painter/core/gradient.h"

painter::core::Gradient Stripes()
{
    namespace core = painter::core;
    return core::Gradient::Linear({0, 0}, {20, 0}, {{0.0f, core::Colors::Black}, {1.0f, core::Colors::White}}, core::Extend::Reflect);
}
```

### `Gradient::Radial`

**Краткое описание:** Создаёт радиальный градиент от центра до радиуса, при желании из фокуса.

**Параметры:**
- `Middle` (`PointF`), `Radius` (`float`): окружность конца.
- `Focus` (`PointF`): точка внутри окружности, откуда начинается градиент; во второй перегрузке.
- `Stops`, `Past`: как у `Linear`.

**Возвращает:** (`Gradient`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/gradient.h"

painter::core::Gradient Ball()
{
    namespace core = painter::core;
    return core::Gradient::Radial({50, 50}, 40, {35, 35}, {{0.0f, core::Colors::White}, {1.0f, core::Colors::Blue}});
}
```

### `Gradient::Conic`

**Краткое описание:** Создаёт конический градиент вокруг центра.

**Параметры:**
- `Middle` (`PointF`).
- `StartDegrees` (`float`): начальный угол, 0 — вверх, по часовой (как в CSS).
- `Stops` (`std::vector<GradientStop>`).

**Возвращает:** (`Gradient`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/gradient.h"

painter::core::Gradient Wheel()
{
    namespace core = painter::core;
    return core::Gradient::Conic({50, 50}, 0, {{0.0f, core::Colors::Red}, {0.5f, core::Colors::Cyan}, {1.0f, core::Colors::Red}});
}
```

### `Gradient::Transformed`

**Краткое описание:** Кладёт градиент сначала в собственное пространство.

**Параметры:**
- `Within` (`const Transform &`).

**Возвращает:** (`Gradient &`) этот градиент.

**Побочные эффекты:** Меняет пространство градиента.

**Типичное применение:** Радиальный градиент, растянутый по одной оси, — эллипс.

**Пример:**
```cpp
#include "painter/core/gradient.h"

painter::core::Gradient Oval(painter::core::Gradient Round) { return Round.Transformed(painter::core::Transform::Scale(2, 1)); }
```

### `Past` / `Of` / `Stops` / `Space` / `Focus`

**Краткое описание:** Возвращают поведение за концами, вид, остановки, пространство и фокус градиента.

**Параметры:** Нет.

**Возвращает:** `Past` — (`Extend`); `Of` — (`Gradient::Kind`: `Linear`, `Radial`, `Conic`); `Stops` — (`const std::vector<GradientStop> &`); `Space` — (`const Transform &`) отображение собственного пространства (начало в (0, 0), конец в (1, 0) для линейного, единичная окружность для остальных) в пространство контура; `Focus` — (`PointF`) фокус радиального в собственном пространстве.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/gradient.h"

bool Usable(const painter::core::Gradient &Colors) { return !Colors.Stops().empty(); }
```
