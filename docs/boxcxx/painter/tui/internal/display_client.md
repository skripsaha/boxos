# painter::tui::DisplayLayer — внутренний клиент DisplayServer

## Назначение

[Заголовок](../../../../../src/userspace/painter/src/tui/display_client.h),
[реализация](../../../../../src/userspace/painter/src/tui/display_client.cpp).

Модуль связывает TUI с протоколом
[box/display.h](../../../../../src/userspace/boxlib/include/box/display.h).
Используется внутри Stage и LayerCanvas; не является публичным прикладным
заголовком. Примеры требуют `-I src/userspace/painter/src/tui`.

Открытие и фиксация выполняются запросом с ответом, уведомления о кадре и
закрытии — отправкой сообщения. Общая память содержит три слота с
состояниями FREE, WRITING, READY и READING. Предположение клиента:
сервер держит в READING не больше одного слота. Протокол отделяет публикацию
кадра от его фактического вывода.

Вспомогательная функция `CellCount` проверяет положительные размеры и
переполнение произведения; `Cause` переводит код BoxOS в box::error;
`Error` собирает StageError. Приватный конструктор сохраняет параметры
ответа и указатель на заголовок bay; наружу создание доступно только через Open.

## Владение и время жизни

### `DisplayLayer::DisplayLayer(DisplayLayer &&)`

**Краткое описание:** Передайте владение при создании нового объекта.

**Параметры:** `Other` (`DisplayLayer &&`, in-out); перемещающий конструктор объявлен `noexcept`.

**Возвращает:** Неприменимо: конструктор.

**Побочные эффекты:** Передаёт ресурсы новому объекту.

**Как работает:** Передаёт внутреннее состояние; ресурс не открывается заново.

**Примечания:** В источнике Header становится nullptr и Opened — false.

**Пример:**

```cpp
#include "display_client.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

tui::DisplayLayer Example(tui::DisplayLayer &&Other) { return tui::DisplayLayer(std::move(Other)); }
```

### `DisplayLayer::operator=`

**Краткое описание:** Замените владение ресурсами перемещением.

**Параметры:** `Other` (`DisplayLayer &&`, in-out); `DisplayLayer &operator=(DisplayLayer &&Other) noexcept`.

**Возвращает:** `DisplayLayer &`: текущий объект.

**Побочные эффекты:** Освобождает или закрывает прежние ресурсы, принимает новые.

**Как работает:** При самоприсваивании сразу возвращает *this. Иначе вызывает Close для прежнего ресурса, игнорирует его статус и передаёт владение из Other.

**Примечания:** Если результат закрытия важен, явно вызовите и проверьте Close до присваивания.

**Пример:**

```cpp
#include "display_client.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::DisplayLayer &Target, tui::DisplayLayer &&Other) { Target = std::move(Other); }
```

### `DisplayLayer::~DisplayLayer`

**Краткое описание:** Освободите принадлежащие объекту ресурсы при завершении времени жизни.

**Параметры:** Нет.

**Возвращает:** Неприменимо: деструктор.

**Побочные эффекты:** Вызывает Close, игнорируя статус, и освобождает состояние.

**Как работает:** RAII обеспечивает вызов Close при обычном выходе из области видимости и раскрутке стека.

**Примечания:** Когда результат закрытия важен, вызовите Close явно и проверьте его статус до уничтожения объекта.

**Пример:**

```cpp
#include "display_client.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::DisplayLayer) {}
```

## Справочник

### `DisplayLayer`

**Краткое описание:** Управляйте одним слоем DisplayServer и его общей памятью.

**Параметры:** Создаётся через Open; копирование запрещено, перемещение `noexcept`.

**Возвращает:** Неприменимо: внутренний владеющий класс.

**Побочные эффекты:** Открывает слой, отображает bay, при уничтожении вызывает Close.

**Как работает:** Владеет bounds, идентификаторами сервера и слоя, Cursor, GlassKind, bay, указателем Header, счётчиком последовательности и флагом Opened. Перемещение передаёт владение и обнуляет Header и Opened источника.

**Примечания:** Атомарные поля слотов обеспечивают обмен с сервером; сам объект и его NextSequence не предназначены для нескольких одновременных производителей.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::DisplayLayer::Open(0, 0, DISPLAY_LAYER_SCENE); }
```

### `DisplayLayer::Open`

**Краткое описание:** Откройте полноразмерный слой с проверкой серверного протокола.

**Параметры:** `LeastCols`, `LeastRows` (`std::int32_t`, in), `Role` (`DisplayLayerRole`, in); `static std::expected<DisplayLayer, StageError> Open(std::int32_t LeastCols, std::int32_t LeastRows, DisplayLayerRole Role) noexcept`.

**Возвращает:** `std::expected<DisplayLayer, StageError>`.

**Побочные эффекты:** Спрашивает размеры, отправляет запрос, открывает общую память.

**Как работает:** Проверяет неотрицательные минимумы, размеры консоли и допустимое число ячеек. Посылает LAYER_OPEN с console_order_mark, проверяет длину и команду ответа, роль режима вывода, идентификаторы и завершение строки bay_tag. После открытия bay проверяет размер памяти, magic, version, геометрию и число слотов.

**Примечания:** Ошибки: DimensionsUnavailable при display_size; NoConsole для пустого размера; TooSmall для отрицательного минимума или недостаточного размера; FrameUnavailable при недопустимом объёме; InvalidState для отказа запроса или повреждённого протокола. Cause содержит подробность.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::DisplayLayer::Open(20, 4, DISPLAY_LAYER_OVERLAY); }
```

