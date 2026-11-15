# painter::tui::console_surface — рисунок в тексте консоли

## Назначение

[Заголовок](../../../../src/userspace/painter/include/painter/tui/console_surface.h),
[реализация](../../../../src/userspace/painter/src/tui/console_surface.cpp).

`ConsoleSurface` нужен программе, чей рисунок должен остаться частью вывода
после её завершения. Пока транзакция открыта, рисунок живёт в отдельном слое;
`Commit()` переносит его в базовую текстовую поверхность и закрывает слой.
[Surface](surface.md) предназначен для временного рисунка без такой фиксации.

У объекта различаются каретка рисунка, сохранённый курсор продолжения и
счётчик прокрутки. `Paint` переносит каретку кадра, `SetCursor` задаёт также
сохранённую позицию, `ScrollUp` меняет только счётчик. Вызовы одного объекта
не защищены внутренними блокировками.

Серверная сторона фиксации описана исходниками
[layers.cpp](../../../../src/userspace/display/layers.cpp): текстовое
представление строится из глифов слоя с удалением пустых крайних строк и
пробелов в конце строк. Это не сохранение исходной строки UTF-8 программы.

## Владение и время жизни

### `ConsoleSurface::ConsoleSurface(ConsoleSurface &&)`

**Краткое описание:** Передайте владение при создании нового объекта.

**Параметры:** `Other` (`ConsoleSurface &&`, in-out); перемещающий конструктор объявлен `noexcept`.

**Возвращает:** Неприменимо: конструктор.

**Побочные эффекты:** Передаёт ресурсы новому объекту.

**Как работает:** Передаёт внутреннее состояние; ресурс не открывается заново.

**Примечания:** У источника отсутствует состояние; IsOpen возвращает false.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

tui::ConsoleSurface Example(tui::ConsoleSurface &&Other) { return tui::ConsoleSurface(std::move(Other)); }
```

### `ConsoleSurface::operator=`

**Краткое описание:** Замените владение ресурсами перемещением.

**Параметры:** `Other` (`ConsoleSurface &&`, in-out); `ConsoleSurface &operator=(ConsoleSurface &&Other) noexcept`.

**Возвращает:** `ConsoleSurface &`: текущий объект.

**Побочные эффекты:** Освобождает или закрывает прежние ресурсы, принимает новые.

**Как работает:** При самоприсваивании сразу возвращает *this. Иначе вызывает Close для прежнего ресурса, игнорирует его статус и передаёт владение из Other.

**Примечания:** Если результат закрытия важен, явно вызовите и проверьте Close до присваивания.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::ConsoleSurface &Target, tui::ConsoleSurface &&Other) { Target = std::move(Other); }
```

### `ConsoleSurface::~ConsoleSurface`

**Краткое описание:** Освободите принадлежащие объекту ресурсы при завершении времени жизни.

**Параметры:** Нет.

**Возвращает:** Неприменимо: деструктор.

**Побочные эффекты:** Вызывает Close, игнорируя статус, и освобождает состояние.

**Как работает:** RAII обеспечивает вызов Close при обычном выходе из области видимости и раскрутке стека.

**Примечания:** Деструктор не выполняет Commit; для фиксации нужен явный вызов с проверкой результата.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::ConsoleSurface) {}
```

## Справочник

### `ConsoleSurface`

**Краткое описание:** Соберите рисунок во временном слое и зафиксируйте его в тексте консоли.

**Параметры:** Создаётся через `Open`; копирование запрещено, перемещение разрешено (`noexcept`).

**Возвращает:** Неприменимо: владеющий класс.

**Побочные эффекты:** Выделяет слой и память; деструктор закрывает незавершённую транзакцию.

**Как работает:** Владеет внутренним `LayerCanvas` и счётчиком `ScrollRows`. Перемещающее присваивание закрывает прежнее состояние без автоматического Commit.

**Примечания:** `Paint` уже показывает временный слой; подготовка не скрыта от экрана. `Close()` и уничтожение не заменяют `Commit()`.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::ConsoleSurface::Open(); }
```

