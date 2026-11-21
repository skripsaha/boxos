# box::cpu

## Назначение

Заголовок: [`box/cxx/cpu.h`](../../../src/userspace/boxcxx/include/box/cxx/cpu.h).
C-основа: `box/cpu.h`.

Возможности процессора, счётчик тактов, аппаратная энтропия и аппаратное
ожидание записи в память. Всё читается со страницы `cpu_caps`, которую
публикует ядро, — пересечения возможностей всех ядер: без системного
вызова, без `CPUID` в программе и без сюрпризов на ядре, где возможности
нет.

Всё честно отключается, если возможности нет: проверки отвечают `false`,
`monitor_wait()` возвращает `wake::unavailable` без ожидания,
`hardware_entropy` сообщает `engaged() == false`.

## Возможности

### `cpu::has_waitpkg` / `has_invariant_tsc` / `has_pku` / `has_pks` / `has_lam` / `has_cet` / `has_tme` / `has_fsgsbase` / `has_rdrand` / `has_rdseed`

**Краткое описание:** Сообщают, доступна ли возможность процессора на любом ядре, где может работать кабина.

**Параметры:** Нет.

**Возвращает:** (`bool`).

**Побочные эффекты:** Нет.

**Как работает:** `true`, только если возможность есть на всех ядрах (UMWAIT/UMONITOR, инвариантный TSC, ключи защиты памяти PKU/PKS, LAM, CET, TME, FSGSBASE, RDRAND, RDSEED).

**Пример:**
```cpp
#include "box/cxx/cpu.h"

bool CanIsolate() { return box::cpu::has_pku(); }
```

## Такты

### `cpu::tsc_freq_khz` / `tsc_now` / `tsc_end` / `tsc_to_ns` / `ms_to_tsc`

**Краткое описание:** Дают частоту и показания счётчика тактов и переводы в наносекунды и обратно.

**Параметры:** `ticks`, `ms` (`std::uint64_t`).

**Возвращает:** (`std::uint64_t`). `tsc_now` — `lfence; rdtsc` (начало окна), `tsc_end` — `rdtsc; lfence` (конец окна); `tsc_to_ns` — 0 без калиброванной частоты.

**Побочные эффекты:** Нет.

**Примечания:** Для замеров, а не для настенного времени (для него — `<chrono>`). `tsc_to_ns` осмыслен только при `has_invariant_tsc()`.

**Пример:**
```cpp
#include "box/cxx/cpu.h"

std::uint64_t Nanoseconds(void (*Work)())
{
    const std::uint64_t Start = box::cpu::tsc_now();
    Work();
    return box::cpu::tsc_to_ns(box::cpu::tsc_end() - Start);
}
```

## Аппаратное ожидание

### `cpu::wake`

**Краткое описание:** Называет, почему вернулось `monitor_wait`.

**Параметры:**
- `written` — запись в отслеживаемую линию или прерывание.
- `deadline` — истёк срок TSC или предел ОС.
- `unavailable` — WAITPKG нет, ожидания не было.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Пример:** См. `monitor_wait`.

### `cpu::monitor_supported` / `cpu::monitor_wait`

**Краткое описание:** Сообщают, есть ли UMWAIT, и усыпляют ядро до записи в кэш-линию адреса или до срока.

**Параметры:**
- `addr` (`const volatile void *`): отслеживаемый адрес.
- `deadline` (`std::uint64_t`): абсолютный срок в тактах (`tsc_now() + ms_to_tsc(…)`).
- `state` (`unsigned`): 0 — C0.2 (глубже, медленнее просыпаться), 1 — C0.1.

**Возвращает:** `monitor_supported` — (`bool`); `monitor_wait` — (`wake`).

**Побочные эффекты:** Ядро процессора переходит в состояние низкого потребления.

**Примечания:** Запись между последней проверкой условия и постановкой монитора не теряется навсегда, но может задержаться до срока — поэтому цикл «проверить → ждать → проверить снова». Перепроверяйте условие после каждого пробуждения.

**Пример:**
```cpp
#include <atomic>

#include "box/cxx/cpu.h"

void WaitFlag(const std::atomic<int> &Flag)
{
    while (Flag.load() == 0)
        if (box::cpu::monitor_wait(&Flag, box::cpu::tsc_now() + box::cpu::ms_to_tsc(10)) == box::cpu::wake::unavailable) break;
}
```

## Энтропия

### `box::hardware_entropy`

**Краткое описание:** Генератор случайных бит из энтропии процессора для `<random>`.

**Параметры:** Конструктор по умолчанию; копировать нельзя.

**Возвращает:** Неприменимо: класс, удовлетворяет `uniform_random_bit_generator` (`result_type` = `std::uint64_t`, `min()`, `max()`, `operator()`).

**Побочные эффекты:** Нет.

**Как работает:** Предпочитает RDSEED (источник энтропии NIST SP 800-90B), затем RDRAND (DRBG SP 800-90A). 64 бита за раз. Без обоих — некриптографическая смесь TSC (splitmix64): генератор всегда отвечает, но `engaged()` = `false`, `entropy()` = 0.

**Пример:**
```cpp
#include <random>

#include "box/cxx/cpu.h"

int Dice()
{
    box::hardware_entropy Seed;
    std::mt19937_64 Engine(Seed());
    return std::uniform_int_distribution<int>(1, 6)(Engine);
}
```

### `active_source` / `engaged` / `entropy`

**Краткое описание:** Сообщают источник, есть ли настоящий источник и оценку энтропии на выборку.

**Параметры:** Нет.

**Возвращает:** `active_source` — (`source`: `seed`, `rand`, `none`); `engaged` — (`bool`); `entropy` — (`double`) 64 или 0.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/cpu.h"

bool Secure() { return box::hardware_entropy::engaged(); }
```
