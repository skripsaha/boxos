# box::flat_hash_map / box::flat_hash_set

## Назначение

Заголовки: [`box/cxx/flat_hash_map.h`](../../../src/userspace/boxcxx/include/box/cxx/flat_hash_map.h),
[`box/cxx/flat_hash_set.h`](../../../src/userspace/boxcxx/include/box/cxx/flat_hash_set.h);
общий движок — [`box/cxx/flat_hash_table.h`](../../../src/userspace/boxcxx/include/box/cxx/flat_hash_table.h) (внутренний, напрямую не подключать).

Хэш-таблицы с открытой адресацией (robin-hood) — «плоские» соседи
`std::unordered_map` и `std::unordered_set`. `std::unordered_map` в boxcxx
— таблица с цепочками: блок кучи на каждый элемент, адреса узлов стабильны
(около 37 байт и один `malloc` на элемент при `<int,int>`). Плоская
таблица — другой обмен: **одно** выделение памяти, без заголовка на
элемент, ключи плотно лежат в одном массиве, поиск остаётся в кэше.

Устройство: три параллельных массива в одном блоке — управляющие слова
(занятость + отпечаток хэша + идеальный слот), ключи, значения (у множества
значений нет). Поиск читает только управляющие слова и ключи. Расстояние
пробы вычисляется, а не хранится, поэтому вырожденный хэш не ломает
таблицу; при перехэшировании хэш не вычисляется заново. Предел — 2³¹
слотов. Коэффициент заполнения зафиксирован на 7/8.

**Два отличия от `unordered_map`, которые нельзя пропустить:**

1. **Ничто не стабильно.** Любая вставка, удаление или перехэширование перемещает элементы: все итераторы, указатели и ссылки становятся недействительными после любого изменения.
2. `value_type` карты — `std::pair<Key, T>`, а **не** `pair<const Key, T>` (ключи перемещаются). `*it` возвращает **прокси** `pair<const Key&, T&>`: `for (auto [k, v] : m)` и `for (auto &&[k, v] : m)` работают, `for (auto &[k, v] : m)` — нет (прокси — временное значение). Ключ через итератор изменить нельзя.

У множества итератор отдаёт настоящий `const Key &`; `iterator` и
`const_iterator` — один тип.

Члены — как у `std::unordered_map`/`unordered_set`, кроме двух семейств,
которых у плоской таблицы физически нет: корзин (`bucket_count`, `bucket`,
`bucket_size`, `local_iterator`) и узлов (`extract`, `insert(node_type)`,
`merge`). Вместо них — словарь слотов и `fallible()`.

Гарантии исключений: аргумент, ссылающийся на элемент этой же таблицы
(`m[m.begin()->first]`), читается до перемещений; конструктор элемента,
бросивший исключение, оставляет таблицу как была (строгая гарантия).

## Контейнеры

### `box::flat_hash_map<Key, T, Hash, KeyEq, Alloc>` / `box::flat_hash_set<Key, Hash, KeyEq, Alloc>`

**Краткое описание:** Плоская хэш-карта и хэш-множество.

**Параметры:** Шаблонные параметры как у `std::unordered_map`/`unordered_set`; `Alloc` по умолчанию `std::allocator<std::pair<Key, T>>` (карта). Конструкторы и руководства вывода типов — из пары итераторов, `std::from_range`, списка инициализации, с аллокатором.

**Возвращает:** Неприменимо: классы. `==` и `swap` определены.

**Побочные эффекты:** Одно выделение памяти на весь массив.

**Пример:**
```cpp
#include <string>

#include "box/cxx/flat_hash_map.h"
#include "box/cxx/flat_hash_set.h"

int Count(const std::string &Text)
{
    box::flat_hash_map<char, int> Letters;
    for (char C : Text) ++Letters[C];
    box::flat_hash_set<char> Seen;
    for (auto [Letter, Times] : Letters)
        if (Times > 1) Seen.insert(Letter);
    return static_cast<int>(Seen.size());
}
```

