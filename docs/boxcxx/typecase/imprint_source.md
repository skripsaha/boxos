# typecase::imprint_source

## Назначение

Заголовок: [`typecase/imprint_source.h`](../../../src/userspace/typecase/include/typecase/imprint_source.h).
Реализация: [`src/imprint_source.cpp`](../../../src/userspace/typecase/src/imprint_source.cpp).

Оттиск (`boxos_imprint.h`) как источник: его символы такими, как их
нарисовал `tools/imprint`, в самом большом из его размеров, ячейка которого
помещается в запрошенную, посередине этой ячейки — без повторного
сглаживания и никогда не мельче размера, который у него есть. Им рисуют те,
кто не умеет читать шрифт (загрузчик, ядро в аварийном экране), и он же
спасает того, кто умеет, когда шрифт не читается. Оттиск остаётся у
владельца и должен жить, пока жив источник.

## Класс ImprintSource

### `ImprintSource`

**Краткое описание:** Создаёт источник поверх оттиска.

**Параметры:**
- `Of` (`const BoxImprint &`): оттиск; источник хранит ссылку.

**Возвращает:** Неприменимо: конструктор (`explicit`).

**Побочные эффекты:** Нет.

**Примечания:** Системный оттиск `BoxSystemImprint` объявлен в генерируемом при сборке `boxos_imprint_data.h`.

**Пример:** См. [chain](chain.md).

### `Has` / `Draw`

**Краткое описание:** Сообщают, есть ли символ в оттиске, и рисуют его.

**Параметры:**
- `Character` (`char32_t`), `At` (`const Size &`).

**Возвращает:** `Has` — `bool`; `Draw` — `std::optional<GlyphImage>`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/imprint_source.h"

bool InImprint(const typecase::ImprintSource &Source, char32_t Character) { return Source.Has(Character); }
```
