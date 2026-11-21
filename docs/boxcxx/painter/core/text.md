# painter::core::text

## Назначение

Заголовок: [`painter/core/text.h`](../../../../src/userspace/painter/include/painter/core/text.h).
Реализация: [`src/core/text_draw.cpp`](../../../../src/userspace/painter/src/core/text_draw.cpp).

Текст, разложенный Typecase (`typecase/layout.h`), рисуется на холсте
двумя путями: заливкой (`Canvas::FillText`, см. [canvas](canvas.md)) или
как фигура — контуры глифов и линии подчёркивания и зачёркивания, — чтобы
обвести его, отбросить тень или обрезать им что-то: всё, чем рисуется
`Path`.

## Функции

### `TextPath`

**Краткое описание:** Возвращает контур текста.

**Параметры:**
- `Text` (`const typecase::Layout &`): разложенный текст.
- `At` (`PointF`): где левый верхний угол раскладки.
- `Ink` (`std::optional<std::uint32_t>`): только часть, нарисованная этими чернилами; пусто — весь текст.

**Возвращает:** (`Path`) контуры глифов такими, как они рисуются (`typecase/glyph_look.h`), и линии; символ, которого нет ни в одном шрифте, — рамка его квадрата.

**Побочные эффекты:** Выделяет память; может бросить `std::bad_alloc`.

**Типичное применение:** Обведённый текст, тень от букв, текст-окно, через которое видна картинка.

**Пример:**
```cpp
#include "painter/core/canvas.h"
#include "painter/core/system_fonts.h"
#include "painter/core/text.h"
#include "typecase/text.h"

bool Outlined(painter::core::Canvas &Out, std::string_view Words)
{
    namespace core = painter::core;
    const auto Fonts = core::SystemFonts();
    if (!Fonts) return false;
    typecase::Style Look;
    Look.Size   = 64;
    Look.Weight = 800;
    const typecase::Layout Laid = typecase::Layout::Of(typecase::Text::Of(Words, Look), typecase::TextBox{}, *Fonts);
    const core::Path Letters = core::TextPath(Laid, {10, 10});
    core::Stroke Line;
    Line.Width = 2;
    Line.Join  = core::LineJoin::Round;
    return Out.Fill(Letters, core::Colors::Amber) && Out.Stroke(Letters, Line, core::Colors::Black);
}
```
