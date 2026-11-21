# typecase::symbol_source

## Назначение

Заголовок: [`typecase/symbol_source.h`](../../../src/userspace/typecase/include/typecase/symbol_source.h).
Реализация: [`src/symbol_source.cpp`](../../../src/userspace/typecase/src/symbol_source.cpp).

Собственные символы BoxOS (`boxos_symbols.h`), вычисленные по ячейке, как
геометрия: Duppo шириной в две ячейки — его одноцветный знак
(`boxos_mascot.h`), нарисованный через пару ячеек так крупно, как пара
вмещает, и разрезанный надвое: левая половина — один символ, правая —
другой. В малом размере линия вокруг глаза была бы тоньше пикселя, а глаз —
пятном: тогда знак рисуется без неё, только зрачок и блик; поле вокруг
среднего пузыря и блик никогда не тоньше пикселя.

Источник, который спрашивают до геометрии: он не рисует ничего из того, что
рисует геометрия.

## Класс SymbolSource

### `Has` / `Draw`

**Краткое описание:** Сообщают, свой ли это символ BoxOS, и рисуют его.

**Параметры:**
- `Character` (`char32_t`), `At` (`const Size &`).

**Возвращает:** `Has` — `bool`; `Draw` — `std::optional<GlyphImage>`.

**Побочные эффекты:** `Draw` использует собственный растеризатор.

**Пример:**
```cpp
#include <memory>

#include "typecase/chain.h"
#include "typecase/symbol_source.h"

void First(typecase::Chain &Sources) { Sources.Add(std::make_unique<typecase::SymbolSource>()); }
```
