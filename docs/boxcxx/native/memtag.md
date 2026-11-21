# box::memtag

## Назначение

Заголовок: [`box/cxx/memtag.h`](../../../src/userspace/boxcxx/include/box/cxx/memtag.h).
C-основа: `box/memtag.h`.

**MemTag** — реестр областей оперативной памяти, устроенный как TagFS:
каждая область физической памяти носит упорядоченный набор тегов
`ключ:значение`, а кабины получают и теряют доступ к охраняемым тегам.

- `box::memtag::region` — вид одной области (id, описание, её теги, шифрование);
- `box::memtag::stats` — снимок счётчиков реестра;
- запросы: `find`, `covering`, `query`, `counters` (без привилегий);
- управление: `set_guard`, `grant`, `revoke`, `grant_scope` (нужен системный бит тега TagFS `system`); `cabin_tags`, `check_access` — без привилегий.

По умолчанию реестр разрешающий: пока ни один тег не охраняется,
`check_access` истинно для любой области.

## Типы

### `memtag::region`

**Краткое описание:** Вид одной помеченной области памяти.

**Параметры:** Создаётся `find`, `covering`, `query`.

**Возвращает:** Неприменимо: класс. `id()`, `raw()`, `base_phys()`, `base_virt()`, `pages()`, `size_bytes()` (страницы × 4096), `tag_count()`, `flags()`, `generation()`.

**Побочные эффекты:** Нет. Есть `std::formatter` (одна строка без системных вызовов).

**Пример:** См. `memtag::query`.

### `region::tags` / `region::has_tag`

**Краткое описание:** Возвращают теги области и проверяют наличие тега.

**Параметры:** `tag` (`std::string_view`) для `has_tag`.

**Возвращает:** `tags` — (`std::vector<std::string>`) строки `ключ:значение`, пусто при ошибке; `has_tag` — (`bool`).

**Побочные эффекты:** Системный вызов (буфер не больше 16 КиБ за раз).

**Пример:**
```cpp
#include "box/cxx/memtag.h"

bool WriteBack(const box::memtag::region &Area) { return Area.has_tag("cache:wb"); }
```

### `region::encrypted` / `region::keyid`

**Краткое описание:** Сообщают, шифруется ли память (TME), и ключ TME-MK области.

**Параметры:** Нет.

**Возвращает:** `encrypted` — (`bool`) TME включён на всей платформе; `keyid` — (`std::uint16_t`) ключ области, 0 при простом TME или без TME.

**Побочные эффекты:** Чтение состояния TME ([hw](hw.md)).

**Примечания:** Это производный вид, а не факт ядра об области: отдельного вызова «зашифрована ли область» нет.

**Пример:**
```cpp
#include "box/cxx/memtag.h"

bool Private(const box::memtag::region &Area) { return Area.keyid() != 0; }
```

### `memtag::stats` / `memtag::counters`

**Краткое описание:** Снимок счётчиков реестра.

**Параметры:** Нет.

**Возвращает:** `counters()` — (`box::result<stats>`): `tag_count()`, `region_active()`, `region_slot_count()`, `region_slot_cap()`, `registry_generation()`, `region_generation()`, `bitmap_generation()`, `cache_hits()`, `cache_misses()`, `raw()`; есть `std::formatter`.

**Побочные эффекты:** Системный вызов.

**Пример:**
```cpp
#include <print>

#include "box/cxx/memtag.h"

void Report() { if (auto Now = box::memtag::counters()) std::println("{}", *Now); }
```

## Запросы

### `memtag::find` / `memtag::covering`

**Краткое описание:** Возвращают область по id или область, покрывающую физический адрес.

**Параметры:** `region_id` (`std::uint32_t`); `phys` (`std::uint64_t`).

**Возвращает:** (`box::result<region>`); ошибка — `object_not_found` (нет такой области / адрес не покрыт) или причина неудачного запроса.

**Побочные эффекты:** Системный вызов.

**Пример:**
```cpp
#include "box/cxx/memtag.h"

std::uint64_t Pages(std::uint64_t Phys) { auto Area = box::memtag::covering(Phys); return Area ? Area->pages() : 0; }
```

### `memtag::query`

**Краткое описание:** Возвращает области, теги которых удовлетворяют условию (все обязательные ∧ хотя бы один из «любых» ∧ ни одного исключённого).

**Параметры:**
- `required`, `any`, `excluded` (`std::initializer_list<const char *>`): списки `"ключ:значение"`; пустой список пропускает условие.

**Возвращает:** (`std::vector<region>`) до `MEMTAG_MAX_QUERY_RESULTS` областей.

**Побочные эффекты:** Системные вызовы.

**Пример:**
```cpp
#include <print>

#include "box/cxx/memtag.h"

void Zone0()
{
    for (const box::memtag::region &Area : box::memtag::query({"zone:0"}, {}, {"reserved:true"})) std::println("{}", Area);
}
```

## Права

### `memtag::set_guard` / `grant` / `revoke`

**Краткое описание:** Включают охрану тега и выдают или отзывают у кабины доступ к нему.

**Параметры:** `tag` (`const char *`), `on` (`bool`); `pid` (`std::uint32_t`) для `grant`/`revoke`.

**Возвращает:** (`box::status`); ошибка — `invalid_argument` (`nullptr`), `access_denied` (у кабины нет системного бита).

**Побочные эффекты:** Охрана тега переводит каждую область с этим тегом под контроль: доступ только у кабин, которым тег выдан.

**Пример:**
```cpp
#include "box/cxx/memtag.h"

bool Lock(std::uint32_t Driver) { return box::memtag::set_guard("dev:nic", true) && box::memtag::grant(Driver, "dev:nic"); }
```

### `memtag::cabin_tags` / `memtag::check_access`

**Краткое описание:** Возвращают теги, выданные кабине, и проверяют её доступ к области.

**Параметры:** `pid` (`std::uint32_t`); `region_id` (`std::uint32_t`) или `const region &`.

**Возвращает:** `cabin_tags` — (`std::vector<std::string>`); `check_access` — (`bool`), при ошибке `false`.

**Побочные эффекты:** Системные вызовы; привилегий не требуют.

**Пример:**
```cpp
#include "box/cxx/memtag.h"

bool May(std::uint32_t Pid, const box::memtag::region &Area) { return box::memtag::check_access(Pid, Area); }
```

### `memtag::grant_scope` / `scoped_grant`

**Краткое описание:** Выдаёт кабине тег на время жизни объекта.

**Параметры:** `pid` (`std::uint32_t`), `tag` (`const char *`).

**Возвращает:** (`box::result<scoped_grant>`): `pid()`, `tag()`, `held()` — выдал ли тег именно этот объект. Если тег у кабины уже был, объект ничего не делает и при уничтожении ничего не отзывает — прежнее право никогда не отбирается. Только перемещение.

**Побочные эффекты:** Выдача; деструктор отзывает только собственную выдачу.

**Примечания:** Парного объекта для охраны нет: прочитать прежнее состояние охраны нельзя, поэтому честно восстановить его невозможно.

**Пример:**
```cpp
#include "box/cxx/memtag.h"

void Maintenance(std::uint32_t Tool)
{
    auto Lease = box::memtag::grant_scope(Tool, "dev:disk");
    if (!Lease) return;
    // ... пока Lease жив, у Tool есть доступ к областям dev:disk
}
```
