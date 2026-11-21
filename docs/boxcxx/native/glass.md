# box::glass

## Назначение

Заголовок: [`box/cxx/glass.h`](../../../src/userspace/boxcxx/include/box/cxx/glass.h).
C-основа: `display_effect`, `display_effect_lift` в `box/display.h`.

Эффект поверх всего, что показывает дисплей. Любая программа может
попросить дисплей наложить один эффект на всё стекло на время — на текст,
элементы всех областей, каретку. Дисплей снимает его по истечении времени,
когда попросившая программа закончится, или по `lift()`. Эффект один:
новый занимает место прежнего, кто бы его ни наложил.

## Типы

### `box::glass_effect`

**Краткое описание:** Описывает накладываемый эффект.

**Параметры:**
- `kinds` (`std::uint32_t`): сумма `DISPLAY_EFFECT_*` — `TINT`, `FADE`, `INVERT`, `SWAY`, `MIRROR`, `SHIMMER`, `RIPPLE`, `QUAKE`, `MOSAIC`, `RAINBOW`, `MELT`, `GLITCH`, `SPOTLIGHT`, `SEPIA`, `SNOW`, `PULSE`.
- `strength` (`std::uint8_t`): сила для всех видов, 1–255.
- `axes` (`std::uint8_t`): `DISPLAY_EFFECT_AXIS_X | AXIS_Y` — для покачивания и зеркала.
- `color` (`std::uint32_t`): #RRGGBB для оттенка.
- `duration` (`std::chrono::milliseconds`): сколько длится, не больше `DISPLAY_EFFECT_LONGEST_MS` (5 минут); отрицательное — 0.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `glass::lay`.

## Функции

### `glass::lay`

**Краткое описание:** Накладывает эффект на всё стекло вместо любого другого.

**Параметры:** `effect` (`const glass_effect &`).

**Возвращает:** (`box::status`).

**Побочные эффекты:** Меняет вид всего экрана на время эффекта.

**Пример:**
```cpp
#include <chrono>

#include "box/cxx/glass.h"

void Underwater()
{
    using namespace std::chrono_literals;
    (void)box::glass::lay({DISPLAY_EFFECT_SWAY | DISPLAY_EFFECT_TINT, 160, DISPLAY_EFFECT_AXIS_X | DISPLAY_EFFECT_AXIS_Y, 0x60D8F0, 20s});
}
```

### `glass::lift`

**Краткое описание:** Снимает эффект, если он наложен этой программой (или программа — хранитель).

**Параметры:** Нет.

**Возвращает:** (`box::result<bool>`) `true` — эффект снят, `false` — снимать было нечего.

**Побочные эффекты:** Возвращает обычный вид экрана.

**Пример:**
```cpp
#include "box/cxx/glass.h"

void Calm() { (void)box::glass::lift(); }
```
