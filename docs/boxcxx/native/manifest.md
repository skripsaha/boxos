# box::manifest

## Назначение

Заголовок: [`box/cxx/manifest.h`](../../../src/userspace/boxcxx/include/box/cxx/manifest.h).
C-основа: `box/core/manifest.h`, `box/core/crate.h`, номера колод — `boxos_decks.h`.

Низкоуровневое C++-лицо системного вызова BoxOS. Каждый системный вызов —
**манифест**: упорядоченный список операций (колода + код операции +
встроенные параметры) и таблица **ящиков** (Crate) — описателей полезной
нагрузки переменного размера, указывающих в кучу кабины. Все
типизированные слои `box::` (`tagfs`, `message`, `bay`…) построены на
этом; заголовок нужен коду, которому требуется прямой доступ к колодам и
кодам операций или несколько операций в одном вызове.

Ящики — **невладеющие** виды: буферы должны жить, пока идёт отправка (ядро
читает и пишет их на месте).

Колоды: `DECK_EXECUTION` (0x00), `DECK_OPERATIONS` (0x01), `DECK_STORAGE`
(0x02), `DECK_HARDWARE` (0x03), `DECK_NETWORK` (0x04), `DECK_SYSTEM` (0xFF);
коды операций — в заголовках `boxos_*.h` соответствующих подсистем.

## Ящик

### `box::crate` / `box::no_crate`

**Краткое описание:** Описатель полезной нагрузки поверх памяти вызывающего.

**Параметры:**
- `crate::input(std::span<const std::byte>)` — операция читает (ядро не меняет байты).
- `crate::output(std::span<std::byte>)` — операция пишет до размера буфера.
- `crate::in_out(std::span<std::byte>, std::uint64_t valid)` — читает `valid` байт и пишет на месте.
- `crate::input_object<T>(const T &)`, `crate::output_object<T>(T &)` — для тривиально копируемых `T`.
- `no_crate` — «у операции нет ящика».

**Возвращает:** Неприменимо: класс. `raw()`, `kind()`, `size()` (действительные / записанные байты), `capacity()`, `produced()` — что операция записала (после отправки).

**Побочные эффекты:** Нет.

**Пример:** См. `box::manifest`.

## Манифест

### `box::manifest<BufBytes, MaxCrates>`

**Краткое описание:** Построитель манифеста фиксированной ёмкости на несколько операций.

**Параметры:** `BufBytes` (по умолчанию 256) — буфер манифеста; `MaxCrates` (по умолчанию 8) — ящиков.

**Возвращает:** Неприменимо: класс; копировать нельзя. `valid()` / `operator bool` — построение не испорчено.

**Побочные эффекты:** Нет до `submit`.

**Пример:**
```cpp
#include <span>

#include "box/cxx/manifest.h"
#include "box/system.h"   // system_info_t

bool Info(system_info_t &Out)
{
    box::manifest<> Call;
    const std::uint16_t Answer = Call.add(box::crate::output_object(Out));
    Call.op(DECK_SYSTEM, SYSTEM_OP_INFO, box::no_crate, Answer);
    return static_cast<bool>(Call.submit());
}
```

### `manifest::add` / `manifest::op`

**Краткое описание:** Регистрирует ящик и добавляет операцию.

**Параметры:**
- `add(const crate &)` — возвращает индекс ящика.
- `op(deck, opcode, in_crate = no_crate, out_crate = no_crate, params = {}, flags = 0)`: `params` — встроенные параметры не больше 65535 байт.

**Возвращает:** `add` — (`std::uint16_t`) индекс, `no_crate` при переполнении; `op` — (`manifest &`) для цепочки.

**Побочные эффекты:** Ошибка (переполнение, слишком большие параметры) помечает манифест недействительным, а не обрезает данные.

**Пример:** См. `box::manifest`.

### `manifest::submit` / `box::mf_outcome`

**Краткое описание:** Завершает построение и отправляет манифест.

**Параметры:**
- `target_pid` (`std::uint32_t`): 0 — себе (ядру от своего имени).
- `timeout_ms` (`std::uint32_t`): 0 — по умолчанию.

**Возвращает:** (`mf_outcome`): `rc` — 0 при успехе, иначе код ядра или −1 (испорченное построение); `result` — запись ядра; `operator bool` — успех.

**Побочные эффекты:** Системный вызов; ядро пишет в ящики вывода.

**Пример:** См. `box::manifest`.

### `crate_at` / `crate_count` / `crates` / `raw` / `finalize`

**Краткое описание:** Дают доступ к ящикам после отправки, к сырому манифесту и завершают построение без отправки.

**Параметры:** `idx` (`std::uint16_t`) для `crate_at`.

**Возвращает:** `crate_at` — (`crate`); `crate_count` — (`std::uint16_t`); `crates` — (`Crate *`); `raw` — (`const Manifest *`); `finalize` — (`bool`).

**Побочные эффекты:** `finalize` завершает манифест (повторно — ничего).

**Пример:**
```cpp
#include "box/cxx/manifest.h"

std::size_t Got(box::manifest<> &Call, std::uint16_t Out) { return Call.crate_at(Out).produced().size(); }
```

## Подготовленный манифест

### `box::compiled_manifest`

**Краткое описание:** Скомпилированный один раз манифест для многократной отправки.

**Параметры:** `compiled_manifest(manifest<B, M> &mf)` (`explicit`): завершает и компилирует.

**Возвращает:** Неприменимо: класс; только перемещение. `operator bool`, `handle()`.

**Побочные эффекты:** Ядро хранит подготовленный манифест; деструктор освобождает его.

**Как работает:** Каждая отправка пропускает проверку манифеста и поиск операций в реестре ядра. Структура операций — закэшированная; данные меняются через буферы ящиков между отправками.

**Пример:**
```cpp
#include "box/cxx/manifest.h"
#include "box/system.h"   // system_info_t

int Sample(int Times)
{
    system_info_t Out{};
    box::manifest<> Call;
    Call.op(DECK_SYSTEM, SYSTEM_OP_INFO, box::no_crate, Call.add(box::crate::output_object(Out)));
    box::compiled_manifest Prepared(Call);
    int Done = 0;
    for (int I = 0; I < Times && Prepared; I++) Done += static_cast<bool>(Prepared.submit(Call));
    return Done;
}
```

### `compiled_manifest::submit`

**Краткое описание:** Отправляет подготовленный манифест.

**Параметры:** `mf` (`manifest<B, M> &`) — его таблица ящиков; или `crates` (`Crate *`), `crate_count`; `timeout_ms`.

**Возвращает:** (`mf_outcome`); у недействительного — `rc == -1`.

**Побочные эффекты:** Системный вызов.

**Пример:** См. `box::compiled_manifest`.

## Одна операция

### `box::mf_call1` / `box::mf_call_result`

**Краткое описание:** Выполняет одну операцию одним вызовом.

**Параметры:** `deck`, `opcode` (`std::uint16_t`); `params`, `in` (`std::span<const std::byte>`), `out` (`std::span<std::byte>`); `timeout_ms`.

**Возвращает:** (`mf_call_result`): `rc` (0 — успех; −1 — размеры не помещаются в поля ABI), `produced` — байты, записанные в `out`; `operator bool`.

**Побочные эффекты:** Системный вызов.

**Пример:**
```cpp
#include <span>

#include "box/cxx/manifest.h"
#include "box/system.h"   // system_info_t

bool Quick(system_info_t &Out)
{
    return static_cast<bool>(box::mf_call1(DECK_SYSTEM, SYSTEM_OP_INFO, {}, {}, std::as_writable_bytes(std::span(&Out, 1))));
}
```
