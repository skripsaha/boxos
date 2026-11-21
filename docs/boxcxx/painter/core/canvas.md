# painter::core::canvas

## Назначение

Заголовок: [`painter/core/canvas.h`](../../../../src/userspace/painter/include/painter/core/canvas.h).
Реализация: [`src/core/canvas.cpp`](../../../../src/userspace/painter/src/core/canvas.cpp),
закраска строк — `paint.cpp`, `lanes_*.cpp`; текст — `text_draw.cpp`; размытие — `blur.cpp`.

`Canvas` — пиксели в памяти и всё, чем на них рисуют: фигуры, залитые и
обведённые цветами, градиентами и картинками; маски; тени; другие холсты;
текст. Каждый край покрывается так, как его покрывает кисть
(`boxos_shape.h`), каждый цвет кладётся как свет в 16 битах (`Pixel`).
Дисплей компонует на холстах экран, программа рисует на холсте свою
картинку (страница `painter::gui::Canvas` — это `core::Canvas`).

Общие правила:

- каждое рисование ограничено текущей обрезкой (`PushClip`), кроме `Clear` и `FadeTo`;
- всё нарисованное копится в прямоугольник изменений (`Changed`);
- рисование, которое нельзя выполнить (фигура дальше `BOX_SHAPE_REACH` = 32768 пикселей, преобразование растягивает слишком сильно, градиент без остановок), отвергается целиком: возвращается `false`, ничего не нарисовано;
- на одном холсте одновременно рисует один поток.

## Типы

### `Dither`

**Краткое описание:** Выбирает, как свет холста переводится в 8-битные числа стекла.

**Параметры:**
- `Off`: ближайшее число.
- `On`: неподвижный узор порогов (синий шум 64×64) между двумя соседними числами.

**Возвращает:** Неприменимо: перечисление `bool`.

**Побочные эффекты:** Нет.

**Как работает:** Узор стоит на месте: пиксель, который не меняется, всегда записывается одинаково, поэтому перерисовывается только изменившееся. Плавные тёмные градиенты не дают ступенек.

**Пример:** См. `Canvas::CopyOut`.

### `Foreign`

**Краткое описание:** Описывает пиксели, которые рисует другая рука, в памяти, которую та может писать во время чтения.

**Параметры:**
- `Pixels` (`const Pixel *`), `Width`, `Height` (`std::uint32_t`), `Stride` (`std::size_t`, пикселей от строки до строки).

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Как работает:** При чтении свет каждого пикселя ограничивается его непрозрачностью, как у пикселя холста: чужие ошибки не портят ничего, кроме собственной картинки.

**Пример:** См. `Canvas::Draw`.

## Создание

### `Canvas::Make`

**Краткое описание:** Создаёт холст в собственной памяти, все пиксели прозрачны.

**Параметры:**
- `Width`, `Height` (`std::uint32_t`): размер.

**Возвращает:** (`std::optional<Canvas>`) холст; пусто, если сторона 0, больше `BOX_SHAPE_REACH` или памяти нет.

**Побочные эффекты:** Выделяет `Width × Height × 8` байт.

**Примечания:** Копировать нельзя, перемещать можно.

**Пример:**
```cpp
#include "painter/core/canvas.h"

std::optional<painter::core::Canvas> Scratch() { return painter::core::Canvas::Make(256, 256); }
```

### `Canvas::Over`

**Краткое описание:** Создаёт холст поверх чужой памяти пикселей.

**Параметры:**
- `Memory` (`Pixel *`): пиксели.
- `Width`, `Height` (`std::uint32_t`).
- `Stride` (`std::size_t`): пикселей от строки до строки, не меньше `Width`.

**Возвращает:** (`std::optional<Canvas>`) холст или пусто при неверных параметрах.

**Побочные эффекты:** Нет; память холсту не принадлежит и должна жить дольше него.

**Типичное применение:** Страница холста, разделённая с дисплеем (так её отдаёт `CanvasBlock::Page`).

**Пример:**
```cpp
#include <vector>

#include "painter/core/canvas.h"

std::optional<painter::core::Canvas> OnTop(std::vector<painter::core::Pixel> &Store)
{
    Store.assign(64 * 32, painter::core::Pixel{});
    return painter::core::Canvas::Over(Store.data(), 64, 32, 64);
}
```

### `Width` / `Height` / `Row`

**Краткое описание:** Возвращают размер холста и указатель на строку пикселей.

**Параметры:**
- `Y` (`std::uint32_t`): строка — для `Row`.

