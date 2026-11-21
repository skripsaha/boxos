# box::heap

## Назначение

Заголовок: [`box/cxx/heap.h`](../../../src/userspace/boxcxx/include/box/cxx/heap.h).
C-основа: `box/memory.h`.

Куча BoxOS помечает каждое выделение **тегом**, поэтому память можно
считать по подсистемам («render», «physics», «ml:weights»). Здесь:

- `box::tagged_resource` — `std::pmr::memory_resource`, который выделяет через `malloc_tagged(bytes, tag)`: вся память контейнера `std::pmr` считается под одним тегом;
- `box::heap` — запросы к живой куче (`counters`, `count`, `for_each`), выделение с настоящей причиной ошибки (`allocate`, `reallocate`, `make`) и `box::heap::tag` с охраной от утечек `watch()`.

Строка имени тега должна жить, пока используется (естественный выбор —
строковый литерал). Реестр тегов ограничен (`HEAP_TAG_CAP`); сверх него
выделение идёт без тега, а не завершается ошибкой.

**Правило блокировки:** `for_each` и `for_each_tagged` вызывают функцию
при **удержанной** блокировке кучи — функция не должна выделять или
освобождать память и обращаться к `box::heap` (ни напрямую, ни через
контейнер): это та же нерекурсивная блокировка, будет взаимоблокировка.
Читать, суммировать, копировать наружу можно.

## tagged_resource

### `box::tagged_resource`

**Краткое описание:** Ресурс памяти `std::pmr`, помечающий каждое выделение тегом.

**Параметры:** `tagged_resource(const char *tag)` (`explicit`): регистрирует тег сразу, чтобы запросы работали до первого выделения.

**Возвращает:** Неприменимо: класс; копировать нельзя. `tag()` — имя.

**Побочные эффекты:** Выделения через `malloc_tagged`; бросает `std::bad_alloc` при нехватке.

**Как работает:** До 16 байт выравнивания — прямо `malloc_tagged`; большее выравнивание — с запасом и обратным указателем на исходный блок для освобождения.

**Пример:**
```cpp
#include <memory_resource>
#include <vector>

#include "box/cxx/heap.h"

std::size_t Mesh()
{
    box::tagged_resource Res("render:mesh");
    std::pmr::vector<float> Vertices(&Res);
    Vertices.resize(3000);
    return box::heap::count("render:mesh");   // живые блоки под тегом
}
```

## Запросы

### `heap::counters` / `heap::stats`

**Краткое описание:** Возвращают снимок счётчиков всей кучи.

**Параметры:** Нет.

**Возвращает:** (`heap::stats`): `heap_bytes()` (взято у системы), `in_use_bytes()`, `free_bytes()`, `overhead_bytes()` (служебные расходы), `live_blocks()`, `free_blocks()`, `malloc_calls()`, `free_calls()`, `raw()`. Есть `std::formatter` — одна строка `heap used=… in_use=… …`, без выделения памяти.

**Побочные эффекты:** Нет (потокобезопасно).

**Пример:**
```cpp
#include <print>

#include "box/cxx/heap.h"

void Report() { std::println("{}", box::heap::counters()); }
```

### `heap::count` / `heap::tag_id` / `heap::tag_name` / `heap::tag_none`

**Краткое описание:** Возвращают число живых блоков под тегом, id тега без регистрации, имя по id и «нет тега».

**Параметры:** `tag_name` / `name` (`const char *`), `id` (`std::uint8_t`).

**Возвращает:** `count` — (`std::size_t`); `tag_id` — (`std::uint8_t`), `tag_none`, если тег не регистрировался; `tag_name` — (`const char *`) или `nullptr`.

**Побочные эффекты:** Нет; `tag_id` тег не создаёт.

**Пример:**
```cpp
#include "box/cxx/heap.h"

bool Known(const char *Name) { return box::heap::tag_id(Name) != box::heap::tag_none; }
```

### `heap::for_each` / `heap::for_each_tagged`

**Краткое описание:** Обходят живые блоки под тегом или под любым тегом.

**Параметры:**
- `tag_name` (`const char *`) — для `for_each`.
- `fn` — вызывается как `fn(void *ptr, std::size_t size, const char *tag_name)`.

**Возвращает:** `void`.

**Побочные эффекты:** Держат блокировку кучи во время обхода — см. правило блокировки выше.

