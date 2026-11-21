# typecase::fonts

## Назначение

Заголовок: [`typecase/fonts.h`](../../../src/userspace/typecase/include/typecase/fonts.h).
Реализация: [`src/fonts.cpp`](../../../src/userspace/typecase/src/fonts.cpp).

Книга шрифтов, которыми набирается текст: начертания, собранные в
семейства по имени семейства, которое называет каждый шрифт. Стиль просит
семейства по имени — одно или несколько через запятую, самое желанное
первым, как `font-family` в CSS, — насыщенность и наклон. В каждом
семействе находится ближайшее начертание так, как находит CSS:

1. наклонное или прямое — как просили, если в семействе такие есть;
2. затем ближайшая насыщенность;
3. если подходящего нет — начертание делается полужирным (просили 600 и больше, а ближайшее легче 600) или наклонным (просили курсив, а курсива нет) — см. [glyph_look](glyph_look.md).

Символ, которого нет в выбранном начертании, ищется в следующем названном
семействе, затем в запасных семействах (системных), затем в семействе по
умолчанию, затем в любом начертании, где он есть; символ, которого нет
нигде, рисуется своим кодом. Текст никогда не исчезает.

Начертания разделяются: разложенный текст держит начертания, которыми он
набран, живыми. Книгой пользуется один поток за раз.

Книгу системных шрифтов даёт `painter::core::SystemFonts()`.

## Типы

### `FaceChoice`

**Краткое описание:** Описывает начертание, к которому пришёл стиль в одном семействе, и во что его превращают.

**Параметры:**
- `Face` (`std::shared_ptr<const Font>`): начертание; пусто — его нет.
- `Bold` (`bool`): сделать полужирным.
- `Slant` (`bool`): сделать наклонным.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `Fonts::Choose`.

## Класс Fonts

### `Fonts::Add`

**Краткое описание:** Добавляет начертание в его семейство.

**Параметры:**
- `Face` (`Font` или `std::shared_ptr<const Font>`).

**Возвращает:** `void`.

**Побочные эффекты:** Семейство — то, что называет шрифт (`Font::Style().Family`), или `""` для шрифта без имени. Первое добавленное семейство становится семейством по умолчанию, если его не задали.

**Пример:**
```cpp
#include "painter/core/system_fonts.h"
#include "typecase/fonts.h"

bool AddOwn(typecase::Fonts &Book)
{
    auto Bytes = painter::core::ReadFontFile("Inter-Bold.ttf");
    if (!Bytes) return false;
    auto Face = typecase::Font::Read(std::move(*Bytes));
    if (!Face) return false;
    Book.Add(std::move(*Face));
    return true;
}
```

### `Fonts::SetFallback` / `Fonts::SetDefault`

**Краткое описание:** Задают запасные семейства и семейство по умолчанию.

**Параметры:**
- `Families` (`std::vector<std::string>`): запасные, по порядку; спрашиваются после названных стилем.
- `Family` (`std::string`): семейство для стиля, который не называет ни одного, и последнее, у кого спрашивают перед «любым начертанием».

**Возвращает:** `void`.

**Побочные эффекты:** Меняют книгу.

**Пример:**
```cpp
#include "typecase/fonts.h"

void Prefer(typecase::Fonts &Book)
{
    Book.SetDefault("Inter");
    Book.SetFallback({"JetBrains Mono NL"});
}
```

### `Empty` / `Families` / `Default` / `Fallbacks` / `Faces`

**Краткое описание:** Возвращают признак пустоты, имена семейств, семейство по умолчанию, запасные и все начертания.

**Параметры:** Нет.

**Возвращает:** `bool`; `std::vector<std::string>`; `std::string`; `const std::vector<std::string> &`; `std::vector<std::shared_ptr<const Font>>` — семейство за семейством в порядке добавления.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include <print>

#include "typecase/fonts.h"

void List(const typecase::Fonts &Book)
{
    for (const std::string &Name : Book.Families()) std::println("{}", Name);
}
```

### `Fonts::Choices`

**Краткое описание:** Возвращает начертание каждого семейства для стиля, по порядку поиска.

**Параметры:**
- `Families` (`std::string_view`): имена через запятую.
- `Weight` (`std::uint16_t`), `Italic` (`bool`).

**Возвращает:** (`std::vector<FaceChoice>`) названные и имеющиеся семейства в их порядке, затем запасные, затем по умолчанию, затем все остальные — каждое один раз. Пусто только у пустой книги.

**Побочные эффекты:** Выделяет память.

**Пример:**
```cpp
#include "typecase/fonts.h"

std::size_t Candidates(const typecase::Fonts &Book) { return Book.Choices("Inter", 400, false).size(); }
```

### `Fonts::Choose` / `Fonts::ChooseFor`

**Краткое описание:** Возвращают первое начертание для стиля и первое, у которого есть символ.

**Параметры:**
- `Character` (`char32_t`) — для `ChooseFor`.
- `Families`, `Weight`, `Italic`: как у `Choices`.

**Возвращает:** (`FaceChoice`); пусто — книга пуста (`Choose`) или символа нет ни у кого (`ChooseFor`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/fonts.h"

bool CanShow(const typecase::Fonts &Book, char32_t Character)
{
    return Book.ChooseFor(Character, "", 400, false).Face != nullptr;
}
```

## Функции

### `Has`

**Краткое описание:** Сообщает, есть ли у начертания читаемый глиф для символа.

**Параметры:**
- `Face` (`const Font &`), `Character` (`char32_t`).

**Возвращает:** (`bool`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "typecase/fonts.h"

bool Cyrillic(const typecase::Font &Face) { return typecase::Has(Face, U'Ж'); }
```
