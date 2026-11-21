# box::tagfs

## Назначение

Заголовок: [`box/cxx/tagfs.h`](../../../src/userspace/boxcxx/include/box/cxx/tagfs.h).
C-основа: `box/file.h`.

Файлы BoxOS. Хранилище — не дерево путей, а объекты с тегами: файл имеет
числовой id, имя (не обязательно уникальное) и теги `ключ:значение`.
Файлы находят запросом по тегам, а не обходом папок. `std::filesystem` в
BoxOS нет — намеренно.

- `box::tagfs::file` — лёгкий **невладеющий** дескриптор файла по id: метаданные, изменение тегов, чтение и запись по смещению, потоковый канал;
- `box::tagfs::record` — неизменяемый снимок метаданных (без системных вызовов при чтении полей);
- `create`, `query`, `all`, `find`, `find_all`, `tags` — создание и поиск;
- `on_anchor`, `anchor_event` — события надёжной записи на диск.

**Use Context** ([use](use.md)) — контекст человека, один на машину. Все
`query` и `create` выполняются **внутри** него: после `use code cpp`
запрос `query("project")` значит «code И cpp И project», а созданный файл
получает теги `code` и `cpp`. Варианты `*_everywhere` спрашивают весь том
без контекста.

**Теги файлов — только человека.** Ядро не придаёт тегам файлов особого
смысла действий; программа ставит те теги, которые описывают файл.

У одного файла хранится до 5 тегов в описании (`file_info_t`).

## Тег

### `box::tagfs::tag`

**Краткое описание:** Хранит один тег файла.

**Параметры:**
- `key`, `value` (`std::string`): ключ и значение (пустое у голого тега).
- `system` (`bool`): голый тег из зарезервированного словаря, который ядро ставит при монтировании.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `tag::as`.

### `tag::as<T>`

**Краткое описание:** Читает значение тега как целое число или `bool`.

**Параметры:** `T` — целый тип или `bool` (`"0"`/`"1"`).

**Возвращает:** (`box::result<T>`); ошибка — `out_of_range` (не помещается) или `invalid_argument` (не число).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

int Year(const box::tagfs::file &Photo)
{
    auto Tag = Photo.tag_named("year");
    if (!Tag) return 0;
    return Tag->as<int>().value_or(0);
}
```

## Файл

### `box::tagfs::file`

**Краткое описание:** Невладеющий дескриптор файла по id.

**Параметры:** `file()` — пустой; `file(file_id_t)` (`explicit`).

**Возвращает:** Неприменимо: класс-значение, копируется. `id()`, `operator bool` (id не 0).

**Побочные эффекты:** Нет: файл — постоянное общее состояние, дескриптор ничего не закрывает; удаление — явное `remove()`.

**Пример:** См. `box::tagfs::create`.

### `file::info`

**Краткое описание:** Возвращает полный снимок метаданных файла.

**Параметры:** Нет.

**Возвращает:** (`box::result<file_info_t>`); ошибка — `invalid_argument` для пустого дескриптора или причина ядра (`object_not_found`, если файла уже нет).

**Побочные эффекты:** Один системный вызов.

**Примечания:** Каждый из `name`, `size`, `tags`, `has_tag`, `tag_named` делает свой снимок: если нужно несколько полей, вызовите `info()` один раз (или возьмите `record`).

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

std::uint64_t Bytes(const box::tagfs::file &Of) { auto Info = Of.info(); return Info ? Info->size : 0; }
```

### `name` / `size` / `tags` / `has_tag` / `tag_named`

**Краткое описание:** Возвращают имя, размер, теги файла, наличие тега и тег по ключу.

**Параметры:** `key` (`const char *`) — для `has_tag` и `tag_named`.

**Возвращает:** `name` — (`std::string`), пусто при ошибке; `size` — (`std::uint64_t`), 0 при ошибке; `tags` — (`std::vector<tag>`); `has_tag` — (`bool`); `tag_named` — (`box::result<tag>`), ошибка — причина `info()` или `tag_not_found`.

**Побочные эффекты:** По одному системному вызову.

**Пример:**
```cpp
#include <print>

#include "box/cxx/tagfs.h"

void Show(const box::tagfs::file &Of)
{
    std::print("{}:", Of.name());
    for (const box::tagfs::tag &Each : Of.tags()) std::print(" {}{}{}", Each.key, Each.value.empty() ? "" : ":", Each.value);
    std::println("");
}
```

### `add_tag` / `remove_tag` / `rename` / `remove` / `anchor` / `span`

**Краткое описание:** Меняют теги и имя, удаляют файл, сохраняют его надёжно и задают его длину.

