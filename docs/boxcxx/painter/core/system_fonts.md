# painter::core::system_fonts

## Назначение

Заголовок: [`painter/core/system_fonts.h`](../../../../src/userspace/painter/include/painter/core/system_fonts.h).
Реализация: [`src/core/system_fonts.cpp`](../../../../src/userspace/painter/src/core/system_fonts.cpp).

Шрифты системы и чтение файла шрифта с тома TagFS. Шрифты — обычные файлы
с тегом `font`; файл носит основу своего имени (до последней точки) как
тег, поэтому `JetBrainsMonoNL-Regular.ttf` находится по
`JetBrainsMonoNL-Regular`. Файлы в корзине не находятся: человек их
отложил.

Ячейки консоли пользуются своим шрифтом — настройкой дисплея
`text font`: это семейство для кода, а не для текста.

## Функции

### `SystemFonts`

**Краткое описание:** Возвращает книгу шрифтов системы.

**Параметры:** Нет.

**Возвращает:** (`std::shared_ptr<const typecase::Fonts>`) книга: все файлы на томе с тегом `font`, которые читаются как шрифт; семейство по умолчанию — `SystemTextFamily` (Inter), остальные — запасные.

**Побочные эффекты:** При первом вызове читает файлы целиком (потокобезопасно, один раз за жизнь программы); файл, который не читается, сообщается в журнал отладки и пропускается.

**Примечания:** Чтение всех шрифтов — заметная работа при первом обращении; вызывайте один раз и храните указатель.

**Типичное применение:** Набор текста для `Canvas::FillText` и `TextPath`.

**Пример:** См. [text](text.md).

### `SystemTextFamily`

**Краткое описание:** Называет семейство, которым набирается текст, если не сказано другое.

**Параметры:** Нет; `inline constexpr const char *SystemTextFamily = "Inter"`.

**Возвращает:** Неприменимо: константа.

**Побочные эффекты:** Нет.

**Примечания:** Действует, пока тема не задаёт семейство каждой роли.

**Пример:**
```cpp
#include <string>

#include "painter/core/system_fonts.h"

const std::string Family = painter::core::SystemTextFamily;
```

### `ReadFontFile`

**Краткое описание:** Находит файл шрифта по имени везде на томе и читает его целиком.

**Параметры:**
- `Name` (`std::string_view`): имя файла, например `"Inter-Bold.ttf"`.
- `Tags` (`std::string_view`): дополнительные теги через запятую, которые файл должен носить; `""` — без них.

**Возвращает:** (`std::expected<std::vector<std::uint8_t>, std::string>`) байты файла, или фраза, объясняющая причину: имени нет, у имени нет основы, имя содержит `:` или `,`, такого файла нет, файл не читается, не хватает памяти.

**Побочные эффекты:** Запрос к тому (не только к текущему Use Context), чтение файла. Если подходят несколько файлов, читается созданный первым, и это сообщается в журнал отладки.

**Примечания:** Байты не разбираются: шрифт из них делает `typecase::Font::Read`, передаёт дисплею `painter::gui::Area::OpenFont`.

**Пример:**
```cpp
#include <print>

#include "painter/core/system_fonts.h"

bool Load(std::vector<std::uint8_t> &Into)
{
    auto Bytes = painter::core::ReadFontFile("JetBrainsMonoNL-Bold.ttf");
    if (!Bytes) {
        std::println("font: {}", Bytes.error());
        return false;
    }
    Into = std::move(*Bytes);
    return true;
}
```