### `ConsoleSurface::Open`

**Краткое описание:** Откройте транзакцию рисования в текущую консоль.

**Параметры:** `LeastCols`, `LeastRows` (`std::int32_t`, in) = `0`; `static std::expected<ConsoleSurface, StageError> Open(std::int32_t LeastCols = 0, std::int32_t LeastRows = 0) noexcept`.

**Возвращает:** `std::expected<ConsoleSurface, StageError>`.

**Побочные эффекты:** Открывает слой роли `OVERLAY`, создаёт прозрачный буфер и состояние.

**Как работает:** `LayerCanvas` запоминает позицию консольного курсора, полученную при открытии слоя. Порядок открытия связан с ранее напечатанным этим strand текстом через `console_order_mark()`.

**Примечания:** Отрицательные минимумы отвергаются; размеры относятся ко всей доступной консоли.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::ConsoleSurface::Open(20, 4); }
```

### `ConsoleSurface::Size`

**Краткое описание:** Получите размер консоли транзакции.

**Параметры:** Нет; `core::Size Size() const noexcept`.

**Возвращает:** `core::Size`: фактический размер либо `{}` без состояния.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Size Example(const tui::ConsoleSurface &Screen) { return Screen.Size(); }
```

### `ConsoleSurface::IsOpen`

**Краткое описание:** Проверьте, открыта ли транзакция локально.

**Параметры:** Нет; `bool IsOpen() const noexcept`.

**Возвращает:** `bool`: наличие открытого canvas.

**Побочные эффекты:** Нет.

**Примечания:** После успешного `Commit()` — `false`.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::ConsoleSurface &Screen) { return Screen.IsOpen(); }
```

### `ConsoleSurface::Glass`

**Краткое описание:** Получите тип устройства вывода.

**Параметры:** Нет; `GlassKind Glass() const noexcept`.

**Возвращает:** `GlassKind`: режим сервера либо `Pixels` без состояния.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::GlassKind Example(const tui::ConsoleSurface &Screen) { return Screen.Glass(); }
```

### `ConsoleSurface::Cursor`

**Краткое описание:** Получите сохранённую позицию продолжения консоли с поправкой на прокрутку.

**Параметры:** Нет; `std::expected<core::Point, box::error> Cursor() const noexcept`.

**Возвращает:** `std::expected<core::Point, box::error>`: позиция или `invalid_state`.

**Побочные эффекты:** Нет.

**Как работает:** Берёт сохранённый `LayerCanvas::Cursor`, затем уменьшает Y на число запрошенных `ScrollUp`, ограничивая результат снизу нулём. X остаётся прежним.

**Примечания:** Не опрашивает текущий курсор сервера. Каретка, пришедшая из `Frame`, сама по себе не меняет это сохранённое значение.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example(const tui::ConsoleSurface &Screen) { return Screen.Cursor(); }
```

### `ConsoleSurface::Paint`

**Краткое описание:** Замените прямоугольник временного слоя непрозрачными ячейками кадра.

**Параметры:** `Origin` (`core::Point`, in): начало назначения; `FrameBuffer` (`const Frame &`, in); `Region` (`core::Rect`, in) в перегрузке, иначе весь кадр. Обе перегрузки возвращают `box::status` и объявлены `noexcept`.

**Возвращает:** `box::status`: успех или ошибка; `out_of_range` при выходе прямоугольника за границы, `invalid_state` после закрытия.

**Побочные эффекты:** Заменяет локальные ячейки и каретку, публикует полный временный слой.

**Как работает:** Как `Surface::Composite`, проверяет полное вхождение области источника и назначения. При копировании принудительно ставит `Visibility::Opaque`, сохраняя цвета, глиф и отметку; переносит каретку из выбранной области. После замены публикует буфер.

**Примечания:** Флаг прозрачности кадра не сохраняется; специальные значения цветов не преобразуются в RGB. При ошибке публикации локальное изменение остаётся. Отсечения границ нет.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::ConsoleSurface &Screen, const tui::Frame &Picture) {
    return Screen.Paint({0, 0}, Picture);
}
```

