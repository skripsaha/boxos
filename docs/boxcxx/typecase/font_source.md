# typecase::font_source

## Назначение

Заголовок: [`typecase/font_source.h`](../../../src/userspace/typecase/include/typecase/font_source.h).
Реализация: [`src/font_source.cpp`](../../../src/userspace/typecase/src/font_source.cpp).

Шрифт TrueType как источник: символы, которые его карта называет глифом,
который читается, нарисованные по em размера с пером на базовой линии
ячейки так, как их рисует текст ([glyph_look](glyph_look.md)); высота
строчных — целое число пикселей.

## Класс FontSource

### `FontSource`

**Краткое описание:** Создаёт источник из шрифта.

**Параметры:**
- `Face` (`Font`): шрифт; источник хранит свою копию.

**Возвращает:** Неприменимо: конструктор (`explicit`).

**Побочные эффекты:** Нет.

**Пример:** См. [chain](chain.md).

### `Of` / `Has` / `Draw`

**Краткое описание:** Возвращают шрифт источника, сообщают, есть ли символ, и рисуют его.

**Параметры:**
- `Character` (`char32_t`), `At` (`const Size &`).

**Возвращает:** `Of` — `const Font &`; `Has` — `bool` (глиф есть и читается); `Draw` — `std::optional<GlyphImage>`.

**Побочные эффекты:** `Draw` использует собственный растеризатор.

**Пример:**
```cpp
#include "typecase/font_source.h"

bool Knows(const typecase::FontSource &Source, char32_t Character) { return Source.Has(Character); }
```
