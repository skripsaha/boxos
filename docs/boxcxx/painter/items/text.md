# painter::items::text (внутренний)

## Назначение

Заголовок: [`painter/items/text.h`](../../../../src/userspace/painter/include/painter/items/text.h).
Реализация: [`src/items/text.cpp`](../../../../src/userspace/painter/src/items/text.cpp).

Текстовый элемент, разложенный так, как его кладёт дисплей: стиль роли под
тем, что сказал элемент ([look](look.md)), его отрезки, его рамка — всё в
точках (масштаб экрана применяется только при рисовании) — и цвета, которые
называют отрезки. Один путь и для дисплея, который рисует, и для
программы, которая измеряет заранее (`gui::Measure`).

## Типы

### `LaidText`

**Краткое описание:** Хранит разложенный текстовый элемент.

**Параметры:**
- `Laid` (`typecase::Layout`): раскладка.
- `Inks` (`std::vector<core::Color>`): цвета, которые называют отрезки; чернила 1 и далее.
- `Measured` (`ItemPoint`): размер по содержимому в единицах `ITEM_POINT` — вместе с плашкой, если она есть.
- `Inset` (`ItemPoint`): где слова начинаются от угла элемента (отступ внутри плашки).

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `LayText`.

## Функции

### `LayText`

**Краткое описание:** Раскладывает текстовый элемент так, как его кладёт дисплей.

**Параметры:**
- `Said` (`const Properties &`): свойства элемента.
- `Role` (`std::uint16_t`): роль.
- `Look` (`const Theme &`): тема.
- `Book` (`const typecase::Fonts &`): шрифты.

**Возвращает:** (`LaidText`).

**Побочные эффекты:** Нет; бросает только `std::bad_alloc`.

**Как работает:** Ширина и высота рамки — размер элемента минус удвоенный отступ плашки (если размер не по содержимому). Измеренный размер — размер раскладки плюс удвоенный отступ.

**Пример:**
```cpp
#include "painter/core/system_fonts.h"
#include "painter/items/text.h"

std::int32_t WidthOf(const painter::items::Properties &Said)
{
    const auto Fonts = painter::core::SystemFonts();
    if (!Fonts) return 0;
    return painter::items::LayText(Said, ITEM_ROLE_BODY, painter::items::Theme{}, *Fonts).Measured.X / ITEM_POINT;
}
```
