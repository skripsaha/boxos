# typecase::font

## Назначение

Заголовок: [`typecase/font.h`](../../../src/userspace/typecase/include/typecase/font.h).
Реализация: [`src/font.cpp`](../../../src/userspace/typecase/src/font.cpp),
карта символов — `character_map.cpp`, глифы — `glyph_reader.cpp`,
начертание — `face_style.cpp`, кернинг — `kerning.cpp`.

Шрифт TrueType, прочитанный из байтов файла: какой глиф рисует символ,
контур глифа, на сколько он двигает перо, кернинг пар, метрики строки и
место начертания в семействе (имя семейства, насыщенность, наклон).

`Read` забирает байты целиком и хранит их; откуда они — не спрашивает.
Байтам не доверяется: таблица за концом файла, точки глифа за его записью,
составной глиф, содержащий сам себя, — шрифт, который нельзя прочитать,
даёт `Fault` с объяснением, а не чтение за границей. Шрифт, таблицы которого
целы, а часть глифов — нет, читается; такие глифы называются один раз
(`Damage`) и никем не рисуются (`Readable`): их рисует следующий в цепочке.

Не читаются пока: контуры PostScript (CFF, `.otf` типа `OTTO`) — такой шрифт
даёт `Fault` об этом. Программы хинтинга (`fpgm`, `prep`, инструкции глифа)
пропускаются: глифы рисуются как задумано.

В системе шрифты лежат файлами с тегом `font` (Inter, JetBrains Mono NL — в
`src/userspace/typecase/fonts`, с лицензиями OFL).

## Типы

### `GlyphId`

**Краткое описание:** Номер глифа в шрифте.

**Параметры:** Нет; `std::uint16_t`. 0 — «нет такого глифа».

**Возвращает:** Неприменимо: тип.

**Побочные эффекты:** Нет.

**Пример:** См. `Font::Find`.

### `Fault`

**Краткое описание:** Объясняет, почему шрифт или глиф нельзя прочитать.

**Параметры:**
- `Said` (`std::string`): фраза для человека, таблица первой: `"glyf: glyph 812 runs past its table"`.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `Font::Read`.

### `FontMetrics`

**Краткое описание:** Хранит метрики строки в единицах шрифта.

**Параметры:**
- `UnitsPerEm` (`std::uint16_t`).
- `Ascender` (вверх от базовой линии), `Descender` (вниз, отрицательный), `LineGap` (интерлиньяж): типографские из OS/2, если шрифт просит, иначе из `hhea`.
- `XHeight`, `CapHeight`: 0, если шрифт не говорит.
- `UnderlinePosition`, `UnderlineThickness` (из `post`), `StrikePosition`, `StrikeThickness` (из OS/2): верх линии относительно базовой (Y вверх) и толщина; 0 — не сказано.

