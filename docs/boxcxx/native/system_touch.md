# box::system_touch

## Назначение

Заголовок: [`box/cxx/system_touch.h`](../../../src/userspace/boxcxx/include/box/cxx/system_touch.h).
C-основа: раскладки нагрузок в `box/touch.h`.

Ядро сообщает о жизни всей машины событиями Touch на шести известных
тегах: процесс родился или умер, просьба выключить или перезагрузить,
изменение порта USB. Этот заголовок — их типизированное лицо; сосед
[strand_watch](strand.md), который следит за нитями.

Фильтра нет: у шести тегов нет общего ключа (рождение — по родителю,
смерть — по pid, выключение и USB — ни по чему), поэтому разбирайте вид и
pid сами. Наблюдатель принадлежит одной нити; заявите его **до** события,
которое хотите увидеть.

Не держите `system_watch` в нити, которая надзирает за детьми через
[child](child.md): они будут бороться за подписку `process:died`.

## Нагрузки

### `box::process_died` / `process_spawned` / `system_halt` / `usb_device`

**Краткое описание:** Раскладки нагрузок системных событий (псевдонимы C-структур `box/touch.h`).

**Параметры:**
- `process_died` (84 байта): `pid`, `exit_code`, `generation` и как процесс закончился.
- `process_spawned` (20 байт): `pid`, `parent_pid` и поколения.
- `system_halt` (8 байт): `reason`, `grace_ms` — причина и время на завершение.
- `usb_device` (8 байт): `port`, `speed`, `vendor_id`, `product_id` (и подключение, и отключение).

**Возвращает:** Неприменимо: структуры.

**Побочные эффекты:** Нет.

**Пример:** См. `system_watch`.

### `box::system_touch` / `system_touch_kind`

**Краткое описание:** Одно разобранное системное событие.

**Параметры:**
- `kind` (`system_touch_kind`: `process_died`, `process_spawned`, `shutdown`, `reboot`, `usb_connect`, `usb_disconnect`).
- `source` — pid отправителя (0 — ядро); объединение `died`, `spawned`, `halt`, `usb`.
- `pid()` — о каком процессе событие (0 для выключения и USB).

**Возвращает:** Неприменимо: структура.

**Побочные эффекты:** Нет.

**Пример:** См. `system_watch`.

## Наблюдатель

### `box::system_watch`

**Краткое описание:** Держит подписки на все шесть системных тегов.

**Параметры:** Конструктор по умолчанию.

**Возвращает:** Неприменимо: класс; только перемещение. `operator bool` — все шесть подписок удались.

**Побочные эффекты:** Шесть заявок Touch.

**Пример:**
```cpp
#include <print>

#include "box/cxx/system_touch.h"

void Bridge()
{
    box::system_watch Watch;
    for (;;) {
        auto Event = Watch.wait();
        if (!Event) continue;
        switch (Event->kind) {
        case box::system_touch_kind::process_spawned: std::println("родился {}", Event->pid()); break;
        case box::system_touch_kind::process_died: std::println("умер {}", Event->pid()); break;
        case box::system_touch_kind::shutdown: std::println("выключение через {} мс", std::uint32_t{Event->halt.grace_ms}); return;
        default: break;
        }
    }
}
```

### `system_watch::poll` / `system_watch::wait`

**Краткое описание:** Забирают следующее событие без ожидания или ждут его.

**Параметры:** `ms` (`std::uint32_t`) для `wait`, 0 — без предела.

**Возвращает:** (`std::optional<system_touch>`).

**Побочные эффекты:** Забирают события; `wait` блокирует.

**Как работает:** `poll` проверяет теги в постоянном порядке (смерть, рождение, выключение, перезагрузка, подключение, отключение USB). `wait` по очереди ждёт каждый тег отрезками по 16 мс: задержка доставки — до 6 × 16 мс, срок может быть превышен немного. Для одного вида событий с малой задержкой используйте одну подписку ([touch](touch.md)).

**Пример:** См. `system_watch`.
