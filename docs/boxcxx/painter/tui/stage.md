# painter::tui::stage — сцена и общие типы событий

## Назначение

[Заголовок](../../../../src/userspace/painter/include/painter/tui/stage.h),
[реализация](../../../../src/userspace/painter/src/tui/stage.cpp).

`Stage` объединяет полноэкранную сцену, [кадр](frame.md) и
[клавиатуру](keyboard.md). Здесь же объявлены типы ошибок, событий и
именованные коды клавиш, которыми пользуются остальные поверхности.

Сцена рисуется через DisplayServer. Перерисовка строится в памяти программы;
`Present()` публикует её отдельно. Блокировок для одновременных вызовов
одного Stage нет: синхронизируйте доступ извне.

## Владение и время жизни

### `Stage::Stage(Stage &&)`

**Краткое описание:** Передайте владение при создании нового объекта.

**Параметры:** `Other` (`Stage &&`, in-out); перемещающий конструктор объявлен `noexcept`.

**Возвращает:** Неприменимо: конструктор.

**Побочные эффекты:** Передаёт ресурсы новому объекту.

**Как работает:** Передаёт внутреннее состояние; ресурс не открывается заново.

**Примечания:** У источника отсутствует состояние; IsOpen возвращает false.

**Пример:**

```cpp
#include "painter/tui/stage.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

tui::Stage Example(tui::Stage &&Other) { return tui::Stage(std::move(Other)); }
```

### `Stage::operator=`

**Краткое описание:** Замените владение ресурсами перемещением.

**Параметры:** `Other` (`Stage &&`, in-out); `Stage &operator=(Stage &&Other) noexcept`.

**Возвращает:** `Stage &`: текущий объект.

**Побочные эффекты:** Освобождает или закрывает прежние ресурсы, принимает новые.

**Как работает:** При самоприсваивании сразу возвращает *this. Иначе вызывает Close для прежнего ресурса, игнорирует его статус и передаёт владение из Other.

**Примечания:** Если результат закрытия важен, явно вызовите и проверьте Close до присваивания.

**Пример:**

```cpp
#include "painter/tui/stage.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Stage &Target, tui::Stage &&Other) { Target = std::move(Other); }
```

### `Stage::~Stage`

**Краткое описание:** Освободите принадлежащие объекту ресурсы при завершении времени жизни.

**Параметры:** Нет.

**Возвращает:** Неприменимо: деструктор.

**Побочные эффекты:** Вызывает Close, игнорируя статус, и освобождает состояние.

**Как работает:** RAII обеспечивает вызов Close при обычном выходе из области видимости и раскрутке стека.

**Примечания:** Когда результат закрытия важен, вызовите Close явно и проверьте его статус до уничтожения объекта.

**Пример:**

```cpp
#include "painter/tui/stage.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Stage) {}
```

## Справочник

### `StageErrorKind`

**Краткое описание:** Различайте причины отказа открытия поверхности.

**Параметры:** Перечисление: `DimensionsUnavailable`, `NoConsole`, `TooSmall`, `FrameUnavailable`, `KeyboardUnavailable`, `InvalidState`.

**Возвращает:** Неприменимо.

**Побочные эффекты:** Нет.

**Примечания:** `TooSmall` также используется для отрицательного минимального размера; `FrameUnavailable` охватывает ошибки размеров и выделения памяти. `InvalidState` при открытии может содержать транспортную или протокольную причину.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::StageErrorKind Kind = tui::StageErrorKind::TooSmall;
```

### `StageError`

**Краткое описание:** Сохраните категорию ошибки, причину и размеры.

**Параметры:** Агрегат: `Kind` (`StageErrorKind`) = `InvalidState`; `Cause` (`box::error`); `Actual`, `Required` (`core::Size`).

**Возвращает:** Неприменимо.

**Побочные эффекты:** Нет.

**Примечания:** Смысл заполненных размеров зависит от места отказа; проверяйте `Kind` и `Cause`.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::StageError Failure{};
```

### `StageError::Describe`

**Краткое описание:** Сформируйте сообщение об ошибке открытия.

**Параметры:** `Subject` (`std::string_view`, in): название программы; `std::string Describe(std::string_view Subject) const`.

**Возвращает:** `std::string`: сообщение на английском языке.

**Побочные эффекты:** Может выделять память и передавать исключения строковых операций.

