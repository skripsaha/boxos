# painter::core

## Назначение

`painter::core` — общие типы Painter и движок рисования пикселей. Им
рисуют все: дисплей компонует на нём экран, `painter::gui` рисует им
элементы, программа рисует им на своём холсте (`painter::gui::Canvas`).

## Модули

Общие типы:

- [geometry](geometry.md) — `Point`, `PointF`, `Size`, `Rect`, `RectF`.
- [color](color.md) — `Color`, палитра `Colors`, OKLab.
- [coverage](coverage.md) — доля покрытия пикселя по расстоянию до края.

Движок:

- [canvas](canvas.md) — `Canvas`: холст и все операции рисования.
- [pixel](pixel.md) — `Pixel`: пиксель холста, свет в 16 битах.
- [path](path.md) — `Path`, `Corners`: контуры, готовые фигуры, данные пути SVG.
- [stroke](stroke.md) — `Stroke`, `LineCap`, `LineJoin`, `StrokeAlign`: обводка.
- [transform](transform.md) — `Transform`: перенос, масштаб, поворот, наклон.
- [fill](fill.md) — `Fill`, `FillRule`: чем заливать — цвет, градиент, картинка.
- [gradient](gradient.md) — `Gradient`, `GradientStop`, `Extend`.
- [picture](picture.md) — `Picture`: картинка для заливки с уменьшенными копиями.
- [mask](mask.md) — `Mask`: покрытие, сделанное один раз и наложенное многократно.
- [shadow](shadow.md) — `Shadow`: тень любой фигуры, как `box-shadow`.
- [text](text.md) — `TextPath`: текст Typecase как контур.
- [easing](easing.md) — `Easing`, `Spring`: как изменение идёт во времени.
- [system_fonts](system_fonts.md) — шрифты системы и чтение файла шрифта.
- [cpu_path](cpu_path.md) — выбор пути обработки строк: обычный, SSE2, AVX2.

## Как устроен движок

**Свет в 16 битах.** Пиксель холста — четыре 16-битных числа: красный,
зелёный, синий свет, уже умноженные на непрозрачность, и сама
непрозрачность. Цвет кладётся на цвет как свет — единственный способ
смешивания в системе (`boxos_color.h`). Шестнадцать бит дают запас: широкий
тёмный градиент не распадается на полосы.

**Края как у кисти.** Каждая граница покрывается так же, как в загрузчике
и у глифов Typecase (`boxos_shape.h`, `boxos_stroke.h`, `boxos_paint.h`):
один текст правил для всех, кто рисует.

**Отказ целиком.** Рисование, которое нельзя выполнить (фигура дальше
`BOX_SHAPE_REACH` = 32768 пикселей, преобразование, растягивающее слишком
сильно, градиент без остановок), отвергается целиком и сообщает об этом
`false`; ничего из него не рисуется.

**Три пути, одни биты.** Строки пикселей обрабатываются обычным кодом, SSE2
или AVX2 — самым быстрым, что есть у процессора; результат одинаков до
бита (`tools/draw_stand.sh` проверяет это на хосте, `drawcheck` — на
машине).

**Потоки.** На одном холсте рисует один поток за раз; на двух разных —
два одновременно.

## Пример

```cpp
#include "painter/core/canvas.h"

bool Badge(painter::core::Canvas &Out)
{
    namespace core = painter::core;
    const core::Path Card = core::Path::RoundedRect({20, 20, 200, 80}, core::Corners::All(16));
    const core::Shadow Soft{{0, 6}, 16, 0, core::Color::Rgba(0, 0, 0, 90), false};
    if (auto Kept = Soft.MaskFor(Card)) Out.DrawShadow(*Kept, Soft);
    if (!Out.Fill(Card, core::Colors::Ocean)) return false;
    core::Stroke Line;
    Line.Width = 2;
    Line.Align = core::StrokeAlign::Inside;
    return Out.Stroke(Card, Line, core::Colors::White);
}
```
