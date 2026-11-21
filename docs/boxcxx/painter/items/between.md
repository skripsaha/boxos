# painter::items::between (внутренний)

## Назначение

Заголовок: [`painter/items/between.h`](../../../../src/userspace/painter/include/painter/items/between.h).
Реализация: [`src/items/between.cpp`](../../../../src/userspace/painter/src/items/between.cpp).

Значения между двумя значениями свойства — то, что анимация показывает по
пути (`ITEM_OP_ANIMATE`). Свойство из чисел и цветов читается как числа,
которые идут (место, размер, поворот, скругления, точки контура, цвета и
градиенты заливок, обводка, тени, кегль и разрядка; цвета — как видит
глаз, через OKLab, каждый с весом своей непрозрачности), и форма, которая
не идёт. Два значения идут друг к другу, только если у них одна форма.
Свойство без промежуточных чисел (символы, картинка, роль) не идёт:
дисплей плавно сменяет один вид другим.

## Типы

### `Numbers`

**Краткое описание:** Хранит свойство как идущие числа и неизменную форму.

**Параметры:**
- `Values` (`std::vector<double>`): то, что идёт.
- `Form` (`std::vector<std::uint32_t>`): то, что не идёт (счётчики, виды заливок, флаги).

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `NumbersOf`.

## Функции

### `SayAsShown`

**Краткое описание:** Записывает свойство так, как элемент его показывает — значение роли, если не сказано.

**Параметры:**
- `Property` (`std::uint16_t`), `Kind` (`std::uint8_t`), `Role` (`std::uint16_t`), `Look` (`const Theme &`), `Said` (`Properties &`, in-out).

**Возвращает:** (`bool`) `false` для свойства без промежуточных чисел и для размера по содержимому (его знает только дисплей, раскладывая).

**Побочные эффекты:** Меняет `Said`.

**Как работает:** Нужна, чтобы анимация шла от того, что видно, к тому, что будет видно.

**Пример:**
```cpp
#include "painter/items/between.h"

bool Start(painter::items::Properties &Shown, const painter::items::Theme &Look)
{
    return painter::items::SayAsShown(ITEM_PROP_FILLS, ITEM_SHAPE, ITEM_ROLE_PANEL, Look, Shown);
}
```

### `MakeAlike`

**Краткое описание:** Приводит два значения свойства к одной форме, где это возможно.

**Параметры:**
- `Property` (`std::uint16_t`), `From`, `To` (`Properties &`, in-out).

**Возвращает:** `void`.

**Побочные эффекты:** Меняет оба набора.

**Как работает:** Теней становится поровну: лишние появляются или исчезают из «ничего» (прозрачные, без расширения); обводка появляется или исчезает с нулевой толщины.

**Пример:**
```cpp
#include "painter/items/between.h"

void Prepare(painter::items::Properties &From, painter::items::Properties &To) { painter::items::MakeAlike(ITEM_PROP_SHADOWS, From, To); }
```

### `NumbersOf` / `SayNumbers`

**Краткое описание:** Переводят свойство в числа и форму и обратно.

**Параметры:**
- `NumbersOf`: `Property`, `Of` (`const Properties &`), `Look` (`const Theme &`), `Into` (`Numbers &`, out).
- `SayNumbers`: `Property`, `Values` (`std::span<const double>` — столько, сколько чисел у `Like`), `Like` (`const Properties &` — чья форма), `Look`, `Into` (`Properties &`, out).

**Возвращает:** `NumbersOf` — (`bool`) `false` для свойства без чисел или несказанного; `SayNumbers` — `void`.

**Побочные эффекты:** Записывают выходные параметры. Цвет темы, прошедший путь, записывается как обычный цвет.

**Пример:**
```cpp
#include <vector>

#include "painter/items/between.h"

bool Midway(const painter::items::Properties &A, const painter::items::Properties &B, const painter::items::Theme &Look,
            painter::items::Properties &Into)
{
    namespace items = painter::items;
    items::Numbers From, To;
    if (!items::NumbersOf(ITEM_PROP_PLACE, A, Look, From) || !items::NumbersOf(ITEM_PROP_PLACE, B, Look, To)) return false;
    if (From.Form != To.Form || From.Values.size() != To.Values.size()) return false;
    std::vector<double> Half(From.Values.size());
    for (std::size_t I = 0; I < Half.size(); I++) Half[I] = (From.Values[I] + To.Values[I]) / 2;
    items::SayNumbers(ITEM_PROP_PLACE, Half, A, Look, Into);
    return true;
}
```
