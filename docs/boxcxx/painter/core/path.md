# painter::core::path

## Назначение

Заголовок: [`painter/core/path.h`](../../../../src/userspace/painter/include/painter/core/path.h).
Реализация: [`src/core/path.cpp`](../../../../src/userspace/painter/src/core/path.cpp).

Контур фигуры: замкнутые и открытые линии из отрезков и кривых в
собственных единицах (пиксели, пока `Transform` не положит их иначе), а
также готовые прямоугольники, скруглённые прямоугольники и эллипсы,
которые хранятся как есть и рисуются точнее приближения кривыми. Края
заливки и обводки одни и те же (`boxos_shape.h`).

Контур, обойдённый в обратную сторону, вырезает дыру при правиле
`FillRule::NonZero`.

## Corners

### `Corners`

**Краткое описание:** Хранит радиусы четырёх углов прямоугольника.

**Параметры:**
- `TopLeft`, `TopRight`, `BottomRight`, `BottomLeft` (`float`): радиусы четверти окружности.

**Возвращает:** Неприменимо: агрегат. `Corners::All(Radius)` — все одинаковые.

**Побочные эффекты:** Нет.

**Как работает:** Если два угла одной стороны перекрылись бы, все радиусы уменьшаются в одной пропорции до касания — как в CSS.

**Пример:**
```cpp
#include "painter/core/path.h"

constexpr painter::core::Corners Tab{12, 12, 0, 0};   // скруглён только верх
```

## Path

### `Path`

**Краткое описание:** Хранит контур как список шагов и точек.

**Параметры:** Конструктор по умолчанию — пустой контур.

**Возвращает:** Неприменимо: класс; копируется и перемещается.

**Побочные эффекты:** Нет.

**Как работает:** Шаги (`Path::Step`): `Move`, `Line`, `Quad`, `Cubic`, `Close`, `Rect`, `Rounded`, `Ellipse`. Каждый берёт свои точки из общего списка. Все методы построения возвращают `Path &` и цепляются.

**Пример:**
```cpp
#include "painter/core/path.h"

painter::core::Path Triangle()
{
    return painter::core::Path().MoveTo({0, 40}).LineTo({20, 0}).LineTo({40, 40}).Close();
}
```

### `MoveTo` / `LineTo` / `QuadTo` / `CubicTo` / `Close`

**Краткое описание:** Начинают новый контур, добавляют отрезок, квадратичную и кубическую кривую, замыкают контур.

**Параметры:**
- `To` (`PointF`): конечная точка.
- `Pull` (`PointF`): управляющая точка квадратичной кривой.
- `Pull1`, `Pull2` (`PointF`): управляющие точки кубической.

**Возвращает:** (`Path &`).

**Побочные эффекты:** Добавляют шаги и точки.

**Как работает:** Отрезок или кривая без открытого контура начинают контур там, где стоит перо. `Close` добавляет отрезок к началу контура.

**Пример:**
```cpp
#include "painter/core/path.h"

painter::core::Path Wave()
{
    return painter::core::Path().MoveTo({0, 20}).CubicTo({20, 0}, {40, 40}, {60, 20});
}
```

### `ArcTo`

**Краткое описание:** Добавляет дугу эллипса до точки, как команда `A` в SVG.

**Параметры:**
- `RadiusX`, `RadiusY` (`float`): радиусы эллипса.
- `TurnDegrees` (`float`): поворот эллипса.
- `Large` (`bool`): большая дуга.
- `Clockwise` (`bool`): по часовой.
- `To` (`PointF`): конец.

**Возвращает:** (`Path &`).

**Побочные эффекты:** Добавляет кубические кривые — не больше четверти оборота каждая.

**Пример:**
```cpp
#include "painter/core/path.h"

painter::core::Path HalfMoon() { return painter::core::Path().MoveTo({0, 0}).ArcTo(20, 20, 0, false, true, {0, 40}).Close(); }
```

### `AddRect` / `AddRoundedRect` / `AddEllipse` / `AddCircle`

**Краткое описание:** Добавляют готовую фигуру отдельным контуром.

**Параметры:**
- `Area` (`RectF`): прямоугольник.
- `Round` (`Corners`): скругления.
- `Middle` (`PointF`), `RadiusX`, `RadiusY`, `Radius` (`float`): эллипс и круг.

**Возвращает:** (`Path &`).

**Побочные эффекты:** Добавляют шаг готовой фигуры.

**Пример:**
```cpp
#include "painter/core/path.h"

painter::core::Path Ring()   // круг с дырой по правилу EvenOdd
{
    return painter::core::Path().AddCircle({50, 50}, 40).AddCircle({50, 50}, 25);
}
```

### `Rect` / `RoundedRect` / `Ellipse` / `Circle` / `Line`

**Краткое описание:** Возвращают новый контур из одной готовой фигуры или отрезка.

**Параметры:** Как у соответствующих `Add…`; `Line(From, To)` — отрезок.

**Возвращает:** (`Path`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/path.h"

const painter::core::Path Button = painter::core::Path::RoundedRect({0, 0, 120, 36}, painter::core::Corners::All(8));
```

### `FromSvg`

**Краткое описание:** Читает данные пути SVG (атрибут `d`).

**Параметры:**
- `Data` (`std::string_view`): команды `M L H V C S Q T A Z`, каждая также относительная (строчная).

**Возвращает:** (`std::optional<Path>`) контур; пусто, если хоть что-то не является данными пути — испорченный значок виден как испорченный, а не рисуется наполовину.

**Побочные эффекты:** Нет; может бросить `std::bad_alloc`.

**Пример:**
```cpp
#include "painter/core/path.h"

std::optional<painter::core::Path> Check() { return painter::core::Path::FromSvg("M4 12 l5 5 L20 6"); }
```

### `Steps` / `Points` / `Empty` / `Bounds`

**Краткое описание:** Возвращают шаги, точки, признак пустоты и охватывающий прямоугольник.

**Параметры:** Нет.

**Возвращает:** `Steps` — (`std::span<const Step>`); `Points` — (`std::span<const PointF>`); `Empty` — (`bool`); `Bounds` — (`RectF`) наименьший прямоугольник, содержащий все точки, включая управляющие и готовые фигуры.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/path.h"

float Width(const painter::core::Path &Shape) { return Shape.Bounds().Width; }
```