### `ConsoleSurface::SetCursor`

**Краткое описание:** Задайте позицию каретки слоя и продолжения консоли.

**Параметры:** `At` (`core::Point`, in): координаты внутри консоли; `box::status SetCursor(core::Point At) noexcept`.

**Возвращает:** `box::status`: успех, `invalid_state`, `out_of_range` либо ошибка публикации.

**Побочные эффекты:** Меняет сохранённый курсор и каретку буфера, публикует слой.

**Как работает:** Вызывает `Frame::SetCaret`; при успехе сохраняет `At` и отправляет слой. При недопустимой координате прежняя каретка локального кадра снимается, но сохранённый курсор не меняется и кадр не отправляется.

**Примечания:** Не применяет накопленную прокрутку к аргументу. `Cursor()` затем вычитает её из Y; учитывайте это при выборе координат.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::ConsoleSurface &Screen) { return Screen.SetCursor({0, 2}); }
```

### `ConsoleSurface::ScrollUp`

**Краткое описание:** Добавьте одну строку к отложенной прокрутке консоли.

**Параметры:** Нет; `box::status ScrollUp() noexcept`.

**Возвращает:** `box::status`: успех, `invalid_state` либо `out_of_range` при достижении высоты консоли.

**Побочные эффекты:** Увеличивает локальный `ScrollRows`; ничего не рисует и не отправляет.

**Как работает:** Проверяет открытие и предел счётчика, затем увеличивает его. Буфер слоя не сдвигается; сервер получает накопленное число только при `Commit`.

**Примечания:** После N вызовов разрешено ещё `Height - N`; отдельного сброса счётчика нет.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::ConsoleSurface &Screen) { return Screen.ScrollUp(); }
```

### `ConsoleSurface::Commit`

**Краткое описание:** Зафиксируйте слой в базовом тексте консоли и завершите транзакцию.

**Параметры:** Нет; `box::status Commit() noexcept`.

**Возвращает:** `box::status`: подтверждённый ответ сервера или ошибка.

**Побочные эффекты:** Переносит рисунок в консоль, применяет накопленную прокрутку, закрывает временный слой при успехе.

**Как работает:** Передаёт `ScrollRows` в `LayerCanvas::Commit`. Клиент посылает упорядоченный запрос, сервер забирает доступный кадр и переносит его в текстовую модель; успешный ответ закрывает локальный слой и общую память.

**Примечания:** До Commit должен быть опубликован кадр через `Paint` или `SetCursor`, иначе сервер отказывает. Ошибка не помечает canvas закрытым, но не доказывает отсутствие серверного эффекта при транспортном сбое. Отметки ячеек не становятся частью текста.

**Типичное применение:** Проверьте каждый статус рисования, затем результат Commit; после успеха повторный Commit даст `invalid_state`.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::ConsoleSurface &Screen) { return Screen.Commit(); }
```

### `ConsoleSurface::Close`

**Краткое описание:** Отмените незавершённую транзакцию и закройте временный слой.

**Параметры:** Нет; `box::status Close() noexcept`.

**Возвращает:** `box::status`: результат закрытия; повторный вызов успешен.

**Побочные эффекты:** Удаляет временный слой без переноса в базовый текст.

**Как работает:** Вызывает закрытие canvas и слоя; накопленная прокрутка не фиксируется. После успешного Commit слой уже закрыт, поэтому Close ничего не отправляет.

**Пример:**

```cpp
#include "painter/tui/console_surface.h"

namespace tui = painter::tui;
namespace core = painter::core;

box::status Example(tui::ConsoleSurface &Screen) { return Screen.Close(); }
```
