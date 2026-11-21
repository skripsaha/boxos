# typecase::glyph_image

## Назначение

Заголовок: [`typecase/glyph_image.h`](../../../src/userspace/typecase/include/typecase/glyph_image.h).

Нарисованный глиф: сколько чернила покрывает каждый пиксель и где картинка
стоит относительно пера. Перо стоит на базовой линии; X идёт вправо, Y —
вниз от неё, поэтому у картинки над базовой линией `Top` отрицательный.
Цвета выбирает тот, кто показывает: покрытие — это чернила, а не свет.

## Типы

### `GlyphImage`

**Краткое описание:** Хранит картинку глифа как покрытие пикселей.

**Параметры:**
- `Left` (`std::int32_t`): первый столбец картинки от пера.
- `Top` (`std::int32_t`): первая строка от базовой линии.
- `Width`, `Height` (`std::int32_t`): размер.
- `Advance` (`float`): на сколько пикселей перо уходит дальше.
- `Coverage` (`std::vector<std::uint8_t>`): `Width × Height` построчно; 0 — пиксель не покрыт, 255 — покрыт целиком.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/glyph_image.h"

// Покрытие пикселя (X, Y) относительно пера; 0 вне картинки.
std::uint8_t InkAt(const typecase::GlyphImage &Glyph, std::int32_t X, std::int32_t Y)
{
    const std::int32_t Col = X - Glyph.Left, Row = Y - Glyph.Top;
    if (Col < 0 || Row < 0 || Col >= Glyph.Width || Row >= Glyph.Height) return 0;
    return Glyph.Coverage[static_cast<std::size_t>(Row) * static_cast<std::size_t>(Glyph.Width) + static_cast<std::size_t>(Col)];
}
```
