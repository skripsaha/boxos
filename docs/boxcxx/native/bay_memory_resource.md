# box::bay_memory_resource

## Назначение

Заголовок: [`box/cxx/bay_memory_resource.h`](../../../src/userspace/boxcxx/include/box/cxx/bay_memory_resource.h).

`std::pmr::memory_resource` поверх Bay ([bay](bay.md)) — общей области
памяти с именем-тегом. Выделение — монотонное (сдвигом указателя) внутри
Bay, поэтому контейнеры `std::pmr` можно строить прямо в общей памяти.
Освобождение ничего не делает; чтобы отпустить заявку на Bay, уничтожьте
весь ресурс.

## Класс bay_memory_resource

### `bay_memory_resource`

**Краткое описание:** Открывает или создаёт Bay и выделяет из него память.

**Параметры:**
- `tag` (`const char *`).
- `bytes` (`std::uint64_t`): размер для создания; при открытии берётся размер существующего Bay.
- `flags` (`std::uint32_t`): `BAY_CREATE` (по умолчанию) или `BAY_OPEN`.

**Возвращает:** Неприменимо: класс; копировать нельзя.

**Побочные эффекты:** Отображает Bay; деструктор снимает заявку.

**Примечания:** Контейнеры хранят внутри себя абсолютные указатели. Другая кабина видит те же байты, но может отобразить Bay по другому адресу — тогда указатели внутри контейнеров для неё недействительны. Отображается ли Bay во всех кабинах по одному адресу, требует проверки в исходном коде ядра. Для обмена между кабинами надёжнее данные без указателей (`box::bay<T>`).

**Пример:**
```cpp
#include <memory_resource>
#include <vector>

#include "box/cxx/bay_memory_resource.h"

std::size_t Build()
{
    box::bay_memory_resource Shared("demo:pmr", 1u << 16);
    std::pmr::vector<int> Numbers(&Shared);
    for (int I = 0; I < 100; I++) Numbers.push_back(I);
    return Shared.used();
}
```

### `data` / `size` / `used` / `reset`

**Краткое описание:** Возвращают начало и размер области, сколько выделено, и сбрасывают выделение.

**Параметры:** Нет.

**Возвращает:** `data` — (`void *`), `nullptr`, если Bay не открылся; `size`, `used` — (`std::size_t`); `reset` — `void`.

**Побочные эффекты:** `reset` начинает выделять с начала заново — всё, что было выделено, считается свободным.

**Как работает:** `do_allocate` выравнивает адрес и сдвигает отметку; при нехватке места или неоткрытом Bay бросает `std::bad_alloc`.

**Пример:**
```cpp
#include "box/cxx/bay_memory_resource.h"

bool Full(const box::bay_memory_resource &Shared) { return Shared.used() == Shared.size(); }
```
