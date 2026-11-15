# painter::tui::keyboard — клавиатурная подписка

## Назначение

[Заголовок](../../../../src/userspace/painter/include/painter/tui/keyboard.h),
[реализация](../../../../src/userspace/painter/src/tui/keyboard.cpp).

`Keyboard` читает события консоли через Touch. Типы `Event`, `EventKind`,
`KeyEvent`, `StageError`, `Modifiers` и `KeyCodes` объявлены в
[stage](stage.md). Объект не получает владение сценой.

Сообщение длиной `sizeof(kb_event_t)` декодируется в `KeyEvent`.
Уведомление о завершении распознаётся отдельно. Другие форматы не становятся
клавишами. Вызовы одного объекта требуют внешней синхронизации.

В цикле обрабатывайте `Key`, продолжайте работу при `Quiet`, завершайте
чтение при `Gone`. После `HearEnd()` также обрабатывайте `EndAsked`:
сам факт получения запроса не закрывает слой программы.

## Владение и время жизни

### `Keyboard::Keyboard(Keyboard &&)`

**Краткое описание:** Передайте владение при создании нового объекта.

**Параметры:** `Other` (`Keyboard &&`, in-out); перемещающий конструктор объявлен `noexcept`.

**Возвращает:** Неприменимо: конструктор.

**Побочные эффекты:** Передаёт ресурсы новому объекту.

**Как работает:** Передаёт внутреннее состояние; ресурс не открывается заново.

**Примечания:** У источника отсутствует состояние; IsOpen возвращает false.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

tui::Keyboard Example(tui::Keyboard &&Other) { return tui::Keyboard(std::move(Other)); }
```

### `Keyboard::operator=`

**Краткое описание:** Замените владение ресурсами перемещением.

**Параметры:** `Other` (`Keyboard &&`, in-out); `Keyboard &operator=(Keyboard &&Other) noexcept`.

**Возвращает:** `Keyboard &`: текущий объект.

**Побочные эффекты:** Освобождает или закрывает прежние ресурсы, принимает новые.

**Как работает:** При самоприсваивании сразу возвращает *this. Иначе вызывает Close для прежнего ресурса, игнорирует его статус и передаёт владение из Other.

**Примечания:** Если результат закрытия важен, явно вызовите и проверьте Close до присваивания.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Keyboard &Target, tui::Keyboard &&Other) { Target = std::move(Other); }
```

### `Keyboard::~Keyboard`

**Краткое описание:** Освободите принадлежащие объекту ресурсы при завершении времени жизни.

**Параметры:** Нет.

**Возвращает:** Неприменимо: деструктор.

**Побочные эффекты:** Вызывает Close, игнорируя статус, и освобождает состояние.

**Как работает:** RAII обеспечивает вызов Close при обычном выходе из области видимости и раскрутке стека.

**Примечания:** Когда результат закрытия важен, вызовите Close явно и проверьте его статус до уничтожения объекта.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Keyboard) {}
```

## Справочник

### `Keyboard`

**Краткое описание:** Управляйте подпиской на клавиши без открытия экрана.

**Параметры:** Открывается через `Open`; копирование запрещено, перемещение разрешено (`noexcept`).

**Возвращает:** Неприменимо: владеющий класс.

**Побочные эффекты:** Создаёт и снимает подписку; деструктор вызывает `Close()`.

**Как работает:** Состояние хранит Touch-тег, флаг открытия и режим самостоятельного завершения. Перемещающее присваивание закрывает прежнюю подписку.

**Примечания:** Не рисует и не очищает экран. Не создаёт отдельный независимый источник клавиш для каждого объекта: использует `console_listen` / `console_unlisten` консоли.

**Типичное применение:** Используйте вместе с [Surface](surface.md) для интерактивного слоя.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::Keyboard::Open(); }
```

### `Keyboard::Open`

**Краткое описание:** Откройте клавиатурную подписку текущей консоли.

**Параметры:** Нет; `static std::expected<Keyboard, StageError> Open() noexcept`.