**Возвращает:** `Width`, `Height` — (`std::uint32_t`); `Row` — (`Pixel *` или `const Pixel *`).

**Побочные эффекты:** Нет.

**Примечания:** `Row` без проверки границ. Прямая запись в пиксели не попадает в `Changed`; соблюдайте инвариант «свет не больше непрозрачности».

**Пример:**
```cpp
#include "painter/core/canvas.h"

void Noise(painter::core::Canvas &Out, std::uint32_t Seed)
{
    for (std::uint32_t Y = 0; Y < Out.Height(); Y++) {
        painter::core::Pixel *Line = Out.Row(Y);
        for (std::uint32_t X = 0; X < Out.Width(); X++) {
            Seed = Seed * 1664525u + 1013904223u;
            const auto Light = static_cast<std::uint16_t>(Seed >> 16);
            Line[X] = {Light, Light, Light, 0xFFFF};
        }
    }
}
```

## Заливка и обводка

### `Canvas::Clear`

**Краткое описание:** Делает холст или его часть одного цвета.

**Параметры:**
- `To` (`Color`): цвет.
- `Area` (`Rect`): часть холста — во второй перегрузке.

**Возвращает:** `void`.

**Побочные эффекты:** Пиксели заменяются (а не накладываются), обрезка не действует; область попадает в `Changed`.

**Пример:**
```cpp
#include "painter/core/canvas.h"

void Begin(painter::core::Canvas &Out) { Out.Clear(painter::core::Color::Through()); }
```

### `Canvas::Fill`

**Краткое описание:** Заливает фигуру одной или несколькими заливками.

**Параметры:**
- `Shape` (`const Path &`): фигура.
- `Fills` (`std::span<const core::Fill>`) или `With` (`const core::Fill &`): заливки, одна поверх другой; цвет и градиент передаются напрямую (неявное преобразование в `Fill`).
- `Place` (`const Transform &`): куда положить фигуру, по умолчанию тождественно.
- `Rule` (`FillRule`): по умолчанию `NonZero`.
- `Opacity` (`float`): общая непрозрачность 0–1, по умолчанию 1.

**Возвращает:** (`bool`) `false` — отвергнуто, ничего не нарисовано.

**Побочные эффекты:** Рисует; пополняет `Changed`.

**Как работает:** Покрытие фигуры строится один раз и закрашивается всеми заливками по порядку. Градиенты и картинки заливки живут в пространстве контура и следуют `Place`.

**Пример:**
```cpp
#include "painter/core/canvas.h"

bool Button(painter::core::Canvas &Out)
{
    namespace core = painter::core;
    const core::Path Plate = core::Path::RoundedRect({10, 10, 140, 40}, core::Corners::All(10));
    const core::Fill Layers[] = {core::Colors::Ocean,
                                 core::Gradient::Linear({0, 10}, {0, 50}, {{0.0f, core::Color::Rgba(255, 255, 255, 60)},
                                                                           {1.0f, core::Color::Through()}})};
    return Out.Fill(Plate, Layers);
}
```

### `Canvas::Stroke`

**Краткое описание:** Проводит линию вдоль фигуры.

**Параметры:**
- `Shape` (`const Path &`).
- `Line` (`const core::Stroke &`): толщина, концы, углы, пунктир, положение (см. [stroke](stroke.md)).
- `Fills` / `With`, `Place`, `Opacity`: как у `Fill`.

**Возвращает:** (`bool`) `false` — отвергнуто.

**Побочные эффекты:** Рисует; пополняет `Changed`.

**Как работает:** Для `StrokeAlign::Inside` и `Outside` линия строится вдвое толще, и половина по другую сторону края срезается покрытием самой фигуры.

**Пример:** См. [stroke](stroke.md).

### `Canvas::FillText`

**Краткое описание:** Заливает текст, разложенный Typecase.

**Параметры:**
- `Text` (`const typecase::Layout &`): разложенный текст.
- `At` (`PointF`): где левый верхний угол раскладки.
- `Fills` / `With`: заливки.
- `Place` (`const Transform &`), `Opacity` (`float`).
- `Ink` (`std::optional<std::uint32_t>`): только часть текста, нарисованная этими чернилами (номер цвета отрезков раскладки); пусто — весь текст.

**Возвращает:** (`bool`) `false` — отвергнуто.

**Побочные эффекты:** Рисует; кэширует картинки глифов (один кэш на все холсты программы).

