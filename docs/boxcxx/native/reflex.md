# box::reflex

## Назначение

Заголовок: [`box/cxx/reflex.h`](../../../src/userspace/boxcxx/include/box/cxx/reflex.h).
C-основа: `touch_claim(…, TOUCH_REACT, …)` в `box/touch.h`.

**Рефлекс** привязывает подготовленный манифест ([manifest](manifest.md))
к тегу Touch и просит ядро **выполнять** этот манифест в ядре при каждом
событии тега (режим `TOUCH_REACT`). Это замена обработчиков асинхронных
сигналов Unix: реакция — декларативный манифест, который выполняет ядро,
а не вызов кода программы посреди её работы. Нет кадра сигнала, нет
повторного входа на вашем стеке, нет минного поля «async-signal-safe».

`box::subscription` ([touch](touch.md)) — сторона «забираю сам»: событие
доставляется вам. `box::reflex` — сторона «ядро действует за меня».

На подготовленный манифест две независимые ссылки: `compiled_manifest`
держит ссылку программы, а ядро при заявке берёт свою. Деструктор сначала
снимает заявку (доставки прекращаются, ссылка ядра уходит), и только потом
уничтожается манифест — он не может исчезнуть, пока до него может дойти
доставка.

**Одна заявка на тег в кабине:** ядро хранит для кабины одну заявку на тег.
Рефлекс на тег, на который уже есть подписка, тихо переводит эту заявку в
режим REACT — подписка замолкает, а первый уничтоженный объект снимает
общую заявку из-под другого. Для одного тега в кабине выбирайте одно:
подписку или рефлекс.

## Класс reflex

### `box::reflex`

**Краткое описание:** Выполняет манифест в ядре на каждое событие тега, пока жив объект.

**Параметры:**
- `reflex()` — пустой.
- `reflex(const tag &on, compiled_manifest &&program)`: тег и подготовленный манифест (забирается).

**Возвращает:** Неприменимо: класс; только перемещение.

**Побочные эффекты:** Заявляет тег в режиме REACT; деструктор снимает заявку.

**Пример:**
```cpp
#include <array>
#include <cstring>

#include "box/cxx/reflex.h"   // SYSTEM_OP_TOUCH_SEND — из boxos_decks.h

// Каждое событие "sensor:spike" ядро пересылает на тег "alarm".
box::reflex Forward()
{
    using namespace box::literals;
    const box::tag To = "alarm"_tag;
    const TouchTagPair Pair = To.pair();
    std::array<std::byte, 12> Params{};
    const std::uint32_t Words[3] = {Pair.full, Pair.bare, 0};   // [полный][голый][отложить, мс]
    std::memcpy(Params.data(), Words, sizeof Words);
    box::manifest<> Program;
    Program.op(DECK_SYSTEM, SYSTEM_OP_TOUCH_SEND, box::no_crate, box::no_crate, Params);
    return box::reflex("sensor:spike"_tag, box::compiled_manifest(Program));
}
```

### `operator bool` / `bound` / `id` / `manifest`

**Краткое описание:** Сообщают, действует ли рефлекс, его тег и манифест.

**Параметры:** Нет.

**Возвращает:** `operator bool`, `bound` — (`bool`); `id` — (`TouchTag`); `manifest` — (`const compiled_manifest &`).

**Побочные эффекты:** Нет.

**Примечания:** Если рефлекс не действует, `manifest()` отличает неудачу компиляции (манифеста нет) от неудачи заявки (манифест есть, тега нет).

**Пример:**
```cpp
#include "box/cxx/reflex.h"

const char *Why(const box::reflex &R)
{
    if (R) return "работает";
    return R.manifest() ? "тег не заявлен" : "манифест не скомпилирован";
}
```
