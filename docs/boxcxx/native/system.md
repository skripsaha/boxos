# box::system

## Назначение

Заголовок: [`box/cxx/system.h`](../../../src/userspace/boxcxx/include/box/cxx/system.h).
C-основа: `box/system.h`.

Сведения о машине и прошивке, обслуживание и управление:

- `box::system_info` — снимок машины: имя, время работы, память, ядра, такт системы, возможности процессора;
- `box::system` — `info()`, загрузка ядер (`core_usage`), обслуживание (`perf_dump`, `fragmentation`, `defrag`), `reboot()` и `shutdown()`;
- `box::efi_status`, `box::efi` — состояние прошивки UEFI и Secure Boot, таблица ESRT, проверка подписи PE-образа.

Чтение безопасно где угодно; `reboot()` и `shutdown()` действуют на всю
машину.

## Снимок машины

### `box::system_info`

**Краткое описание:** Удобный вид снимка машины (`system_info_t`).

**Параметры:** Создаётся `system::info()`.

**Возвращает:** Неприменимо: класс. `raw()`; `name()` (`std::string_view`); `uptime()` (`std::chrono::nanoseconds`); `total_memory()`, `used_memory()`, `free_memory()`, `tsc_freq_khz()` (`std::uint64_t`); `cpu_total()`, `cpu_k_cores()` (ядра системы), `cpu_app_cores()` (ядра программ), `process_count()`, `tick_freq_hz()` (`std::uint32_t`); `tick_source()` (`std::uint8_t`, сравнивать с `SYSINFO_TICK_SOURCE_*` — источник такта путешествует вместе с частотой, потому что на многих платах такт даёт HPET, а не PIT); `multicore()`, `invariant_tsc()`, `waitpkg()` (`bool`).

**Побочные эффекты:** Нет.

**Пример:** См. `system::info`.

### `system::info`

**Краткое описание:** Возвращает снимок машины.

**Параметры:** Нет.

**Возвращает:** (`box::result<system_info>`).

**Побочные эффекты:** Системный вызов.

**Пример:**
```cpp
#include <chrono>
#include <print>

#include "box/cxx/system.h"

void About()
{
    if (auto Machine = box::system::info())
        std::println("{}: {} МиБ свободно, работает {} с, ядер программ {}", Machine->name(), Machine->free_memory() >> 20,
                     std::chrono::duration_cast<std::chrono::seconds>(Machine->uptime()).count(), Machine->cpu_app_cores());
}
```

### `system::core_usage` / `core_usage_snapshot`

**Краткое описание:** Возвращает накопленный учёт работы одного ядра.

**Параметры:** `core_index` (`std::uint32_t`).

**Возвращает:** (`box::result<core_usage_snapshot>`): `busy_tsc`, `total_tsc` (такты работы и всего), `parked` (ядро запарковано), `kernel` (K-Core — работа системы, а не программ).

**Побочные эффекты:** Системный вызов.

**Типичное применение:** Загрузка ядра за интервал — разность двух снимков.

**Пример:**
```cpp
#include <chrono>
#include <thread>

#include "box/cxx/system.h"

int LoadPercent(std::uint32_t Core)
{
    using namespace std::chrono_literals;
    auto A = box::system::core_usage(Core);
    std::this_thread::sleep_for(500ms);
    auto B = box::system::core_usage(Core);
    if (!A || !B || B->total_tsc == A->total_tsc) return 0;
    return static_cast<int>((B->busy_tsc - A->busy_tsc) * 100 / (B->total_tsc - A->total_tsc));
}
```

## Обслуживание

### `system::perf_dump` / `fragmentation` / `defrag`

**Краткое описание:** Сбрасывают счётчики производительности в журнал, сообщают фрагментацию тома и дефрагментируют файл.

**Параметры:** `file_id` (`file_id_t`) для `defrag`.

**Возвращает:** `perf_dump` — (`box::status`); `fragmentation` — (`box::result<int>`) процент шагов между блоками файлов, которые являются разрывом; `defrag` — (`box::result<int>`) оценка фрагментации после работы (ненулевая оценка — тоже успех).

**Побочные эффекты:** `perf_dump` пишет в журнал; `defrag` переносит блоки файла в один участок, выбранный томом.

**Пример:**
```cpp
#include "box/cxx/system.h"

int Fragmented() { return box::system::fragmentation().value_or(-1); }
```

## Управление

### `system::reboot` / `system::shutdown`

**Краткое описание:** Перезагружают или выключают машину.

**Параметры:** Нет.

**Возвращает:** (`box::status`) — **только при неудаче** (при успехе управление не возвращается); ошибка называет причину (например, `internal` — нет пути ACPI).

**Побочные эффекты:** Действуют на всю машину.

**Пример:**
```cpp
#include <print>

#include "box/cxx/system.h"

void Bye()
{
    if (auto Failed = box::system::shutdown(); !Failed) std::println("не выключилась: {}", Failed.error());
}
```

## Прошивка

### `box::efi_status`

**Краткое описание:** Состояние прошивки и Secure Boot (`efi_info_t`).

**Параметры:** Создаётся `efi::info()`.

**Возвращает:** Неприменимо: класс. `raw()`; `runtime_available()`, `esrt_available()`, `secure_boot_available()`, `secure_boot_enforced()`, `setup_mode()`, `audit_mode()`, `deployed_mode()` (`bool`); `esrt_count()`, `cert_count()`, `hash_count()` (`std::uint32_t`).

**Побочные эффекты:** Нет.

**Пример:** См. `efi::info`.

### `efi::info` / `efi::esrt_entry` / `efi::esrt`

**Краткое описание:** Возвращают состояние прошивки, одну запись ESRT и всю таблицу.

**Параметры:** `idx` (`std::uint32_t`) для `esrt_entry`, от 0 до `esrt_count()`.

**Возвращает:** `info` — (`box::result<efi_status>`); `esrt_entry` — (`box::result<efi_esrt_entry_t>`); `esrt` — (`std::vector<efi_esrt_entry_t>`), пусто, если таблица не опубликована (это нормальный ответ).

**Побочные эффекты:** Системные вызовы.

**Пример:**
```cpp
#include "box/cxx/system.h"

bool Locked() { auto Firmware = box::efi::info(); return Firmware && Firmware->secure_boot_enforced(); }
```

### `efi::verify_pe` / `efi::verified`

**Краткое описание:** Проверяют подпись Authenticode PE-образа по базам прошивки db/dbx.

**Параметры:** `pe` (`std::span<const std::byte>`): образ.

**Возвращает:** `verify_pe` — (`efi_verify_t`): вердикт в `.result` (`EFI_VERIFY_OK` — доверен) и вычисленные SHA-256 образа и подписанта; по умолчанию `EFI_VERIFY_SB_UNAVAILABLE`, чтобы невыполненная проверка не читалась как «доверен». `verified` — (`bool`) доверен ли образ.

**Побочные эффекты:** Системный вызов.

**Пример:**
```cpp
#include "box/cxx/system.h"

bool Trusted(std::span<const std::byte> Image) { return box::efi::verified(Image); }
```
