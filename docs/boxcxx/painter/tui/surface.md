# painter::tui::surface — временный слой

## Назначение

[Заголовок](../../../../src/userspace/painter/include/painter/tui/surface.h),
[реализация](../../../../src/userspace/painter/src/tui/surface.cpp).

`Surface` хранит рисунок поверх базовой сцены. Сервер сохраняет слой между
вызовами, поэтому программа может заменять только нужные прямоугольники.
Слой удаляется при закрытии; DisplayServer также обрабатывает исчезновение
процесса-владельца. Для записи в текст консоли предусмотрен
[ConsoleSurface](console_surface.md).

На поверхности хранится одна каретка: замена прямоугольника снимает прежнюю
каретку в нём; каретка источника внутри копируемой области переносится в
назначение. Если она пришла из другой области, она заменяет прежнюю каретку.
Блокировок внутри объекта нет.

## Владение и время жизни

### `Surface::Surface(Surface &&)`

**Краткое описание:** Передайте владение при создании нового объекта.

**Параметры:** `Other` (`Surface &&`, in-out); перемещающий конструктор объявлен `noexcept`.

**Возвращает:** Неприменимо: конструктор.

**Побочные эффекты:** Передаёт ресурсы новому объекту.

**Как работает:** Передаёт внутреннее состояние; ресурс не открывается заново.

**Примечания:** У источника отсутствует состояние; IsOpen возвращает false.

**Пример:**

```cpp
#include "painter/tui/surface.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

tui::Surface Example(tui::Surface &&Other) { return tui::Surface(std::move(Other)); }
```

### `Surface::operator=`

**Краткое описание:** Замените владение ресурсами перемещением.

**Параметры:** `Other` (`Surface &&`, in-out); `Surface &operator=(Surface &&Other) noexcept`.

**Возвращает:** `Surface &`: текущий объект.

**Побочные эффекты:** Освобождает или закрывает прежние ресурсы, принимает новые.

**Как работает:** При самоприсваивании сразу возвращает *this. Иначе вызывает Close для прежнего ресурса, игнорирует его статус и передаёт владение из Other.

**Примечания:** Если результат закрытия важен, явно вызовите и проверьте Close до присваивания.

**Пример:**

```cpp
#include "painter/tui/surface.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Surface &Target, tui::Surface &&Other) { Target = std::move(Other); }
```

### `Surface::~Surface`

**Краткое описание:** Освободите принадлежащие объекту ресурсы при завершении времени жизни.

**Параметры:** Нет.

**Возвращает:** Неприменимо: деструктор.

**Побочные эффекты:** Вызывает Close, игнорируя статус, и освобождает состояние.

**Как работает:** RAII обеспечивает вызов Close при обычном выходе из области видимости и раскрутке стека.

**Примечания:** Когда результат закрытия важен, вызовите Close явно и проверьте его статус до уничтожения объекта.

**Пример:**

```cpp
#include "painter/tui/surface.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Surface) {}
```

## Справочник

### `SurfaceRole`

**Краткое описание:** Выберите роль временного слоя.

**Параметры:** `Overlay` → `DISPLAY_LAYER_OVERLAY`, `Notice` → `DISPLAY_LAYER_NOTICE`.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Примечания:** Роль задаёт порядок композиции сервера. `Notice` предназначен для уведомлений поверх обычных слоёв.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::SurfaceRole Role = tui::SurfaceRole::Notice;
```

### `Surface`

**Краткое описание:** Храните временный слой рисунка над сценой.

**Параметры:** Открывается через `Open`; копирование запрещено, перемещение разрешено (`noexcept`).

**Возвращает:** Неприменимо: владеющий класс.

**Побочные эффекты:** Создаёт слой и локальный буфер; деструктор вызывает `Close()`.

**Как работает:** Состояние содержит внутренний `LayerCanvas`. Перемещающее присваивание сначала закрывает прежний слой, игнорируя статус закрытия.

**Примечания:** Не открывает клавиатуру и не переносит рисунок в постоянный текст консоли.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::Surface::Open(); }
```

### `Surface::Open`

**Краткое описание:** Откройте временный слой доступной консоли.

**Параметры:** `LeastCols`, `LeastRows` (`std::int32_t`, in) = `0`; `Role` (`SurfaceRole`, in) = `Overlay`; `static std::expected<Surface, StageError> Open(std::int32_t LeastCols = 0, std::int32_t LeastRows = 0, SurfaceRole Role = SurfaceRole::Overlay) noexcept`.

**Возвращает:** `std::expected<Surface, StageError>`.

**Побочные эффекты:** Обращается к DisplayServer, выделяет слой, буфер и состояние.

**Как работает:** Переводит роль в серверное перечисление и открывает `LayerCanvas`, который создаёт прозрачный буфер полного размера консоли. При отказе состояние освобождается через RAII.

