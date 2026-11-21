# painter::core::picture

## Назначение

Заголовок: [`painter/core/picture.h`](../../../../src/userspace/painter/include/painter/core/picture.h).
Реализация: [`src/core/picture.cpp`](../../../../src/userspace/painter/src/core/picture.cpp).

Картинка для заливки фигур на холсте. Хранит пиксели так же, как холст
(`Pixel`), и уменьшенные копии — каждая вдвое меньше предыдущей, до одного
пикселя. Картинка, нарисованная намного меньше своего размера, берётся из
ближайшей по размеру копии, смешанной как свет, и не мерцает. Делается
один раз и разделяется всеми заливками, которые её показывают
(`std::shared_ptr<const Picture>`).

Это картинка движка в памяти программы. Картинка, которую показывает
дисплей как элемент, лежит в блоке (`painter::gui::PictureBlock`).

## Picture

### `Picture::FromRgba`

**Краткое описание:** Создаёт картинку из байтов R, G, B, A.

**Параметры:**
- `Bytes` (`std::span<const std::uint8_t>`): пиксели — числа sRGB, непрозрачность не умножена (как отдаёт декодер PNG или JPEG).
- `Width`, `Height` (`std::uint32_t`): размер.
- `Stride` (`std::size_t`): байт от строки до строки.

**Возвращает:** (`std::shared_ptr<const Picture>`) картинка или `nullptr`: размер нулевой, больше `BOX_SHAPE_REACH`, или байтов мало.

**Побочные эффекты:** Выделяет память под картинку и все уменьшенные копии (около 4/3 исходного размера).

**Пример:**
```cpp
#include <array>

#include "painter/core/picture.h"

std::shared_ptr<const painter::core::Picture> TwoByTwo()
{
    static constexpr std::array<std::uint8_t, 16> Rgba{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 128};
    return painter::core::Picture::FromRgba(Rgba, 2, 2, 8);
}
```

### `Picture::FromPixels`

**Краткое описание:** Создаёт картинку из пикселей движка.

**Параметры:**
- `Pixels` (`std::span<const Pixel>`).
- `Width`, `Height` (`std::uint32_t`).
- `Stride` (`std::size_t`): пикселей от строки до строки.

**Возвращает:** (`std::shared_ptr<const Picture>`) или `nullptr`.

**Побочные эффекты:** Как у `FromRgba`.

**Типичное применение:** Превратить нарисованный холст в картинку для заливки.

**Пример:**
```cpp
#include "painter/core/canvas.h"
#include "painter/core/picture.h"

std::shared_ptr<const painter::core::Picture> Snapshot(const painter::core::Canvas &Out)
{
    const std::size_t Stride = Out.Height() > 1 ? static_cast<std::size_t>(Out.Row(1) - Out.Row(0)) : Out.Width();
    return painter::core::Picture::FromPixels(std::span(Out.Row(0), Stride * (Out.Height() - 1) + Out.Width()),
                                              Out.Width(), Out.Height(), Stride);
}
```

### `Width` / `Height` / `Sizes`

**Краткое описание:** Возвращают размер картинки и все её размеры.

**Параметры:** Нет.

**Возвращает:** `Width`, `Height` — (`std::uint32_t`); `Sizes` — (`std::span<const Picture::Level>`), где уровень 0 — сама картинка, каждый следующий вдвое меньше; `Level` содержит `Width`, `Height` и `Pixels`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/picture.h"

std::size_t Levels(const painter::core::Picture &Of) { return Of.Sizes().size(); }
```
