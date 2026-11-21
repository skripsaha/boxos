# painter::core::transform

## Назначение

Заголовок: [`painter/core/transform.h`](../../../../src/userspace/painter/include/painter/core/transform.h).
Реализация: [`src/core/transform.cpp`](../../../../src/userspace/painter/src/core/transform.cpp).

Аффинное преобразование: куда в пикселях попадает точка собственного
пространства фигуры. То же, что `matrix(a b c d e f)` в SVG:

`x' = A·x + C·y + E`, `y' = B·x + D·y + F`.

Ось Y направлена вниз, поэтому положительный поворот — по часовой стрелке
на экране.

## Transform

### `Transform`

**Краткое описание:** Хранит шесть коэффициентов аффинного преобразования.

**Параметры:**
- `A`, `B`, `C`, `D`, `E`, `F` (`float`): по умолчанию тождественное (1, 0, 0, 1, 0, 0).

**Возвращает:** Неприменимо: агрегат; `==` поэлементное.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/transform.h"

constexpr painter::core::Transform Mirror{-1, 0, 0, 1, 100, 0};   // отражение по X со сдвигом
```

### `Identity` / `Translate` / `Scale`

**Краткое описание:** Возвращают тождественное преобразование, перенос и масштаб.

**Параметры:**
- `Translate(X, Y)` (`float`).
- `Scale(X, Y)` или `Scale(Both)` (`float`).

**Возвращает:** (`Transform`); все `constexpr`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/transform.h"

constexpr auto Double = painter::core::Transform::Scale(2);
```

### `Rotate` / `Skew`

**Краткое описание:** Возвращают поворот вокруг (0, 0) и наклон.

**Параметры:**
- `Degrees` (`float`): поворот по часовой на экране.
- `XDegrees`, `YDegrees` (`float`): X смещается на тангенс `XDegrees` от Y, Y — на тангенс `YDegrees` от X.

**Возвращает:** (`Transform`).

**Побочные эффекты:** Нет.

**Примечания:** Поворот на целое число четвертей оборота точен (без погрешности синуса).

**Пример:**
```cpp
#include "painter/core/transform.h"

painter::core::Transform Italic() { return painter::core::Transform::Skew(-12, 0); }
```

### `Then`

**Краткое описание:** Возвращает преобразование «сначала это, потом `Next`».

**Параметры:**
- `Next` (`const Transform &`).

**Возвращает:** (`Transform`).

**Побочные эффекты:** Нет.

**Типичное применение:** Поворот вокруг произвольной точки: перенести точку в начало, повернуть, перенести обратно.

**Пример:**
```cpp
#include "painter/core/transform.h"

painter::core::Transform TurnAbout(painter::core::PointF Middle, float Degrees)
{
    using painter::core::Transform;
    return Transform::Translate(-Middle.X, -Middle.Y).Then(Transform::Rotate(Degrees)).Then(Transform::Translate(Middle.X, Middle.Y));
}
```

### `Apply` / `Determinant` / `Inverse`

**Краткое описание:** Применяют преобразование к точке, возвращают определитель и обратное преобразование.

**Параметры:**
- `Place` (`PointF`): для `Apply`.

**Возвращает:** `Apply` — (`PointF`); `Determinant` — (`float`) `A·D − B·C`; `Inverse` — (`std::optional<Transform>`), пусто, если преобразование сплющивает плоскость.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/transform.h"

bool Flat(const painter::core::Transform &Of) { return !Of.Inverse().has_value(); }
```