### `box::pmr::flat_hash_map` / `box::pmr::flat_hash_set`

**Краткое описание:** Те же контейнеры на `std::pmr::polymorphic_allocator`.

**Параметры:** Как у основных, без `Alloc`; конструктор принимает `std::pmr::memory_resource *`.

**Возвращает:** Неприменимо: псевдонимы.

**Побочные эффекты:** С `box::tagged_resource` ([heap](heap.md)) вся память таблицы — **одно** помеченное выделение.

**Пример:**
```cpp
#include "box/cxx/flat_hash_map.h"
#include "box/cxx/heap.h"

void Indexed()
{
    box::tagged_resource Res("render:index");
    box::pmr::flat_hash_map<int, float> Depth(&Res);
    Depth.try_emplace(7, 0.5f);
}
```

## Словарь слотов

### `slot_count` / `load_factor` / `max_load_factor`

**Краткое описание:** Возвращают число слотов, заполненность и её предел.

**Параметры:** Нет.

**Возвращает:** `slot_count` — (`size_type`); `load_factor` — (`float`); `max_load_factor` — (`float`) 0.875, статический и неизменяемый (дальше длина проб плоской таблицы растёт без предела).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/flat_hash_set.h"

float Fullness(const box::flat_hash_set<int> &Set) { return Set.load_factor(); }
```

### `max_distance` / `probe_distance`

**Краткое описание:** Возвращают худшее расстояние пробы в таблице и сколько слотов просмотрит поиск ключа.

**Параметры:** `k` (`const key_type &` или прозрачный ключ) для `probe_distance`.

**Возвращает:** (`size_type`).

**Побочные эффекты:** Нет; `max_distance` — O(число слотов).

**Типичное применение:** Проверить, хорошо ли работает хэш-функция, а не гадать по секундомеру.

**Пример:**
```cpp
#include "box/cxx/flat_hash_map.h"

bool HashOk(const box::flat_hash_map<int, int> &Map) { return Map.max_distance() < 16; }
```

## Ошибки без исключений

### `fallible()`

**Краткое описание:** Возвращает фасад, операции которого сообщают причину неудачи результатом, а не `std::bad_alloc`.

**Параметры:** Нет.

**Возвращает:** Фасад с методами `reserve(n)`, `rehash(slots)` → `box::status`; `insert(v)`, `emplace(args…)`, а у карты ещё `try_emplace(k, args…)`, `insert_or_assign(k, v)` → `box::result<std::pair<iterator, bool>>`.

**Побочные эффекты:** Как у обычных операций.

**Как работает:** `std::bad_alloc` превращается в причину из ячейки кучи этой нити ([heap](heap.md), `last_error`), иначе — `no_memory`; `std::length_error` — `invalid_argument`.

**Пример:**
```cpp
#include <print>

#include "box/cxx/flat_hash_map.h"

bool Prepare(box::flat_hash_map<int, int> &Map)
{
    if (auto Done = Map.fallible().reserve(4096); !Done) {
        std::println("не хватило памяти: {}", Done.error());
        return false;
    }
    return true;
}
```

## Массовое удаление

### `box::erase_if`

**Краткое описание:** Удаляет все элементы, для которых предикат истинен.

**Параметры:**
- `m` — карта или множество.
- `pred` — у карты получает прокси `pair<const Key&, T&>` (принимайте `auto &&`), у множества — `const Key &`.

**Возвращает:** (`size_type`) число удалённых.

**Побочные эффекты:** Перемещает элементы.

**Как работает:** Предикат вызывается **ровно один раз** для каждого элемента, хотя удаление перемещает элементы. Самодельный цикл «удалять во время обхода» так не умеет — используйте `erase_if`.

**Пример:**
```cpp
#include "box/cxx/flat_hash_map.h"

std::size_t DropEmpty(box::flat_hash_map<int, int> &Stock)
{
    return box::erase_if(Stock, [](auto &&Item) { return Item.second == 0; });
}
```
