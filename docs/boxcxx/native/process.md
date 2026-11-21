# box::process

## Назначение

Заголовок: [`box/cxx/process.h`](../../../src/userspace/boxcxx/include/box/cxx/process.h).
C-основа: `box/system.h`, `box/core/cabin.h`.

Процессы и кабины. В BoxOS «запустить программу» значит создать новую
**кабину** — отдельное адресное пространство со своим pid, — а не поток.
Потоки внутри одной кабины — это нити ([strand](strand.md)).

Здесь:

- `box::this_process` — кто я (pid, кто меня запустил) и мои теги процесса;
- `box::cabin` — устройство адресного пространства своей кабины;
- `box::process` — дескриптор процесса по pid: запуск, сведения, жив ли;
- `box::tag_scope` — тег процесса на время области видимости.

**Теги процесса** определяют, кому доходят широковещательные сообщения:
`box::broadcast(tag, …)` ([message](message.md)) получает каждый процесс,
который носит тег. Теги ставит только сам процесс себе; другому процессу
тег не поставить.

Для запуска дочерней программы с ожиданием её завершения удобнее
[child](child.md).

## this_process

### `this_process::pid` / `this_process::spawner`

**Краткое описание:** Возвращают pid текущего процесса и pid того, кто его запустил.

**Параметры:** Нет.

**Возвращает:** (`std::uint32_t`).

**Побочные эффекты:** Нет (читают сведения кабины).

**Пример:**
```cpp
#include <print>

#include "box/cxx/process.h"

void Who() { std::println("я {}, меня запустил {}", box::this_process::pid(), box::this_process::spawner()); }
```

### `this_process::add_tag` / `remove_tag` / `has_tag`

**Краткое описание:** Ставят, снимают и проверяют тег процесса.

**Параметры:**
- `tag` (`const char *`): имя тега.

**Возвращает:** `add_tag`, `remove_tag` — (`box::status`) успех или причина (`invalid_argument` для `nullptr`, превышен предел тегов, недопустимое имя тега…); `has_tag` — (`bool`), отсутствие тега — обычный `false`.

**Побочные эффекты:** Меняют членство процесса в широковещательных рассылках.

**Пример:**
```cpp
#include "box/cxx/process.h"

bool Join() { return box::this_process::add_tag("weather").has_value(); }
```

## cabin

### `cabin::pid` / `spawner` / `heap_base` / `heap_max` / `buf_heap_base` / `stack_top` / `info`

**Краткое описание:** Возвращают устройство адресного пространства кабины.

**Параметры:** Нет.

**Возвращает:** `pid`, `spawner` — (`std::uint32_t`); `heap_base`, `heap_max`, `buf_heap_base`, `stack_top` — (`std::uint64_t`) адреса и размер кучи; `info` — (`const CabinInfo &`) все сведения.

**Побочные эффекты:** Нет.

**Типичное применение:** Диагностика, отладочный вывод.

**Пример:**
```cpp
#include <print>

#include "box/cxx/process.h"

void Layout() { std::println("куча с {:#x}, до {} байт", box::cabin::heap_base(), box::cabin::heap_max()); }
```

## process

### `box::process`

**Краткое описание:** Дескриптор процесса по pid.

**Параметры:**
- `process()` — пустой (pid 0).
- `process(std::uint32_t pid)` (`explicit`).

**Возвращает:** Неприменимо: класс-значение; `operator bool` — pid не 0.

**Побочные эффекты:** Нет: дескриптор не владеет процессом и не завершает его.

**Пример:** См. `process::spawn`.

### `process::spawn`

**Краткое описание:** Запускает программу в новой кабине по командной строке.

**Параметры:**
- `line` (`const char *`): строка, как в оболочке: `"say hello world"`; первое слово — программа, вся строка становится Luggage ребёнка ([luggage](luggage.md)) ещё до его первой инструкции.
- `out_gen` (`std::uint32_t *`): если не `nullptr`, сюда пишется поколение pid ребёнка — вторая половина его точной идентичности (pid, поколение); 0 при неудаче.

**Возвращает:** (`box::result<process>`) дескриптор ребёнка; ошибка — `invalid_argument` для `nullptr`, настоящая причина ядра (`file_not_found` для неизвестной программы, `process_limit_exceeded`…) или `spawn_failed`, если ядро не назвало причину.

**Побочные эффекты:** Создаёт процесс.

**Примечания:** `spawn` не ждёт завершения ребёнка. Ожидание и код возврата — [child](child.md).

**Пример:**
```cpp
#include <print>

#include "box/cxx/process.h"

void Hello()
{
    if (auto Child = box::process::spawn("say привет")) std::println("запущен pid {}", Child->pid());
    else std::println("не запустился: {}", Child.error());
}
```

### `process::self` / `process::pid`

**Краткое описание:** Возвращают дескриптор текущего процесса и pid дескриптора.

**Параметры:** Нет.

**Возвращает:** (`process`) / (`std::uint32_t`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/process.h"

std::uint32_t Me() { return box::process::self().pid(); }
```

### `process::info`

**Краткое описание:** Возвращает снимок сведений о процессе.

**Параметры:** Нет.

**Возвращает:** (`box::result<proc_info_t>`) pid, состояние, приоритет; ошибка — `invalid_pid` (0 или больше 65535) или причина ядра (`process_not_found`, когда процесса уже нет).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/process.h"

int Priority(box::process Of) { auto Info = Of.info(); return Info ? Info->priority : -1; }
```

### `process::alive`

**Краткое описание:** Сообщает, жив ли процесс.

**Параметры:** Нет.

**Возвращает:** (`bool`) `info()` удаётся и состояние — не «завершён»; исчезнувший процесс — `false`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "box/cxx/process.h"

bool Running(std::uint32_t Pid) { return box::process(Pid).alive(); }
```

## tag_scope

### `box::tag_scope`

**Краткое описание:** Держит тег процесса, пока жив объект.

**Параметры:**
- `tag_scope()` — пустой.
- `tag_scope(const char *tag)` (`explicit`): ставит тег; при неудаче объект пустой.

**Возвращает:** Неприменимо: класс; только перемещение. `operator bool` — тег поставлен; `tag()` — (`const std::string &`) имя.

**Побочные эффекты:** Конструктор ставит тег, деструктор снимает. Имя копируется до регистрации, поэтому исключение при выделении памяти не оставит тег «висеть» в ядре.

**Типичное применение:** Получать широковещательные сообщения только пока открыт экран или идёт операция.

**Пример:**
```cpp
#include "box/cxx/process.h"

void Listen()
{
    box::tag_scope Weather("weather");
    if (!Weather) return;
    // ... пока Weather жив, рассылки с тегом "weather" доходят до процесса
}
```
