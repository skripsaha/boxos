# painter::tui::LayerCanvas — внутренний буфер слоя

## Назначение

[Заголовок](../../../../../src/userspace/painter/src/tui/layer_canvas.h),
[реализация](../../../../../src/userspace/painter/src/tui/layer_canvas.cpp).

Внутренний модуль разделяет хранение и копирование ячеек между
[Surface](../surface.md) и [ConsoleSurface](../console_surface.md).
Заголовок находится в src, не в публичном include; прикладной код использует
эти два класса. Примеры этой страницы требуют дополнительного пути
`-I src/userspace/painter/src/tui`.

Один вызов Paint или Composite заменяет прямоугольник локального буфера,
после чего публикует полный слой. Локальные изменения не откатываются при
ошибке транспорта. Внутренних блокировок для общего использования нет.

## Владение и время жизни

### `LayerCanvas::LayerCanvas(LayerCanvas &&)`

**Краткое описание:** Передайте владение при создании нового объекта.

**Параметры:** `Other` (`LayerCanvas &&`, in-out); перемещающий конструктор объявлен `noexcept`.

**Возвращает:** Неприменимо: конструктор.

**Побочные эффекты:** Передаёт ресурсы новому объекту.

**Как работает:** Передаёт внутреннее состояние; ресурс не открывается заново.

**Примечания:** У источника отсутствует состояние; IsOpen возвращает false.

**Пример:**

```cpp
#include "layer_canvas.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

tui::LayerCanvas Example(tui::LayerCanvas &&Other) { return tui::LayerCanvas(std::move(Other)); }
```

### `LayerCanvas::operator=`

**Краткое описание:** Замените владение ресурсами перемещением.

**Параметры:** `Other` (`LayerCanvas &&`, in-out); `LayerCanvas &operator=(LayerCanvas &&Other) noexcept`.

**Возвращает:** `LayerCanvas &`: текущий объект.

**Побочные эффекты:** Освобождает или закрывает прежние ресурсы, принимает новые.

**Как работает:** При самоприсваивании сразу возвращает *this. Иначе вызывает Close для прежнего ресурса, игнорирует его статус и передаёт владение из Other.

**Примечания:** Если результат закрытия важен, явно вызовите и проверьте Close до присваивания.

**Пример:**

```cpp
#include "layer_canvas.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::LayerCanvas &Target, tui::LayerCanvas &&Other) { Target = std::move(Other); }
```

### `LayerCanvas::~LayerCanvas`

**Краткое описание:** Освободите принадлежащие объекту ресурсы при завершении времени жизни.

**Параметры:** Нет.

**Возвращает:** Неприменимо: деструктор.

**Побочные эффекты:** Вызывает Close, игнорируя статус, и освобождает состояние.

**Как работает:** RAII обеспечивает вызов Close при обычном выходе из области видимости и раскрутке стека.

**Примечания:** Когда результат закрытия важен, вызовите Close явно и проверьте его статус до уничтожения объекта.

**Пример:**

```cpp
#include "layer_canvas.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::LayerCanvas) {}
```

## Справочник

### `LayerCanvas`

**Краткое описание:** Сохраняйте полный клеточный буфер серверного слоя.

**Параметры:** Открывается фабрикой `Open`; перемещение `noexcept`, копирование запрещено.

**Возвращает:** Неприменимо: внутренний владеющий класс.

**Побочные эффекты:** Владеет слоем и кадром; деструктор вызывает `Close`.

**Как работает:** Скрытый Impl содержит `DisplayLayer Layer`, `Frame LayerCells`, `core::Point Cursor` и `bool Opened`. Перемещающее присваивание закрывает прежнее состояние.

**Примечания:** Рисунок хранит собственную каретку; Cursor отдельно хранит позицию продолжения консоли.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::LayerCanvas::Open(0, 0, DISPLAY_LAYER_OVERLAY); }
```

### `LayerCanvas::Open`

**Краткое описание:** Откройте серверный слой и прозрачный локальный кадр.

**Параметры:** `LeastCols`, `LeastRows` (`std::int32_t`, in), `Role` (`DisplayLayerRole`, in); `static std::expected<LayerCanvas, StageError> Open(std::int32_t LeastCols, std::int32_t LeastRows, DisplayLayerRole Role) noexcept`.

**Возвращает:** `std::expected<LayerCanvas, StageError>`.

**Побочные эффекты:** Создаёт слой и выделяет буфер полного размера консоли.

**Как работает:** Открывает DisplayLayer, создаёт Frame, вызывает ClearTransparent, запоминает Cursor слоя. Если создание кадра или Impl не удалось, слой закрывается через RAII.

**Примечания:** Режим Glass берётся из DisplayLayer, а не из локального кадра. Код не назначает Glass в LayerCells, поскольку его задача — хранение и копирование.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::LayerCanvas::Open(20, 4, DISPLAY_LAYER_OVERLAY); }
```

### `LayerCanvas::Size`

**Краткое описание:** Получите размеры слоя.

**Параметры:** Нет; `core::Size Size() const noexcept`.

**Возвращает:** `core::Size` или `{}` без состояния.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example(const tui::LayerCanvas &Canvas) { return Canvas.Size(); }
```

### `LayerCanvas::Glass`

**Краткое описание:** Получите режим устройства вывода слоя.

**Параметры:** Нет; `GlassKind Glass() const noexcept`.

**Возвращает:** `GlassKind` или `Pixels` без состояния.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example(const tui::LayerCanvas &Canvas) { return Canvas.Glass(); }
```

### `LayerCanvas::IsOpen`

**Краткое описание:** Проверьте локальный флаг открытия canvas.

