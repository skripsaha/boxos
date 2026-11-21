# painter::items::properties (внутренний)

## Назначение

Заголовок: [`painter/items/properties.h`](../../../../src/userspace/painter/include/painter/items/properties.h).
Реализация: [`src/items/properties.cpp`](../../../../src/userspace/painter/src/items/properties.cpp).

Язык элементов, прочитанный: значение свойства так, как его записала
программа, приведённое к своим пределам и хранимое так, как хранит дисплей.
Ничему в байтах не доверяется: каждое число, длина и счётчик проверяются
до использования; значение, нарушающее язык, отвергается целиком с
причиной, а свойство остаётся прежним.

Единицы — как на проводе: места и размеры в 1/256 точки (`ITEM_POINT`),
цвета `ItemColor` 16 бит на канал, непрозрачность до `ITEM_OPAQUE`.

## Типы

### `Fill`, `Stroke`, `Shadow`, `TextStyle`, `Span`, `Shape`, `PictureOf`

**Краткое описание:** Хранят прочитанные значения свойств в единицах языка.

**Параметры:**
- `Fill`: `Kind` (`ITEM_FILL_*`), `Extend`, `Opacity`, `Color`, `Theme`, `From`, `To`, `Radius`, `Focus`, `Start` (доли `ITEM_TURN`), `Stops`, `Block`, `At`, `Size`, `Sharp`.
- `Stroke`: `Width`, `Cap`, `Join`, `Align`, `MiterLimit`, `DashOffset`, `Dashes`, `With`.
- `Shadow`: `Offset`, `Blur`, `Spread`, `Color`, `Inner`.
- `TextStyle`: `Said` (биты `ITEM_SAYS_*` — что сказано), `Family`, `Size`, `Weight`, `Italic`, `Spacing`, `LineHeight` (доли `ITEM_ONE` кегля), `Underline`, `Strike`, `Color`.
- `Span`: `From`, `To`, `Style`.
- `Shape`: `Form` (`ITEM_FORM_*`), `Rule`, `Steps`, `Points`.
- `PictureOf`: `Block`, `Fit`, `Sharp`.

**Возвращает:** Неприменимо: агрегаты.

**Побочные эффекты:** Нет.

**Пример:** См. `Decode`.

### `Properties`

**Краткое описание:** Хранит все свойства элемента; каждое не сказано, пока его не скажут.

**Параметры:** Поля `std::optional`: `Place`, `Size`, `Scale`, `Anchor`, `Opacity`, `Hidden`, `Clip`, `Secret`, `Focusable`, `Disabled`, `Turn`, `Corners`, `Shape`, `Fills`, `Stroke`, `Shadows`, `Text`, `Caption`, `Style`, `Spans`, `Box`, `Picture`, `Canvas`, `Grid`, `Caret`.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Примечания:** Несказанное берётся из роли по теме ([look](look.md)).

**Пример:** См. `Decode`.

## Функции

### `Decode`

**Краткое описание:** Читает байты значения свойства в `Properties`.

**Параметры:**
- `Property` (`std::uint16_t`): `ITEM_PROP_*`.
- `Bytes` (`std::span<const std::uint8_t>`): значение как на проводе.
- `Into` (`Properties &`, in-out).
- `Role` (`std::uint16_t &`, out): роль, если свойство — `ITEM_PROP_ROLE` (роль принадлежит элементу, а не виду).

**Возвращает:** (`error_t`) `OK` или причина отказа; при отказе свойство не меняется.

**Побочные эффекты:** Меняет `Into` (или `Role`). `noexcept`.

**Пример:**
```cpp
#include "painter/gui/changes.h"
#include "painter/items/properties.h"

bool ReadBack(const painter::gui::Changes &Said, std::uint32_t Item, painter::items::Properties &Into)
{
    std::uint16_t Role = 0;
    for (std::uint16_t Property : Said.SaidOf(Item))
        if (painter::items::Decode(Property, *Said.Said(Item, Property), Into, Role) != OK) return false;
    return true;
}
```

### `Unsay` / `Assign` / `IsSaid` / `TextBytes`

**Краткое описание:** Делают свойство несказанным, копируют его из другого набора, проверяют, сказано ли оно, и считают байты текста.

**Параметры:**
- `Property` (`std::uint16_t`); `From`, `Into`, `Of` (`Properties`).

**Возвращает:** `Unsay` — (`error_t`) `OK` или причина; `Assign` — `void`; `IsSaid` — (`bool`); `TextBytes` — (`std::size_t`) байты текста, подписи и семейств в отрезках — то, что считается против предела программы `ITEM_TEXTS_MOST`.

**Побочные эффекты:** `Unsay` и `Assign` меняют `Into`.

**Пример:**
```cpp
#include "painter/items/properties.h"

bool HasFills(const painter::items::Properties &Of) { return painter::items::IsSaid(ITEM_PROP_FILLS, Of); }
```