**Пример:**
```cpp
#include "box/cxx/heap.h"

std::size_t Largest(const char *Tag)
{
    std::size_t Most = 0;
    box::heap::for_each(Tag, [&](void *, std::size_t Size, const char *) { if (Size > Most) Most = Size; });
    return Most;
}
```

## Выделение с причиной

### `heap::allocate` / `heap::reallocate` / `heap::last_error`

**Краткое описание:** Выделяют и меняют размер памяти, возвращая настоящую причину неудачи вместо исключения.

**Параметры:**
- `allocate(std::size_t bytes)`, `allocate(std::size_t bytes, const char *tag)`: `bytes` > 0.
- `reallocate(void *p, std::size_t bytes)`: `bytes == 0` освобождает `p`.

**Возвращает:** (`box::result<void *>`); ошибка — `invalid_argument` (0 байт), причина кучи (исчерпана, повреждена, переполнение…). `reallocate` с 0: чистое освобождение — значение `nullptr`, обнаруженное двойное освобождение или повреждение — ошибка; при неудачном изменении размера `p` не трогается. `last_error` — (`box::error`) последняя причина операции кучи **этой нити**.

**Побочные эффекты:** Выделение или освобождение памяти. Никогда не бросают.

**Как работает:** Причина хранится в ячейке своей нити, поэтому успех соседней нити не скроет неудачу этого вызова.

**Пример:**
```cpp
#include <print>

#include "box/cxx/heap.h"

void *Buffer(std::size_t Bytes)
{
    auto Got = box::heap::allocate(Bytes, "io:buffer");
    if (!Got) std::println("нет памяти: {}", Got.error());
    return Got.value_or(nullptr);
}
```

### `heap::make<T>` / `heap::tagged<T>` / `heap::tagged_deleter`

**Краткое описание:** Создаёт объект `T` в памяти под тегом и возвращает владеющий указатель.

**Параметры:**
- `tag` (`const char *`), `args` — аргументы конструктора `T`.

**Возвращает:** (`box::result<heap::tagged<T>>`): `tagged<T>` = `std::unique_ptr<T, tagged_deleter>` (удаление — деструктор и `free`); ошибка — причина выделения.

**Побочные эффекты:** Выделение; исключение конструктора `T` пробрасывается после освобождения памяти.

**Примечания:** `alignof(T)` не больше 16 (проверяется при компиляции); для большего выравнивания — `tagged_resource`.

**Пример:**
```cpp
#include "box/cxx/heap.h"

struct Level {
    int Width = 0, Height = 0;
};

bool Load()
{
    auto Made = box::heap::make<Level>("game:level");
    return Made && (*Made)->Width == 0;
}
```

## Тег

### `box::heap::tag`

**Краткое описание:** Дескриптор одного тега кучи с запросами и охраной от утечек.

**Параметры:** `tag(const char *name)` (`explicit`): регистрирует тег.

**Возвращает:** Неприменимо: класс. `name()`, `id()`, `registered()`, `count()` — живые блоки, `bytes()` — живые байты, `for_each(fn)`, `watch()`.

**Побочные эффекты:** Регистрация тега; `bytes` и `for_each` держат блокировку кучи.

**Пример:** См. `tag::watch`.

### `tag::watch` / `watch_guard`

**Краткое описание:** Охрана от утечек: запоминает число живых блоков тега и при уничтожении сообщает, если их стало больше.

**Параметры:** Нет.

**Возвращает:** (`watch_guard`, `[[nodiscard]]`): `leaked()` — сколько блоков появилось и не освобождено; `dismiss()` — не сообщать (если удержание ожидаемо). Только перемещение.

**Побочные эффекты:** Деструктор печатает `[heap leak] tag '…': N allocation(s) still live at scope exit`.

**Типичное применение:** Проверить, что загрузка и выгрузка ресурса ничего не оставляют.

**Пример:**
```cpp
#include <memory_resource>
#include <vector>

#include "box/cxx/heap.h"

void Cycle()
{
    box::heap::tag Mesh("render:mesh");
    auto Guard = Mesh.watch();
    {
        box::tagged_resource Res("render:mesh");
        std::pmr::vector<int> Data(1000, 0, &Res);
    }   // всё освобождено — охрана промолчит
}
```