**Как работает:** Если `Place` ставит текст прямо и в одном размере, не больше 256 пикселей на кегль, каждый глиф рисуется один раз в своём размере и с точностью до четверти пикселя по горизонтали и запоминается; соприкасающиеся глифы и строка с подчёркиванием или зачёркиванием сначала собираются в одно покрытие, чтобы общее накладывалось один раз. Иначе (поворот, наклон, крупнее 256) текст заливается как контур (`TextPath`). Текст, обрезанный своей рамкой, остаётся в рамке.

**Типичное применение:** Подписи на собственном холсте программы. На элементах `painter::gui` текст рисует дисплей.

**Пример:**
```cpp
#include "painter/core/canvas.h"
#include "painter/core/system_fonts.h"
#include "typecase/layout.h"
#include "typecase/text.h"

bool Caption(painter::core::Canvas &Out, std::string_view Words)
{
    const auto Fonts = painter::core::SystemFonts();
    if (!Fonts) return false;
    typecase::Style Look;
    Look.Size = 18;   // пикселей на кегль
    const typecase::Text Said = typecase::Text::Of(Words, Look);
    typecase::TextBox Box;
    Box.Width = 300;
    const typecase::Layout Laid = typecase::Layout::Of(Said, Box, *Fonts);
    return Out.FillText(Laid, {12, 12}, painter::core::Colors::White);
}
```

### `Canvas::FillMask`

**Краткое описание:** Заливает покрытие маски там, где маска лежит.

**Параметры:**
- `Cover` (`const Mask &`).
- `Fills` / `With`, `Opacity`.

**Возвращает:** (`bool`) `false` — заливка не может быть нарисована.

**Побочные эффекты:** Рисует.

**Пример:** См. [mask](mask.md).

### `Canvas::DrawShadow`

**Краткое описание:** Накладывает сохранённую маску тени цветом тени.

**Параметры:**
- `Kept` (`const Mask &`): результат `Shadow::MaskFor`.
- `Cast` (`const Shadow &`): от неё берётся цвет.
- `Opacity` (`float`), по умолчанию 1.
- `By` (`Point`): сдвиг на целые пиксели от места, где маска сделана.

**Возвращает:** `void`.

**Побочные эффекты:** Рисует.

**Пример:** См. [shadow](shadow.md).

## Холсты и пиксели

### `Canvas::Draw`

**Краткое описание:** Накладывает на этот холст другой холст или чужие пиксели.

**Параметры:**
- `Picture` (`const Canvas &` или `const Foreign &`).
- `At` (`Point`): где левый верхний угол.
- `Opacity` (`float`), по умолчанию 1.

**Возвращает:** `void`.

**Побочные эффекты:** Рисует в пределах обрезки.

**Как работает:** Пиксели накладываются как свет; при мягкой обрезке доля каждого пикселя умножается на её покрытие. Для `Foreign` свет каждого пикселя сначала ограничивается его непрозрачностью.

**Пример:**
```cpp
#include "painter/core/canvas.h"

void Sprite(painter::core::Canvas &Out, const painter::core::Canvas &Ship, painter::core::Point At) { Out.Draw(Ship, At); }
```

### `Canvas::FadeTo`

**Краткое описание:** Сдвигает каждый пиксель на долю пути к пикселю другого холста.

**Параметры:**
- `Toward` (`const Canvas &`): холст того же размера (используется общая часть).
- `Share` (`float`): 0–1.

**Возвращает:** `void`.

**Побочные эффекты:** Меняет пиксели, не учитывая обрезку.

**Как работает:** Свет и непрозрачность идут прямо, поэтому каждая картинка учитывается настолько, насколько видна.

**Типичное применение:** Плавный переход одной картинки в другую.

**Пример:**
```cpp
#include "painter/core/canvas.h"

void Crossfade(painter::core::Canvas &Shown, const painter::core::Canvas &Next, float Time) { Shown.FadeTo(Next, Time); }
```

### `Canvas::Blur`

**Краткое описание:** Размывает часть холста на месте, как фон за матовым стеклом.

**Параметры:**
- `Area` (`Rect`).
- `Radius` (`float`): гауссово размытие с σ = `Radius / 2`.

**Возвращает:** `void`.

**Побочные эффекты:** Меняет пиксели в пределах обрезки; край области удерживается.

**Примечания:** В мягкой обрезке размытие видно настолько, насколько её покрытие.

**Пример:**
```cpp
#include "painter/core/canvas.h"

void Frost(painter::core::Canvas &Out) { Out.Blur({40, 40, 200, 120}, 12); }
```

