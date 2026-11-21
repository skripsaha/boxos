# box::hw

## Назначение

Заголовок: [`box/cxx/hw.h`](../../../src/userspace/boxcxx/include/box/cxx/hw.h).
C-основа: `box/hw.h`.

Аппаратные рычаги процессора для процесса:

- `box::hw` — маскирование линейных адресов LAM (`lam`, `set_lam`) и состояние шифрования памяти TME/TME-MK (`tme`, `encrypted`, `keyid_of`);
- `box::tagged_pointer<T>` — указатель с 7-битной меткой в битах 62:56, которые процессор игнорирует при включённом LAM-U48.

Чтение безопасно где угодно. Разыменование помеченного указателя без
LAM-U48 даёт неканонический адрес и аварию — это свойство железа.

## LAM

### `hw::lam_mode`

**Краткое описание:** Называет режим маскирования адресов.

**Параметры:**
- `none` — обычные 48-битные адреса (по умолчанию).
- `u48` — биты 62:48 пользовательских адресов игнорируются (BoxOS использует 62:56 под метки).
- `u57` — требует 5-уровневой страничной адресации, которой BoxOS не включает.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Пример:** См. `hw::set_lam`.

### `hw::lam_available` / `hw::tme_available`

**Краткое описание:** Сообщают, есть ли у процессора LAM и у платформы TME.

**Параметры:** Нет.

**Возвращает:** (`bool`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/hw.h"

bool CanTag() { return box::hw::lam_available(); }
```

### `hw::lam` / `hw::set_lam`

**Краткое описание:** Возвращают и задают режим LAM текущего процесса.

**Параметры:** `mode` (`lam_mode`) для `set_lam`.

**Возвращает:** `lam` — (`lam_mode`), `none`, если не задан или не читается; `set_lam` — (`std::expected<void, int>`), ошибка — код boxlib `-ERR_*` (`u57` — всегда, `u48` без LAM).

**Побочные эффекты:** Новый режим вступает в силу со следующей перезагрузкой CR3.

**Пример:**
```cpp
#include "box/cxx/hw.h"

bool EnableTags() { return box::hw::set_lam(box::hw::lam_mode::u48).has_value(); }
```

## TME

### `hw::tme_state` / `hw::tme`

**Краткое описание:** Снимок состояния шифрования памяти платформы.

**Параметры:** Нет.

**Возвращает:** `tme()` — (`std::optional<tme_state>`), пусто при ошибке. `tme_state`: `active()`, `mk_active()` (многоключевой режим), `keyid_bits()`, `algorithm()`, `max_keyid()`, `pool_programmed()`, `in_use()`, `per_proc_quota()`, `reduced_maxphyaddr()`, `this_proc_held()`, `raw()`; есть `std::formatter`.

**Побочные эффекты:** Системный вызов.

**Пример:**
```cpp
#include <print>

#include "box/cxx/hw.h"

void Show() { if (auto State = box::hw::tme()) std::println("{}", *State); }
```

### `hw::encrypted` / `hw::keyid_of`

**Краткое описание:** Сообщают, шифруется ли память, и ключ TME-MK физического адреса.

**Параметры:** Перегрузки с готовым снимком (`const tme_state &`) и без него (снимок берётся сам); `phys` (`std::uint64_t`) для `keyid_of`.

**Возвращает:** `encrypted` — (`bool`) TME включён (простой TME шифрует всю память; при TME-MK ключ лишь выбирает, каким ключом); `keyid_of` — (`std::uint16_t`) биты адреса `[reduced_maxphyaddr, +keyid_bits)`, 0 при простом TME, без TME или при некорректном снимке.

**Побочные эффекты:** Перегрузки без снимка делают системный вызов.

**Примечания:** Производный ответ из глобального снимка, а не факт ядра о конкретной области.

**Пример:**
```cpp
#include "box/cxx/hw.h"

bool Encrypted() { return box::hw::encrypted(); }
```

## Помеченный указатель

### `box::tagged_pointer<T>`

**Краткое описание:** Указатель с 7-битной меткой в битах 62:56.

**Параметры:** `tagged_pointer()`; `tagged_pointer(T *p, std::uint8_t tag)` — метка обрезается до 7 бит.

**Возвращает:** Неприменимо: класс. `get()` — помеченный указатель (разыменовывать только при LAM-U48); `untagged()` — канонический, всегда безопасен; `tag()`; `value()`; `retag(tag)`; `operator bool` (по `untagged()`); `operator*`, `operator->` — требуют LAM-U48; `max_tag` = 0x7F.

**Побочные эффекты:** Нет (чистая битовая арифметика).

**Пример:**
```cpp
#include "box/cxx/hw.h"

struct Node {
    int Value;
};

int Read(Node *P)
{
    box::tagged_pointer<Node> Marked(P, 5);
    return Marked.tag() == 5 ? Marked.untagged()->Value : 0;   // untagged — безопасно без LAM
}
```
