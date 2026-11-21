# typecase::chain

## Назначение

Заголовок: [`typecase/chain.h`](../../../src/userspace/typecase/include/typecase/chain.h).
Реализация: [`src/chain.cpp`](../../../src/userspace/typecase/src/chain.cpp).

Источники, которых спрашивают по очереди: отвечает первый, кто рисует
символ, а символ, которого не рисует никто, рисуется своим кодом
([code_box](code_box.md)), — цепочка отвечает всегда. Источники и их
порядок выбирает тот, кто строит цепочку. Экран из ячеек ставит первыми
источники, рисующие по ячейке, чтобы линия рамки точно встречала линию
соседней ячейки, а шрифты — после них; сплошному тексту нужны только
шрифты (их ведёт [fonts](fonts.md)).

Наборщик ячеек дисплея строит цепочку так: символы BoxOS → геометрия →
шрифт из настройки `text font` → системный оттиск.

## Класс Chain

### `Chain`

**Краткое описание:** Создаёт пустую цепочку с уникальным номером.

**Параметры:** Нет.

**Возвращает:** Неприменимо: конструктор.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include <memory>

#include "typecase/chain.h"
#include "typecase/font_source.h"
#include "typecase/geometry_source.h"
#include "typecase/symbol_source.h"

typecase::Chain ForCells(typecase::Font Mono)
{
    typecase::Chain Sources;
    Sources.Add(std::make_unique<typecase::SymbolSource>());
    Sources.Add(std::make_unique<typecase::GeometrySource>());
    Sources.Add(std::make_unique<typecase::FontSource>(std::move(Mono)));
    return Sources;
}
```

### `Chain::Add`

**Краткое описание:** Добавляет источник в конец очереди.

**Параметры:**
- `Source` (`std::unique_ptr<GlyphSource>`).

**Возвращает:** `void`.

**Побочные эффекты:** Цепочка владеет источником.

**Пример:** См. `Chain`.

### `Has` / `Draw` / `Serial`

**Краткое описание:** Сообщают, показывается ли символ сам собой, рисуют его и возвращают номер цепочки.

**Параметры:**
- `Character` (`char32_t`), `At` (`const Size &`).

**Возвращает:** `Has` — (`bool`) у какого-то источника он есть; `Draw` — (`GlyphImage`) картинка первого ответившего или рамка с кодом; `Serial` — (`std::uint64_t`) номер, по которому кэш узнаёт нарисованное ею.

**Побочные эффекты:** `Draw` — как у источников.

**Пример:**
```cpp
#include "typecase/chain.h"

typecase::GlyphImage Cell(typecase::Chain &Sources, char32_t Character)
{
    return Sources.Draw(Character, typecase::Size{16, 8, 16, 12});
}
```