### `DisplayLayer::Size`

**Краткое описание:** Получите размеры слоя в ячейках.

**Параметры:** Нет; `core::Size Size() const noexcept`.

**Возвращает:** `core::Size`: ширина и высота Bounds.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example(const tui::DisplayLayer &Layer) { return Layer.Size(); }
```

### `DisplayLayer::IsOpen`

**Краткое описание:** Проверьте локальный флаг открытия слоя.

**Параметры:** Нет; `bool IsOpen() const noexcept`.

**Возвращает:** `bool`: Opened.

**Побочные эффекты:** Нет.

**Примечания:** Не опрашивает состояние сервера.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::DisplayLayer &Layer) { return Layer.IsOpen(); }
```

### `DisplayLayer::Glass`

**Краткое описание:** Получите тип устройства из ответа открытия.

**Параметры:** Нет; `GlassKind Glass() const noexcept`.

**Возвращает:** `GlassKind`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example(const tui::DisplayLayer &Layer) { return Layer.Glass(); }
```

### `DisplayLayer::Cursor`

**Краткое описание:** Получите позицию консольного курсора на момент открытия.

**Параметры:** Нет; `core::Point Cursor() const noexcept`.

**Возвращает:** `core::Point`: сохранённая позиция из ответа.

**Побочные эффекты:** Нет.

**Примечания:** Позиция учитывает предшествующий вывод текущего strand, упорядоченный отметкой запроса. Это не текущий запрос курсора.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example(const tui::DisplayLayer &Layer) { return Layer.Cursor(); }
```

### `DisplayLayer::Present`

**Краткое описание:** Запишите полный кадр в слот общей памяти и уведомите сервер.

**Параметры:** `Picture` (`const Frame &`, in); `box::status Present(const Frame &Picture) noexcept`.

**Возвращает:** `box::status`: успех, `invalid_state`, `invalid_argument` при несовпадении размера или ошибка Notify.

**Побочные эффекты:** Пишет слот, каретку и последовательность; отправляет LAYER_FRAME.

**Как работает:** Захватывает FREE-слот атомарным переходом в WRITING; если свободного нет, пытается заменить самый старый READY, который сервер ещё не взял. Копирует ячейки, видимость, отметки и каретку, повышает NextSequence и публикует READY с release, затем вызывает Notify. READING-слот не перезаписывается.

**Примечания:** Успех не является подтверждением показа кадра; более новый кадр может заменить READY. Ошибка Notify возникает уже после публикации. Поиск слота не имеет таймаута.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::DisplayLayer &Layer, const tui::Frame &Picture) { return Layer.Present(Picture); }
```

### `DisplayLayer::Notify`

**Краткое описание:** Отправьте уведомление о готовом кадре.

**Параметры:** Нет; приватный `box::status Notify() noexcept`.

**Возвращает:** `box::status`: успех либо ошибка отправки.

**Побочные эффекты:** Отправляет LAYER_FRAME с идентификатором слоя и поколением strand; может вызывать yield.

**Как работает:** При ERR_ROUTE_TARGET_FULL и ERR_NO_MEMORY повторяет send после yield. Любую другую ошибку возвращает вызывающему коду.

**Примечания:** Отдельного лимита повторов нет. Прямой вызов недоступен; пример показывает использующий его метод.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::DisplayLayer &Layer, const tui::Frame &Picture) {
    // Notify — приватная часть этого публичного пути.
    return Layer.Present(Picture);
}
```

### `DisplayLayer::Commit`

**Краткое описание:** Перенесите уже опубликованный слой в текст консоли.

**Параметры:** `ScrollRows` (`std::uint32_t`, in) = `0`; `box::status Commit(std::uint32_t ScrollRows = 0) noexcept`.

**Возвращает:** `box::status`: успех или ошибка состояния, диапазона, транспорта, ответа сервера.

**Побочные эффекты:** Отправляет упорядоченный запрос; при успехе снимает локальное отображение bay и закрывает слой.

**Как работает:** Проверяет открытие, Header и ScrollRows <= Height. Посылает LAYER_COMMIT с console_order_mark и проверяет размер, команду и код ответа. При успехе сбрасывает Opened, Header и Shared.

**Примечания:** Не вызывает Present. Сервер требует доступный кадр; после успешной фиксации повторный Commit недопустим. Повреждённый ответ даёт corrupted.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::DisplayLayer &Layer) { return Layer.Commit(); }
```

### `DisplayLayer::Close`

**Краткое описание:** Отправьте закрытие слоя и освободите общую память клиента.

**Параметры:** Нет; `box::status Close() noexcept`.

**Возвращает:** `box::status`: успех, в том числе при ERR_PROCESS_NOT_FOUND, либо другая ошибка send.

**Побочные эффекты:** Отправляет LAYER_CLOSE, сбрасывает Opened, Header и Shared.

**Как работает:** При закрытом слое сразу возвращает успех. Иначе отправляет запрос с поколением strand, затем освобождает локальные ресурсы независимо от результата send.

**Примечания:** Закрытие не ждёт подтверждения сервера и не повторяется после ошибки. Деструктор игнорирует статус.

**Пример:**

```cpp
#include "display_client.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::DisplayLayer &Layer) { return Layer.Close(); }
```