**Как работает:** Выбирает текст по `Kind`; для `TooSmall` включает фактический и требуемый размеры, для некоторых категорий — `Cause`. Не все сообщения содержат исходную причину.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

std::string Example(const tui::StageError &Failure) { return Failure.Describe("demo"); }
```

### `Modifiers`

**Краткое описание:** Используйте именованные битовые маски модификаторов.

**Параметры:** `Shift`, `Ctrl`, `Alt`, `Extended` (`inline constexpr std::uint8_t`) соответствуют `KB_MOD_*` из `box/keyboard.h`.

**Возвращает:** Неприменимо: пространство имён констант.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

std::uint8_t Mods = tui::Modifiers::Ctrl;
```

### `KeyCodes`

**Краткое описание:** Используйте именованные коды клавиш BoxOS.

**Параметры:** `Escape`, `Backspace`, `Enter`, `Tab` имеют тип `char`; `Home`, `Up`, `PageUp`, `Left`, `Right`, `End`, `Down`, `PageDown`, `Insert`, `Delete` — `std::uint8_t`; значения соответствуют `KEY_*` из `box/keyboard.h`.

**Возвращает:** Неприменимо.

**Побочные эффекты:** Нет.

**Примечания:** Расширенные клавиши различайте по `Scancode` с учётом `IsExtended()`; `KeyCodes` не является Unicode-набором.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::KeyEvent &Key) { return Key.Character == tui::KeyCodes::Escape; }
```

### `KeyEvent`

**Краткое описание:** Получите символ Unicode, сканкод и модификаторы клавиши.

**Параметры:** Агрегат: `Character` (`char32_t`) = `0`, `Scancode` и `Mods` (`std::uint8_t`) = `0`.

**Возвращает:** Неприменимо.

**Побочные эффекты:** Нет.

**Примечания:** `Character` — сам символ Unicode, который печатает клавиша, а не номер кодовой страницы; ненулевой может быть управляющим кодом. Его запись UTF-8 даёт `Spelling()`, печатный ли он — `IsPrintable()`.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::KeyEvent Key{};
```

### `KeyEvent::HasCharacter`

**Краткое описание:** Проверьте, что клавиша печатает символ.

**Параметры:** Нет; `bool HasCharacter() const noexcept`.

**Возвращает:** `bool`: результат `Character != 0`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::KeyEvent &Key) { return Key.HasCharacter(); }
```

### `KeyEvent::IsPrintable`

**Краткое описание:** Проверьте, что символ показывается: не управляющий код C0 или C1, не DEL и не суррогат.

**Параметры:** Нет; `bool IsPrintable() const noexcept`.

**Возвращает:** `bool`: `true` для символа, который можно вставить в текст.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;

bool Example(const tui::KeyEvent &Key) { return Key.IsPrintable(); }
```

### `KeyEvent::Spelling`

**Краткое описание:** Получите символ клавиши записью UTF-8 — то, что хранит строка текста, в которую он набран.

**Параметры:** Нет; `std::string Spelling() const`.

**Возвращает:** `std::string`: от одного до четырёх байтов UTF-8; пустая строка для клавиши без символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include <string>

#include "painter/tui/stage.h"

namespace tui = painter::tui;

void Example(const tui::KeyEvent &Key, std::string &Line) { if (Key.IsPrintable()) Line += Key.Spelling(); }
```

### `KeyEvent::IsExtended`

**Краткое описание:** Проверьте бит `Modifiers::Extended`.

**Параметры:** Нет; `bool IsExtended() const noexcept`.

**Возвращает:** `bool`: результат `(Mods & Modifiers::Extended) != 0`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::KeyEvent &Key) { return Key.IsExtended(); }
```

### `KeyEvent::IsShift`

**Краткое описание:** Проверьте бит `Modifiers::Shift`.

**Параметры:** Нет; `bool IsShift() const noexcept`.

**Возвращает:** `bool`: результат `(Mods & Modifiers::Shift) != 0`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::KeyEvent &Key) { return Key.IsShift(); }
```

### `KeyEvent::IsCtrl`

**Краткое описание:** Проверьте бит `Modifiers::Ctrl`.

**Параметры:** Нет; `bool IsCtrl() const noexcept`.

**Возвращает:** `bool`: результат `(Mods & Modifiers::Ctrl) != 0`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::KeyEvent &Key) { return Key.IsCtrl(); }
```