**Параметры:** Нет; `bool IsOpen() const noexcept`.

**Возвращает:** `bool`: состояние существует и Opened установлен.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::LayerCanvas &Canvas) { return Canvas.IsOpen(); }
```

### `LayerCanvas::Cursor`

**Краткое описание:** Получите сохранённый курсор продолжения консоли.

**Параметры:** Нет; `std::expected<core::Point, box::error> Cursor() const noexcept`.

**Возвращает:** `std::expected<core::Point, box::error>`; закрытый canvas даёт `invalid_state`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example(const tui::LayerCanvas &Canvas) { return Canvas.Cursor(); }
```

### `LayerCanvas::Paint`

**Краткое описание:** Скопируйте область кадра с принудительным непрозрачным покрытием.

**Параметры:** `Origin` (`core::Point`, in), `FrameBuffer` (`const Frame &`, in), необязательный `Region` (`core::Rect`, in). Две перегрузки возвращают `box::status`, объявлены `noexcept`.

**Возвращает:** `box::status`: успех, `invalid_state`, `out_of_range` либо ошибка публикации.

**Побочные эффекты:** Заменяет буфер и каретку, публикует весь слой.

**Как работает:** Без Region выбирает весь источник. Проверяет положительные размеры области, её полное вхождение в источник и полное вхождение назначения в слой. StoreCell устанавливает Opaque; CarryCaret переносит каретку, затем Present публикует буфер.

**Примечания:** Прозрачные значения цветов не изменяются. Отсечения нет; при ошибке отправки локальный буфер уже изменён.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::LayerCanvas &Canvas, const tui::Frame &Picture) { return Canvas.Paint({0, 0}, Picture); }
```

### `LayerCanvas::Composite`

**Краткое описание:** Скопируйте область кадра с исходным покрытием ячеек.

**Параметры:** `Origin` (`core::Point`, in), `FrameBuffer` (`const Frame &`, in), необязательный `Region` (`core::Rect`, in). Две перегрузки возвращают `box::status`, объявлены `noexcept`.

**Возвращает:** `box::status`: те же проверки и ошибки, что у Paint.

**Побочные эффекты:** Заменяет буфер и каретку, публикует весь слой.

**Как работает:** Повторяет проверку прямоугольников Paint, но StoreCell сохраняет VisibilityMode. Прозрачные ячейки заменяют старое покрытие, а не пропускаются; CarryCaret и публикация применяются после копирования.

**Примечания:** Origin соответствует началу Region в назначении.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::LayerCanvas &Canvas, const tui::Frame &Picture) { return Canvas.Composite({0, 0}, Picture); }
```

### `LayerCanvas::Clear`

**Краткое описание:** Очистите локальный буфер без публикации.

**Параметры:** Нет; `box::status Clear() noexcept`.

**Возвращает:** `box::status`: успех либо `invalid_state`.

**Побочные эффекты:** Вызывает ClearTransparent у буфера; снимает каретку рисунка.

**Как работает:** Проверяет открытие, очищает LayerCells и возвращает успех. Сохранённый Cursor не изменяется.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::LayerCanvas &Canvas) { return Canvas.Clear(); }
```

### `LayerCanvas::SetCursor`

**Краткое описание:** Установите каретку рисунка и сохранённый курсор.

**Параметры:** `At` (`core::Point`, in); `box::status SetCursor(core::Point At) noexcept`.

**Возвращает:** `box::status`: успех, `invalid_state`, `out_of_range` или ошибка публикации.

**Побочные эффекты:** Изменяет каретку, затем Cursor и публикует слой.

**Как работает:** Проверяет координату через Frame::SetCaret. Если она недопустима, каретка сбрасывается локально, но Cursor не меняется; иначе Cursor присваивается At и выполняется Present.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::LayerCanvas &Canvas) { return Canvas.SetCursor({0, 1}); }
```

### `LayerCanvas::Commit`

**Краткое описание:** Передайте слой в текст консоли с заданной прокруткой.

**Параметры:** `ScrollRows` (`std::uint32_t`, in) = `0`; `box::status Commit(std::uint32_t ScrollRows = 0) noexcept`.

**Возвращает:** `box::status`: результат DisplayLayer::Commit либо `invalid_state`.

**Побочные эффекты:** Запрашивает серверную фиксацию; при успехе сбрасывает Opened.

**Как работает:** Делегирует Commit серверному слою. Только успешный статус помечает canvas закрытым.

**Примечания:** Текущий локальный буфер отдельно не публикуется; используется уже отправленный кадр.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::LayerCanvas &Canvas) { return Canvas.Commit(0); }
```

### `LayerCanvas::Close`

**Краткое описание:** Закройте canvas и серверный слой.

**Параметры:** Нет; `box::status Close() noexcept`.

**Возвращает:** `box::status`: результат DisplayLayer::Close или успех без состояния.

**Побочные эффекты:** Сбрасывает Opened и закрывает серверный слой.

**Как работает:** Повторный вызов передаётся слою, у которого закрытие идемпотентно. Деструктор игнорирует возвращённый статус.

**Пример:**

```cpp
#include "layer_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::LayerCanvas &Canvas) { return Canvas.Close(); }
```

## Внутренние операции копирования

`StoreCell` меняет только VisibilityMode при ForceOpaque и передаёт ячейку
в Frame::Put. `Holds` проверяет включение точки в полуоткрытый прямоугольник.
`CarryCaret` снимает каретку назначения, попавшую в заменяемый прямоугольник,
затем переносит каретку источника из Region с поправкой на Origin.
Если источник приносит каретку, она становится единственной кареткой слоя.
Локальная функция `Error` только собирает StageError из переданных полей.
