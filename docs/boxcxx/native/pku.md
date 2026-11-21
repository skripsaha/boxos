# box::pku

## Назначение

Заголовок: [`box/cxx/pku.h`](../../../src/userspace/boxcxx/include/box/cxx/pku.h).
C-основа: `box/pku.h`.

Ключи защиты памяти Intel (PKU). Каждая конечная запись таблицы страниц
несёт 4-битный ключ; регистр PKRU своей нити держит для каждого ключа пару
«запрет доступа / запрет записи», и программа переключает их из ring 3 без
системного вызова. BoxOS привязывает ключ к страницам области через
MemTag ([memtag](memtag.md)).

- `box::pku` — сырое управление: `available`, `read`, `write`, `set_rights`, `get_rights`, `apply_region`;
- `box::protection_key` — дескриптор одного из 16 ключей;
- `box::access_window` — открыть ключ на область видимости и вернуть прежние права (окно «показать» для W^X);
- `box::sealed_region<T>` — запечатанная ключом область памяти, открываемая по требованию.

Без PKU всё честно отключается: операции прав ничего не делают,
`sealed_region` не привязывается. Регистр PKRU работает на любом
процессоре с PKU, но сама авария при доступе к запечатанной области
требует аппаратного принуждения (эмулятор может объявить PKU и не
падать).

## pku

### `pku::available`

**Краткое описание:** Сообщает, включены ли ключи защиты (CR4.PKE) на всех ядрах.

**Параметры:** Нет.

**Возвращает:** (`bool`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/pku.h"

bool Protected() { return box::pku::available(); }
```

### `pku::read` / `pku::write` / `pku::max_keys`

**Краткое описание:** Читают и записывают весь регистр PKRU (по 2 бита на ключ).

**Параметры:** `value` (`std::uint32_t`) для `write`.

**Возвращает:** `read` — (`std::uint32_t`); `write` — `void`; `max_keys` = 16.

**Побочные эффекты:** `write` меняет права текущей нити.

**Пример:**
```cpp
#include "box/cxx/pku.h"

std::uint32_t Snapshot() { return box::pku::read(); }
```

### `pku::rights` / `pku::set_rights` / `pku::get_rights`

**Краткое описание:** Задают и читают права одного ключа.

**Параметры:** `key` (`std::uint8_t`), `access_disable`, `write_disable` (`bool`).

**Возвращает:** `set_rights` — (`bool`) `false` без PKU или при ключе вне диапазона; `get_rights` — (`rights{access_disabled, write_disabled}`): первое запрещает любой доступ, второе — только запись.

**Побочные эффекты:** Меняют PKRU нити.

**Пример:**
```cpp
#include "box/cxx/pku.h"

bool ReadOnly(std::uint8_t Key) { return box::pku::set_rights(Key, false, true); }
```

### `pku::apply_region`

**Краткое описание:** Ставит ключ `pku:<key>` на страницы области во всех отображениях.

**Параметры:** `region_id` (`std::uint32_t`) — область, которой владеет вызывающий; `key` (`std::uint8_t`), 0 снимает политику.

**Возвращает:** (`bool`).

**Побочные эффекты:** Меняет записи таблиц страниц.

**Пример:** См. `sealed_region`.

## protection_key

### `box::protection_key`

**Краткое описание:** Дескриптор одного аппаратного ключа.

**Параметры:** `protection_key(std::uint8_t key)` (`explicit`, `constexpr`).

**Возвращает:** Неприменимо: класс. `value()`; `unlock()` — полный доступ; `read_only()` — только чтение; `lock()` — без доступа; `rights()`.

**Побочные эффекты:** Методы меняют PKRU нити.

**Пример:**
```cpp
#include "box/cxx/pku.h"

void Freeze() { (void)box::protection_key(3).read_only(); }
```

## access_window

### `box::access_window`

**Краткое описание:** Открывает ключ на время области видимости и восстанавливает его права на выходе.

**Параметры:** `access_window(protection_key k, bool write = true)` или `access_window(std::uint8_t key, bool write = true)` (`explicit`): `write == false` — только чтение.

**Возвращает:** Неприменимо: класс; перемещающий конструктор есть, копирования и присваивания нет.

**Побочные эффекты:** Деструктор возвращает ровно два бита этого ключа к прежнему значению, не трогая другие ключи.

**Пример:** См. `sealed_region`.

## sealed_region

### `box::sealed_region<T>`

**Краткое описание:** Защищённая ключом область памяти, запечатанная по умолчанию.

**Параметры:** `sealed_region(std::span<T> mem, std::uint8_t key)`: память должна принадлежать области MemTag (обычно `box::bay<T>`, [bay](bay.md)); область находится по адресу.

**Возвращает:** Неприменимо: класс; копировать нельзя. `bound()` — привязка удалась (PKU есть, область найдена, ключ поставлен); `region()`; `key()`; `data()`.

**Побочные эффекты:** Ставит ключ на страницы и запирает доступ; деструктор снимает ключ с области.

**Пример:**
```cpp
#include "box/cxx/bay.h"
#include "box/cxx/pku.h"

struct Secret {
    std::uint8_t Bytes[64];
};

void Guarded()
{
    auto Store = box::bay<Secret>::create("demo:secret", 1);
    box::sealed_region<Secret> Seal(Store.as_span(), 5);
    if (!Seal.bound()) return;
    {
        auto Window = Seal.reveal();   // доступ открыт только в этой области
        Seal.data()[0].Bytes[0] = 42;
    }                                   // снова запечатано
}
```

### `seal` / `seal_read_only` / `reveal`

**Краткое описание:** Запечатывают полностью, оставляют только чтение и открывают окно доступа.

**Параметры:** `write` (`bool`) для `reveal`, по умолчанию `true`.

**Возвращает:** `seal`, `seal_read_only` — `void`; `reveal` — (`access_window`, `[[nodiscard]]`).

**Побочные эффекты:** Меняют PKRU нити.

**Пример:** См. `sealed_region`.