**Параметры:**
- `add_tag(const char *tag)` — `"ключ:значение"` или `"ключ"`; `add_tag(const char *key, T v)` — целое или `bool`, значение не длиннее 11 символов.
- `remove_tag(const char *key)`.
- `rename(const char *new_name)`.
- `span(std::uint64_t n)` — сделать файл ровно `n` байт: короче — хвост отрезается и блоки возвращаются; длиннее — новые байты читаются нулями.

**Возвращает:** (`box::status`); ошибка — `invalid_argument` для пустого дескриптора, `buffer_too_small` (длинное значение), или причина ядра (предел тегов файла, `already_exists`, `object_not_found`, `permission_denied` для файла, который система держит в роли).

**Побочные эффекты:** Меняют том. `remove` кладёт файл в корзину; `anchor` записывает его надёжно на диск; изменения публикуются на каждом теге файла (Touch).

**Примечания:** Запись расширяет файл только в его конце; запись за концом — ошибка. Пропуск («дыра») делается через `span`.

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

box::status Rate(box::tagfs::file Photo, int Stars)
{
    if (auto Done = Photo.add_tag("stars", Stars); !Done) return Done;
    return Photo.anchor();
}
```

### `read_at` / `write_at` / `append`

**Краткое описание:** Читают и пишут байты по смещению и дописывают в конец.

**Параметры:**
- `offset` (`std::uint64_t`).
- `p` (`void *` / `const void *`), `n` (`std::size_t`); или `buf` (`std::span<std::byte>` / `std::span<const std::byte>`).

**Возвращает:** (`box::result<std::size_t>`) число перенесённых байтов (до 4 ГиБ); ошибка — `invalid_argument` или причина ядра (`out_of_range`, `io`).

**Побочные эффекты:** Ввод-вывод. `append` пишет в конец файла таким, каким он будет в момент записи: несколько писателей не наложат байты друг на друга.

**Пример:**
```cpp
#include <string_view>

#include "box/cxx/tagfs.h"

box::result<std::size_t> Log(box::tagfs::file Journal, std::string_view Line) { return Journal.append(Line.data(), Line.size()); }
```

### `read_async` / `write_async`

**Краткое описание:** Ставят чтение или запись в очередь без ожидания и возвращают объект для `co_await`.

**Параметры:** Как у `read_at` / `write_at`.

**Возвращает:** (`box::ferry`) — `co_await` в [executor](executor.md) даёт число байтов или причину ([ferry](ferry.md)). Пустой дескриптор — сразу готовый `invalid_argument`.

**Побочные эффекты:** Ядро пишет в буфер по DMA: буфер должен пережить `co_await`. Несколько операций могут идти одновременно.

**Примечания:** Нужно подключить `box/cxx/ferry.h`.

**Пример:** См. [ferry](ferry.md).

### `read_object` / `write_object`

**Краткое описание:** Читают и пишут целый объект тривиально копируемого типа по смещению.

**Параметры:** `offset` (`std::uint64_t`); `v` (`const T &`) для записи.

**Возвращает:** (`box::result<T>`) / (`box::status`); неполная передача — `io`, а не тихое усечение.

**Побочные эффекты:** Ввод-вывод.

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

struct Save {
    std::uint32_t Level, Score;
};

box::result<Save> Load(const box::tagfs::file &Slot) { return Slot.read_object<Save>(0); }
```

### `bytes`

**Краткое описание:** Открывает потоковый канал на файл через Current.

**Параметры:**
- `r` (`box::role`): чтение или запись; для записи файл создаётся при необходимости.

**Возвращает:** (`box::byte_current`) с `write`, `read`, `read_line`, `seek` ([current](current.md)); пустой при ошибке.

**Побочные эффекты:** Открывает канал.

**Примечания:** Канал находит файл по **имени** (схема `file:`); для ввода-вывода точно по id — `read_at`/`write_at`.

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

void Lines(const box::tagfs::file &Text)
{
    box::byte_current In = Text.bytes(box::role::read);
    for (std::string Line = In.read_line(); !Line.empty(); Line = In.read_line()) { /* ... */ }
}
```

## Снимок

### `box::tagfs::record`

**Краткое описание:** Неизменяемый снимок метаданных: id и собственная копия `file_info_t`.

**Параметры:** `record()`; `record(file_id_t, const file_info_t &)`.

**Возвращает:** Неприменимо: тривиально копируемое значение. `id()`, `operator bool`, `live()` — живой дескриптор, `raw()`, `name()`, `size()`, `tags()`, `has_tag()`, `tag_named()` — все без системных вызовов.

**Побочные эффекты:** Нет.

**Пример:** См. `box::tagfs::find`.

### `record::reread`

**Краткое описание:** Перечитывает снимок из ядра.

**Параметры:** Нет.

**Возвращает:** (`box::status`).

**Побочные эффекты:** Один системный вызов.

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

bool Fresh(box::tagfs::record &Of) { return Of.reread().has_value(); }
```