## Обрезка и изменения

### `Canvas::PushClip` / `Canvas::PopClip` / `Canvas::Clip`

**Краткое описание:** Сужают обрезку прямоугольником или фигурой, возвращают прежнюю и сообщают текущую.

**Параметры:**
- `Area` (`Rect`): прямоугольник.
- `Shape` (`const Path &`), `Place` (`const Transform &`), `Rule` (`FillRule`): фигура с краями, мягкими как у заливки.

**Возвращает:** `PushClip(Rect)` — `void`; `PushClip(Path)` — (`bool`) `false`, если фигура отвергнута (обрезка не меняется); `PopClip` — `void`; `Clip` — (`Rect`) прямоугольник текущей обрезки.

**Побочные эффекты:** Меняют стек обрезок. Обрезки вкладываются: новая пересекается с прежней.

**Примечания:** `PopClip` никогда не снимает нижний уровень (весь холст).

**Пример:**
```cpp
#include "painter/core/canvas.h"

void Porthole(painter::core::Canvas &Out, const painter::core::Canvas &View)
{
    namespace core = painter::core;
    if (!Out.PushClip(core::Path::Circle({100, 100}, 80))) return;
    Out.Draw(View, {20, 20});
    Out.PopClip();
}
```

### `Canvas::Changed` / `Canvas::TakeChanged`

**Краткое описание:** Возвращают наименьший прямоугольник со всем, что нарисовано с прошлого взятия, и забирают его.

**Параметры:** Нет.

**Возвращает:** (`Rect`); `TakeChanged` обнуляет накопленное.

**Побочные эффекты:** `TakeChanged` — сброс.

**Типичное применение:** Передать в `gui::Canvas::Draw` как изменившуюся часть или показать только её.

**Пример:**
```cpp
#include "painter/core/canvas.h"

painter::core::Rect Dirty(painter::core::Canvas &Out) { return Out.TakeChanged(); }
```

## Обмен с экраном

### `Canvas::CopyOut`

**Краткое описание:** Записывает часть холста на стекло поверх чёрного.

**Параметры:**
- `Area` (`Rect`): часть холста.
- `Glass` (`std::uint32_t *`): пиксель стекла под углом холста.
- `Stride` (`std::size_t`): пикселей стекла от строки до строки.
- `Format` (`std::uint32_t`): раскладка байтов — `BOX_PIXEL_RGB`, `BOX_PIXEL_BGR`, `BOX_PIXEL_BGRX` (`boxos_pixel.h`).
- `Mode` (`core::Dither`).
- `Corner` (`Point`): где угол холста лежит на стекле — чтобы узор шума был узором стекла и картинка, выводимая кусками, давала то же, что целиком. По умолчанию (0, 0).

**Возвращает:** `void`.

**Побочные эффекты:** Пишет стекло, не читая его.

**Примечания:** Нужен тем, кто сам владеет framebuffer (дисплей, загрузчик); обычной программе — нет.

**Пример:**
```cpp
#include "painter/core/canvas.h"
#include "boxos_pixel.h"   // после <cstdint>: заголовок ждёт типы от того, кто его подключает

void Show(const painter::core::Canvas &Out, std::uint32_t *Frame, std::size_t Pitch)
{
    Out.CopyOut({0, 0, static_cast<std::int32_t>(Out.Width()), static_cast<std::int32_t>(Out.Height())}, Frame, Pitch,
                BOX_PIXEL_BGRX, painter::core::Dither::On);
}
```

### `Canvas::CopyIn` / `Canvas::TakePixels`

**Краткое описание:** Берут в часть холста непрозрачные пиксели 0x00RRGGBB или пиксели движка, нарисованные другой рукой.

**Параметры:**
- `Area` (`Rect`).
- `From` (`const std::uint32_t *` для `CopyIn`, `const Pixel *` для `TakePixels`): пиксель под углом холста; строка Y источника соответствует строке Y холста.
- `Stride` (`std::size_t`): пикселей от строки до строки.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяют пиксели (не накладывают); `TakePixels` ограничивает свет непрозрачностью.

**Пример:**
```cpp
#include "painter/core/canvas.h"

void Restore(painter::core::Canvas &Out, const std::uint32_t *Saved)
{
    Out.CopyIn({0, 0, static_cast<std::int32_t>(Out.Width()), static_cast<std::int32_t>(Out.Height())}, Saved, Out.Width());
}
```