**Примечания:** Аргументы — минимальный размер консоли, не размер отдельного окна.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::Surface::Open(20, 4, tui::SurfaceRole::Overlay); }
```

### `Surface::Size`

**Краткое описание:** Получите фактические размеры поверхности.

**Параметры:** Нет; `core::Size Size() const noexcept`.

**Возвращает:** `core::Size`: размеры консоли либо `{}` без состояния.

**Побочные эффекты:** Нет.

**Примечания:** После закрытия размеры сохраняются, пока существует состояние.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Size Example(const tui::Surface &Screen) { return Screen.Size(); }
```

### `Surface::Glass`

**Краткое описание:** Получите тип устройства вывода.

**Параметры:** Нет; `GlassKind Glass() const noexcept`.

**Возвращает:** `GlassKind`: режим сервера либо `Pixels` без состояния.

**Побочные эффекты:** Нет.

**Типичное применение:** Передайте его в `Frame::SetGlass` при работе `FineCanvas`.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::GlassKind Example(const tui::Surface &Screen) { return Screen.Glass(); }
```

### `Surface::Mosaics`

**Краткое описание:** Получите сетки тоньше половин, которые рисует стекло.

**Параметры:** Нет; `std::uint8_t Mosaics() const noexcept`.

**Возвращает:** `std::uint8_t`: флаги `DisplayMosaic`, как их сказал сервер, либо 0 без состояния.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

std::uint8_t Example(const tui::Surface &Screen) { return Screen.Mosaics(); }
```

### `Surface::IsOpen`

**Краткое описание:** Проверьте локальное открытие поверхности.

**Параметры:** Нет; `bool IsOpen() const noexcept`.

**Возвращает:** `bool`: состояние существует и canvas открыт.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::Surface &Screen) { return Screen.IsOpen(); }
```

### `Surface::Composite`

**Краткое описание:** Замените прямоугольник слоя данными кадра с сохранением прозрачности.

**Параметры:** `Origin` (`core::Point`, in): левый верхний угол назначения; `FrameBuffer` (`const Frame &`, in): источник; необязательный `Region` (`core::Rect`, in): прямоугольник источника. Перегрузки возвращают `box::status`, объявлены `noexcept`; без `Region` используется весь кадр.

**Возвращает:** `box::status`: успех или ошибка; `invalid_state` для закрытого объекта, `out_of_range` для недопустимого прямоугольника.

**Побочные эффекты:** Меняет локальный буфер, переносит каретку и публикует весь слой.

**Как работает:** Проверяет, что непустой `Region` целиком внутри кадра, а прямоугольник того же размера от `Origin` целиком внутри поверхности. Копирует все поля ячеек, включая прозрачность; каретка источника внутри `Region` переносится в координаты назначения. Затем отправляет сохранённый полный буфер.

**Примечания:** Отсечения границ нет. `Origin` — место начала выбранного `Region`, а не смещение всего кадра. Прозрачная ячейка удаляет прежнее покрытие этой позиции в текущем слое; не оставляет старый рисунок. Ошибка отправки не откатывает локальную замену.

**Типичное применение:** При перемещении фигуры очищайте старое место или вызывайте `Clear()` перед отправкой нового расположения.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::Surface &Screen, const tui::Frame &Picture) {
    return Screen.Composite({2, 1}, Picture, {0, 0, 4, 2});
}
```

### `Surface::Clear`

**Краткое описание:** Подготовьте полностью прозрачный буфер для следующей публикации.

**Параметры:** Нет; `box::status Clear() noexcept`.

**Возвращает:** `box::status`: успех или `invalid_state`.

**Побочные эффекты:** Очищает локальный буфер и снимает его каретку; не отправляет кадр.

**Как работает:** Делегирует `LayerCanvas::Clear`, заполняющий буфер прозрачными ячейками. Видимый слой изменится только при следующем `Composite` или закрытии.

**Примечания:** Для немедленного удаления слоя используйте `Close()`.

**Типичное применение:** Связка `Clear()` → `Composite(newOrigin, Picture)` публикует замену без отдельного пустого кадра.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::Surface &Screen) { return Screen.Clear(); }
```

### `Surface::Close`

**Краткое описание:** Закройте временный слой.

**Параметры:** Нет; `box::status Close() noexcept`.

**Возвращает:** `box::status`: статус отправки закрытия; повторное закрытие успешно.

**Побочные эффекты:** Помечает локальное состояние закрытым, отправляет серверу удаление слоя.

**Как работает:** Передаёт закрытие canvas и далее `DisplayLayer`. Память локального кадра остаётся до уничтожения состояния.

**Примечания:** Статус деструктора не возвращается; явно закрывайте объект, когда результат важен. Ошибка отправки уже оставляет локальный объект закрытым.

**Пример:**

```cpp
#include "painter/tui/surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::Surface &Screen) { return Screen.Close(); }
```