## Создание и поиск

### `box::tagfs::create` / `create_everywhere`

**Краткое описание:** Создаёт файл с именем и тегами.

**Параметры:**
- `name` (`const char *`).
- `tags` (`std::initializer_list<const char *>`) — `{"ключ:значение", "голый"}`; или `tagspec` (`const char *`) — теги через запятую, по умолчанию `""`.

**Возвращает:** (`box::result<file>`); ошибка — `invalid_argument` (пустое или длинное имя) или причина ядра (`already_exists`).

**Побочные эффекты:** Создаёт файл; `create` добавляет теги Use Context, `create_everywhere` — нет.

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

box::result<box::tagfs::file> NewNote() { return box::tagfs::create("todo.txt", {"note", "priority:high"}); }
```

### `box::tagfs::query` / `all` / `query_everywhere` / `all_everywhere`

**Краткое описание:** Возвращают файлы, которые носят все теги запроса.

**Параметры:**
- `tagspec` (`const char *`): теги через запятую; `nullptr` — все файлы.
- `without` (`const char *`): теги через запятую, файлы с которыми исключить; `nullptr` — никаких. Том сам ничего не скрывает; удалённые файлы не находятся.

**Возвращает:** (`std::vector<file>`) живые дескрипторы, все совпадения; ошибка — пустой вектор.

**Побочные эффекты:** Запрос к тому.

**Пример:**
```cpp
#include <ranges>

#include "box/cxx/tagfs.h"

std::size_t BigVideos()
{
    auto Big = box::tagfs::query("video", "trash") | std::views::filter([](const box::tagfs::file &F) { return F.size() > (1u << 20); });
    return static_cast<std::size_t>(std::ranges::distance(Big));
}
```

### `box::tagfs::find` / `find_all`

**Краткое описание:** Находят файл по имени.

**Параметры:**
- `name` (`const char *`), `without` (`const char *`).

**Возвращает:** `find` — (`box::result<record>`) первый найденный; ошибка — причина неудачного поиска или `file_not_found`. `find_all` — (`std::vector<record>`) все файлы с этим именем (имена не уникальны); ошибка — пустой вектор.

**Побочные эффекты:** Запрос к тому.

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

std::uint64_t ConfigSize()
{
    auto Found = box::tagfs::find("settings.cfg", nullptr);
    return Found ? Found->size() : 0;
}
```

### `box::tagfs::tags` / `tags_everywhere`

**Краткое описание:** Возвращают теги, которые носят файлы, и сколько файлов носит каждый.

**Параметры:**
- `without` (`const char *`): файлы с этими тегами не считаются.

**Возвращает:** (`std::vector<tag_count>`): `what` (`tag`), `files` (`std::uint32_t`); в порядке, в котором том узнал теги; ошибка — пустой вектор.

**Побочные эффекты:** Один запрос к списку тегов тома (не обход файлов).

**Примечания:** Теги, которые не носит ни один файл (процессов, Touch), в список не входят.

**Пример:**
```cpp
#include <print>

#include "box/cxx/tagfs.h"

void Cloud()
{
    for (const box::tagfs::tag_count &Each : box::tagfs::tags(nullptr)) std::println("{} — {}", Each.what.key, Each.files);
}
```

## Надёжная запись

### `box::tagfs::anchor_all`

**Краткое описание:** Сбрасывает всю файловую систему на надёжный носитель.

**Параметры:** Нет.

**Возвращает:** (`box::status`).

**Побочные эффекты:** Запись на диск; публикует событие на теге `anchor`.

**Пример:**
```cpp
#include "box/cxx/tagfs.h"

bool Sync() { return box::tagfs::anchor_all().has_value(); }
```

### `box::tagfs::on_anchor` / `anchor_event` / `anchor_payload`

**Краткое описание:** Подписываются на события надёжной записи и разбирают их.

**Параметры:**
- `on_anchor()` — заявка на тег `anchor`.
- `anchor_event(const box::touch &)` — разбор события; `file_id()` (0 — вся ФС), `op()` (2 — ANCHOR, 1 — WRITE на тегах файла), `whole_fs()`, `is_anchor()`, `operator bool`.
- `anchor_payload` — 24-байтовая раскладка нагрузки ядра: `file_id`, `op`, `now_us`.

**Возвращает:** `on_anchor` — (`box::subscription`).

**Побочные эффекты:** Подписка.

**Пример:**
```cpp
#include <print>

#include "box/cxx/tagfs.h"

void Durable()
{
    box::subscription Anchors = box::tagfs::on_anchor();
    while (auto Event = Anchors.wait(5000))
        if (box::tagfs::anchor_event Got(*Event); Got.is_anchor()) std::println("надёжно: файл {}", Got.file_id());
}
```