### `KeyEvent::IsAlt`

**Краткое описание:** Проверьте бит `Modifiers::Alt`.

**Параметры:** Нет; `bool IsAlt() const noexcept`.

**Возвращает:** `bool`: результат `(Mods & Modifiers::Alt) != 0`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::KeyEvent &Key) { return Key.IsAlt(); }
```

### `EventKind`

**Краткое описание:** Различайте клавишу, отсутствие события и завершение ввода.

**Параметры:** `Key`, `Quiet`, `Gone`, `EndAsked`.

**Возвращает:** Неприменимо.

**Побочные эффекты:** Нет.

**Примечания:** `Quiet` не завершает цикл. `Gone` означает завершённый ввод или ошибку; проверяйте `Cause`. `EndAsked` — запрос завершиться, доступный после `HearEnd()`.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::EventKind Kind = tui::EventKind::Quiet;
```

### `Event`

**Краткое описание:** Передайте результат чтения ввода без исключений.

**Параметры:** Агрегат: `Kind` (`EventKind`) = `Gone`; `Key` (`KeyEvent`); `Cause` (`box::error`).

**Возвращает:** Неприменимо.

**Побочные эффекты:** Нет.

**Примечания:** `Key` имеет смысл при `Kind == Key`; успешное событие `Gone` может иметь пустую причину.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Event Result{};
```

### `Stage`

**Краткое описание:** Управляйте полноэкранным слоем, кадром и подпиской клавиатуры.

**Параметры:** Открывается через `Open`; копирование запрещено, перемещение разрешено (`noexcept`).

**Возвращает:** Неприменимо: владеющий класс.

**Побочные эффекты:** Открытие создаёт слой и подписку, уничтожение вызывает `Close()`.

**Как работает:** Состояние содержит `Frame`, внутренний `DisplayLayer` роли `SCENE` и `Keyboard`. Перемещающее присваивание сначала закрывает прежнее состояние; ошибки такого закрытия и деструктора не возвращаются.

**Примечания:** Для интерактивного временного слоя используйте отдельные `Surface` и `Keyboard`.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::Stage::Open(20, 4); }
```

### `Stage::Open`

**Краткое описание:** Откройте сцену размером с доступную консоль.

**Параметры:** `LeastCols`, `LeastRows` (`std::int32_t`, in): минимальные размеры; `static std::expected<Stage, StageError> Open(std::int32_t LeastCols, std::int32_t LeastRows) noexcept`.

**Возвращает:** `std::expected<Stage, StageError>`.

**Побочные эффекты:** Обращается к DisplayServer, выделяет память, открывает клавиатурную подписку.

**Как работает:** Открывает слой, создаёт кадр полного фактического размера и назначает ему `Glass()` и `Mosaics()` сервера. Затем открывает `Keyboard` и состояние Stage; при отказе ранее созданные ресурсы освобождаются деструкторами.

**Примечания:** Аргументы задают минимум, а не размер области. Открытие само по себе не вызывает `Present`.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::Stage::Open(40, 10); }
```

### `Stage::Size`

**Краткое описание:** Получите фактические размеры сцены.

**Параметры:** Нет; `core::Size Size() const noexcept`.

**Возвращает:** `core::Size`: размеры слоя, либо `{}` без состояния.

**Побочные эффекты:** Нет.

**Примечания:** После `Close()` сохранённое состояние остаётся, поэтому размеры не обязаны стать нулевыми.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Size Example(const tui::Stage &Screen) { return Screen.Size(); }
```

### `Stage::IsOpen`

**Краткое описание:** Проверьте локальное состояние открытия сцены.

**Параметры:** Нет; `bool IsOpen() const noexcept`.

**Возвращает:** `bool`: наличие состояния и флаг открытия.

**Побочные эффекты:** Нет.

**Примечания:** Не проверяет доступность DisplayServer.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::Stage &Screen) { return Screen.IsOpen(); }
```

### `Stage::Glass`

**Краткое описание:** Получите тип устройства вывода сцены.

**Параметры:** Нет; `GlassKind Glass() const noexcept`.

**Возвращает:** `GlassKind`: режим сервера или `Pixels` без состояния.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::GlassKind Example(const tui::Stage &Screen) { return Screen.Glass(); }
```

### `Stage::Mosaics`

**Краткое описание:** Получите сетки тоньше половин, которые рисует стекло.