**Возвращает:** `std::expected<Keyboard, StageError>`; `KeyboardUnavailable` / `device_not_ready` при недоступном теге, `FrameUnavailable` / `no_memory` при отказе выделения памяти.

**Побочные эффекты:** Вызывает `console_listen`, выделяет состояние.

**Как работает:** Получает тег через `console_listen()`, затем создаёт состояние. Если выделение не удалось, вызывает `console_unlisten()` до возврата ошибки.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::Keyboard::Open(); }
```

### `Keyboard::IsOpen`

**Краткое описание:** Проверьте локальный флаг открытия подписки.

**Параметры:** Нет; `bool IsOpen() const noexcept`.

**Возвращает:** `bool`: состояние существует и открыто.

**Побочные эффекты:** Нет.

**Примечания:** Не проверяет, завершён ли глобальный консольный ввод и существует ли DisplayServer.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::Keyboard &Ear) { return Ear.IsOpen(); }
```

### `Keyboard::Poll`

**Краткое описание:** Извлеките первое распознанное событие без ожидания.

**Параметры:** Нет; `Event Poll() noexcept`.

**Возвращает:** `Event`: `Key`, `EndAsked`, `Gone` или `Quiet`, если распознанных событий нет.

**Побочные эффекты:** Удаляет прочитанные Touch-сообщения; может пометить консольный ввод завершённым.

**Как работает:** Проверяет открытие и, без `HearEnd`, признак `console_input_ended()`. Читает тег через `touch_try_pop_tag`, пропуская сообщения неподходящего формата, пока не получит событие или очередь не опустеет.

**Примечания:** Закрытый объект даёт `Gone` с `invalid_state`; завершённый ввод — `Gone` с пустой причиной.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Event Example(tui::Keyboard &Ear) { return Ear.Poll(); }
```

### `Keyboard::Wait`

**Краткое описание:** Дождитесь распознанного события или окончания времени ожидания.

**Параметры:** `TimeoutMs` (`std::uint32_t`, in) = `0`: ждать без ограничения; `Event Wait(std::uint32_t TimeoutMs = 0) noexcept`.

**Возвращает:** `Event`; `ERR_TIMEOUT` превращается в `Quiet`, прочие ошибки ожидания — в `Gone` с `Cause`.

**Побочные эффекты:** Ожидает Touch, удаляет сообщение из очереди, может завершить консольный ввод.

**Как работает:** Вызывает `touch_await`. При неизвестном формате и ненулевом таймауте возвращает `Quiet` сразу, даже если таймаут ещё не истёк; при нулевом продолжает ожидание.

**Примечания:** Не считайте `Quiet` доказательством истечения всего интервала.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Event Example(tui::Keyboard &Ear) { return Ear.Wait(250); }
```

### `Keyboard::HearEnd`

**Краткое описание:** Примите ответственность за обработку запроса завершения.

**Параметры:** Нет; `void HearEnd() noexcept`.

**Возвращает:** `void`.

**Побочные эффекты:** Устанавливает флаг `HearsEnd`, если состояние существует.

**Как работает:** После включения уведомление `CONSOLE_NOTICE_END` возвращается как `EndAsked`. Без включения оно вызывает `console_input_ends()` и возвращается как `Gone`.

**Примечания:** Режим необратим до закрытия объекта; влияет на обработку последующих сообщений, но не отменяет уже обработанное завершение.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Keyboard &Ear) { Ear.HearEnd(); }
```

### `Keyboard::Close`

**Краткое описание:** Снимите подписку клавиатуры.

**Параметры:** Нет; `box::status Close() noexcept`.

**Возвращает:** `box::status`: в текущей реализации всегда успех.

**Побочные эффекты:** Вызывает `console_unlisten`, сбрасывает локальный тег и флаг.

**Как работает:** Для отсутствующего или закрытого состояния ничего не делает. Иначе помечает объект закрытым и освобождает подписку.

**Пример:**

```cpp
#include "painter/tui/keyboard.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::Keyboard &Ear) { return Ear.Close(); }
```
