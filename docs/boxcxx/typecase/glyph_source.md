# typecase::glyph_source

## Назначение

Заголовок: [`typecase/glyph_source.h`](../../../src/userspace/typecase/include/typecase/glyph_source.h).

То, что рисует символы в ячейке: шрифт, оттиск, геометрия ячейки,
собственные символы BoxOS. Источник говорит, какие символы у него есть, и
рисует их; для остальных — и для своего, который он не может нарисовать —
ничего не рисует, и цепочка ([chain](chain.md)) спрашивает следующий.

Готовые источники: [font_source](font_source.md),
[geometry_source](geometry_source.md), [symbol_source](symbol_source.md),
[imprint_source](imprint_source.md). Можно написать свой — например,
значки программы.

## Класс GlyphSource

### `GlyphSource::Has`

**Краткое описание:** Сообщает, есть ли у источника символ.

**Параметры:**
- `Character` (`char32_t`).

**Возвращает:** (`bool`).

**Побочные эффекты:** Нет.

**Пример:** См. `GlyphSource::Draw`.

### `GlyphSource::Draw`

**Краткое описание:** Рисует символ в ячейке заданного размера.

**Параметры:**
- `Character` (`char32_t`).
- `At` (`const Size &`): ячейка и кегль.

**Возвращает:** (`std::optional<GlyphImage>`) картинка, или пусто — тогда цепочка спросит следующий источник.

**Побочные эффекты:** Зависят от источника (обычно — память растеризатора).

**Пример:**
```cpp
#include <vector>

#include "typecase/glyph_source.h"

// Свой источник: символ U+E000 — закрашенная ячейка.
class Solid final : public typecase::GlyphSource {
public:
    bool Has(char32_t Character) const override { return Character == 0xE000; }
    std::optional<typecase::GlyphImage> Draw(char32_t Character, const typecase::Size &At) override
    {
        if (!Has(Character)) return std::nullopt;
        typecase::GlyphImage Made;
        Made.Top     = -At.Baseline;
        Made.Width   = At.Width;
        Made.Height  = At.Height;
        Made.Advance = static_cast<float>(At.Width);
        Made.Coverage.assign(static_cast<std::size_t>(At.Width) * static_cast<std::size_t>(At.Height), 255);
        return Made;
    }
};
```
