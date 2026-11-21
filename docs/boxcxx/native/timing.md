# box::timing

## Назначение

Заголовок: [`box/cxx/timing.h`](../../../src/userspace/boxcxx/include/box/cxx/timing.h).

Помощники для времени поверх `std::chrono::steady_clock` (монотонные
часы от загрузки): «сколько это заняло», «можно ли уже действовать»,
«сколько спать до следующего кадра». Это не счётчик тактов — тот в
[cpu](cpu.md) (`box::cpu::tsc_now()`).

## stopwatch

### `box::stopwatch`

**Краткое описание:** Измеряет прошедшее время с создания или последнего сброса.

**Параметры:** Конструктор по умолчанию — отсчёт с этого момента.

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Нет.

**Пример:** См. `elapsed`.

### `elapsed` / `elapsed_as` / `reset`

**Краткое описание:** Возвращают прошедшее время, его же в нужных единицах, и перезапускают отсчёт.

**Параметры:** `Duration` — шаблонный параметр `elapsed_as`.

**Возвращает:** `elapsed` — (`std::chrono::nanoseconds`); `elapsed_as<D>` — (`D`); `reset` — (`std::chrono::nanoseconds`) время круга до сброса.

**Побочные эффекты:** `reset` начинает новый отсчёт.

**Пример:**
```cpp
#include <chrono>
#include <print>

#include "box/cxx/timing.h"

void Measure()
{
    box::stopwatch Watch;
    volatile long Sum = 0;
    for (long I = 0; I < 1000000; I++) Sum += I;
    std::println("{} мкс", Watch.elapsed_as<std::chrono::microseconds>().count());
}
```

## throttle

### `box::throttle`

**Краткое описание:** Ограничитель частоты: срабатывает не чаще раза в интервал.

**Параметры:** `throttle(std::chrono::steady_clock::duration interval)` (`explicit`).

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Нет.

**Пример:** См. `try_fire`.

### `try_fire` / `ready` / `remaining` / `reset`

**Краткое описание:** Пробуют сработать, проверяют готовность, сообщают остаток и взводят заново.

**Параметры:** Нет.

**Возвращает:** `try_fire` — (`bool`) прошёл интервал с прошлого срабатывания (первый вызов срабатывает всегда), окно сдвигается; `ready` — (`bool`) без сдвига; `remaining` — (`std::chrono::nanoseconds`) до готовности, 0 — готов; `reset` — `void`, следующий `try_fire` сработает сразу.

**Побочные эффекты:** `try_fire` при успехе сдвигает окно.

**Пример:**
```cpp
#include <chrono>
#include <print>

#include "box/cxx/timing.h"

void Progress(int Percent)
{
    using namespace std::chrono_literals;
    static box::throttle Often(250ms);
    if (Often.try_fire()) std::println("{}%", Percent);
}
```

## pacer

### `box::pacer`

**Краткое описание:** Держит ровную частоту кадров, отсчитывая каждый кадр от начала предыдущего.

**Параметры:** Конструктор по умолчанию.

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Нет.

**Пример:** См. `pacer::wait`.

### `pacer::interval` / `pacer::wait`

**Краткое описание:** Возвращают длительность кадра для частоты и сколько спать до следующего кадра.

**Параметры:**
- `interval(unsigned frames_per_second)` — статическая; 0 считается за 1.
- `wait(std::chrono::microseconds interval)` — вызывать раз за кадр, после рисования.

**Возвращает:** (`std::chrono::microseconds`). Если кадр опоздал — 0, и расписание начинается заново от текущего момента (пропущенные кадры не догоняются очередью).

**Побочные эффекты:** `wait` сдвигает расписание.

**Пример:**
```cpp
#include <thread>

#include "box/cxx/timing.h"

void Animate()
{
    box::pacer Frames;
    const auto Step = box::pacer::interval(60);
    for (int Frame = 0; Frame < 600; Frame++) {
        // ... нарисовать кадр
        std::this_thread::sleep_for(Frames.wait(Step));
    }
}
```