**Параметры:** Нет; `std::uint8_t Mosaics() const noexcept`.

**Возвращает:** `std::uint8_t`: флаги `DisplayMosaic`, как их сказал сервер, либо 0 без состояния.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

std::uint8_t Example(const tui::Stage &Screen) { return Screen.Mosaics(); }
```

### `Stage::FrameBuffer`

**Краткое описание:** Получите ссылку на принадлежащий сцене кадр.

**Параметры:** Нет; перегрузки `Frame &FrameBuffer() noexcept` и `const Frame &FrameBuffer() const noexcept`.

**Возвращает:** `Frame &` или `const Frame &`.

**Побочные эффекты:** Получение ссылки не меняет кадр.

**Примечания:** Требуется наличие состояния: не вызывайте у перемещённого объекта. Ссылка зависит от времени жизни состояния Stage; вызов не отправляет рисунок.

**Типичное применение:** Обычно очистка, рисование, затем `Present`.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Stage &Screen) { Screen.FrameBuffer().Clear(); }
```

### `Stage::Present`

**Краткое описание:** Опубликуйте текущий кадр сцены.

**Параметры:** Нет; `box::status Present() noexcept`.

**Возвращает:** `box::status`: успех или ошибка, включая `invalid_state` для закрытого Stage.

**Побочные эффекты:** Копирует кадр в общую память и отправляет уведомление DisplayServer.

**Как работает:** Проверяет `IsOpen()` и передаёт кадр внутреннему `DisplayLayer::Present`. Протокол допускает замену кадра, ещё не взятого сервером, более новым.

**Примечания:** Успех не подтверждает физический показ каждого кадра. При ошибке уведомления слот уже может быть опубликован.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::Stage &Screen) { return Screen.Present(); }
```

### `Stage::Poll`

**Краткое описание:** Прочитайте доступное событие без ожидания клавиши.

**Параметры:** Нет; `Event Poll() noexcept`.

**Возвращает:** `Event`; для закрытой сцены — `Gone` с `invalid_state`.

**Побочные эффекты:** Может извлекать события подписки.

**Как работает:** После проверки открытия делегирует `Keyboard::Poll`. Правила `Quiet`, `Gone` и `EndAsked` описаны в [keyboard](keyboard.md).

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Event Example(tui::Stage &Screen) { return Screen.Poll(); }
```

### `Stage::Wait`

**Краткое описание:** Ожидайте событие с заданным временем ожидания.

**Параметры:** `TimeoutMs` (`std::uint32_t`, in) = `0`: без ограничения; `Event Wait(std::uint32_t TimeoutMs = 0) noexcept`.

**Возвращает:** `Event`; таймаут даёт `Quiet`.

**Побочные эффекты:** Может приостанавливать выполнение и извлекать события.

**Как работает:** Проверяет состояние и делегирует `Keyboard::Wait(TimeoutMs)`. У закрытого Stage возвращает `Gone` с `invalid_state`.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Event Example(tui::Stage &Screen) { return Screen.Wait(100); }
```

### `Stage::HearEnd`

**Краткое описание:** Включите самостоятельную обработку запроса завершения.

**Параметры:** Нет; `void HearEnd() noexcept`.

**Возвращает:** `void`.

**Побочные эффекты:** Меняет режим чтения клавиатуры.

**Как работает:** При наличии состояния вызывает `Keyboard::HearEnd`. После этого запрос завершения приходит как `EndAsked`, и программа сама решает, когда завершить работу.

**Примечания:** Обратного переключателя в API нет.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Stage &Screen) { Screen.HearEnd(); }
```

### `Stage::Close`

**Краткое описание:** Закройте подписку и сцену.

**Параметры:** Нет; `box::status Close() noexcept`.

**Возвращает:** `box::status`: результат закрытия слоя; повторное закрытие успешно.

**Побочные эффекты:** Снимает подписку и отправляет закрытие слоя; помечает Stage закрытым.

**Как работает:** Сначала сбрасывает локальный флаг, затем закрывает клавиатуру, игнорируя её статус, и возвращает статус слоя. Память состояния и кадра остаётся до уничтожения или замены объекта.

**Примечания:** Даже при ошибке закрытия повторный `Close()` не повторяет отправку.

**Пример:**

```cpp
#include "painter/tui/stage.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::Stage &Screen) { return Screen.Close(); }
```