**Возвращает:** Неприменимо: агрегат (`std::int16_t`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/font.h"

float LineAt(const typecase::Font &Face, float Em)
{
    const typecase::FontMetrics &M = Face.Metrics();
    return static_cast<float>(M.Ascender - M.Descender + M.LineGap) * Em / M.UnitsPerEm;
}
```

### `FontStyle`

**Краткое описание:** Описывает начертание в его семействе.

**Параметры:**
- `Family` (`std::string`): семейство — типографское, если шрифт его называет, иначе обычное («Inter»).
- `Name` (`std::string`): имя начертания («Bold Italic»).
- `Weight` (`std::uint16_t`): 1–1000, из OS/2 или из стиля `head`.
- `Italic` (`bool`): курсив или наклонное.

**Возвращает:** Неприменимо: агрегат. Несказанные имена пусты.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/font.h"

bool IsBold(const typecase::Font &Face) { return Face.Style().Weight >= 600; }
```

### `PairKerning`

**Краткое описание:** Описывает, как сдвинуть пару глифов, в единицах шрифта.

**Параметры:**
- `FirstShift`, `FirstAdvance`: сдвиг первого и изменение его продвижения.
- `SecondShift`, `SecondAdvance`: то же для второго.

**Возвращает:** Неприменимо: агрегат; `==` поэлементное.

**Как работает:** Это парная корректировка GPOS; таблица `kern` даёт только `FirstAdvance`.

**Побочные эффекты:** Нет.

**Пример:** См. `Font::KerningOf`.

## Класс Font

### `Font::Read`

**Краткое описание:** Читает шрифт из байтов файла.

**Параметры:**
- `Bytes` (`std::vector<std::uint8_t>`): файл целиком; забирается.
- `Index` (`std::uint32_t`): номер шрифта в коллекции `.ttc`, по умолчанию 0.

**Возвращает:** (`std::expected<Font, Fault>`).

**Побочные эффекты:** Проверяет таблицы, находит повреждённые глифы.

**Примечания:** `Font` копируется (копия — тот же шрифт с тем же `Serial()`) и перемещается.

**Пример:**
```cpp
#include <print>

#include "painter/core/system_fonts.h"
#include "typecase/font.h"

std::optional<typecase::Font> Open(std::string_view Name)
{
    auto Bytes = painter::core::ReadFontFile(Name);
    if (!Bytes) return std::nullopt;
    auto Face = typecase::Font::Read(std::move(*Bytes));
    if (!Face) {
        std::println("{}: {}", Name, Face.error().Said);
        return std::nullopt;
    }
    if (Face->Damage()) std::println("{}: {}", Name, Face->Damage()->Said);
    return std::move(*Face);
}
```

### `Metrics` / `Style` / `GlyphCount` / `Serial`

**Краткое описание:** Возвращают метрики, начертание, число глифов и уникальный номер шрифта.

**Параметры:** Нет.

**Возвращает:** `const FontMetrics &`, `const FontStyle &`, `std::uint16_t`, `std::uint64_t` — номер, которого нет ни у одного другого прочитанного шрифта; по нему кэш узнаёт картинки глифов.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/font.h"

std::string Title(const typecase::Font &Face) { return Face.Style().Family + " " + Face.Style().Name; }
```

### `Font::Find`

**Краткое описание:** Возвращает глиф, который рисует символ.

**Параметры:**
- `Character` (`char32_t`).

**Возвращает:** (`GlyphId`) глиф; 0, если карта символов его не называет.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/font.h"

bool HasEuro(const typecase::Font &Face) { return Face.Find(U'€') != 0; }
```

### `OutlineOf` / `AdvanceOf` / `Readable`

**Краткое описание:** Возвращают контур глифа, его продвижение и признак читаемости.

**Параметры:**
- `Glyph` (`GlyphId`).

**Возвращает:** `OutlineOf` — (`std::expected<Outline, Fault>`) контур с составными частями на местах, в единицах шрифта; `AdvanceOf` — (`std::uint16_t`) в единицах шрифта; `Readable` — (`bool`).

**Побочные эффекты:** `OutlineOf` выделяет память.

**Пример:**
```cpp
#include "typecase/font.h"

float WidthOf(const typecase::Font &Face, char32_t Character, float Em)
{
    return static_cast<float>(Face.AdvanceOf(Face.Find(Character))) * Em / Face.Metrics().UnitsPerEm;
}
```

### `KerningOf` / `Kerns`

**Краткое описание:** Возвращают кернинг пары глифов и признак, что шрифт кернит хоть что-то.

**Параметры:**
- `First`, `Second` (`GlyphId`).
- `Of` (`Script`): письменность текста, по умолчанию `Common`.

**Возвращает:** `KerningOf` — (`PairKerning`) парные корректировки GPOS функции `kern` для письменности (или для её умолчания, затем для латиницы, если о письменности ничего не сказано); если GPOS ничего не кернит — пары таблицы `kern`; нули для шрифта без кернинга. `Kerns` — (`bool`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/font.h"

std::int32_t AV(const typecase::Font &Face)
{
    return Face.KerningOf(Face.Find(U'A'), Face.Find(U'V'), typecase::Script::Latin).FirstAdvance;
}
```

### `Font::Damage`

**Краткое описание:** Возвращает описание нечитаемых глифов.

**Параметры:** Нет.

**Возвращает:** (`const std::optional<Fault> &`) сколько глифов не читается и почему не читается первый; пусто для целого шрифта.

**Побочные эффекты:** Нет.

**Пример:** См. `Font::Read`.
