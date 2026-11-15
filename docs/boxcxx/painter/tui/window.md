# painter::tui::window — оформление текстового окна

## Назначение

[Заголовок](../../../../src/userspace/painter/include/painter/tui/window.h),
[реализация](../../../../src/userspace/painter/src/tui/window.cpp).

Модуль рисует общее оформление окна на готовом кадре: фон, рамку,
подписи и тень. Он не владеет поверхностью и не содержит дерева элементов,
фокуса или событий. Перерисовка, размещение содержимого и ввод остаются
в программе.

Подписи обрабатываются как UTF-8 через [text](text.md). Многоточие
автоматически не добавляется. Тень меняет уже нарисованный кадр; повторное
рисование без восстановления фона повторно затемняет подложку. Для
изменяемого кадра используйте внешнюю синхронизацию.

## Справочник

### `WindowPaper`

**Краткое описание:** Используйте установленный цвет оформления: фон окна.

**Параметры:** Нет; `inline constexpr core::Color WindowPaper = core::Color::Rgb(0x24, 0x24, 0x24)`.

**Возвращает:** Неприменимо: константа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Color Ink = tui::WindowPaper;
```

### `WindowRule`

**Краткое описание:** Используйте установленный цвет оформления: линии рамки.

**Параметры:** Нет; `inline constexpr core::Color WindowRule = core::Color::Rgb(0x9A, 0x9A, 0x9A)`.

**Возвращает:** Неприменимо: константа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Color Ink = tui::WindowRule;
```

### `WindowInk`

**Краткое описание:** Используйте установленный цвет оформления: обычный текст.

**Параметры:** Нет; `inline constexpr core::Color WindowInk = core::Color::Rgb(0xD4, 0xD4, 0xD4)`.

**Возвращает:** Неприменимо: константа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Color Ink = tui::WindowInk;
```

### `TitleInk`

**Краткое описание:** Используйте установленный цвет оформления: заголовок.

**Параметры:** Нет; `inline constexpr core::Color TitleInk = core::Color::Rgb(0xF4, 0xF4, 0xF4)`.

**Возвращает:** Неприменимо: константа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Color Ink = tui::TitleInk;
```

### `HintInk`

**Краткое описание:** Используйте установленный цвет оформления: подсказка и примечание.

**Параметры:** Нет; `inline constexpr core::Color HintInk = core::Color::Rgb(0x8C, 0x8C, 0x8C)`.

**Возвращает:** Неприменимо: константа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Color Ink = tui::HintInk;
```

### `ChosenPaper`

**Краткое описание:** Используйте установленный цвет оформления: фон выбранной строки.

**Параметры:** Нет; `inline constexpr core::Color ChosenPaper = core::Color::Rgb(0x2E, 0x4A, 0x6A)`.

**Возвращает:** Неприменимо: константа.

**Побочные эффекты:** Нет.

**Примечания:** DrawWindow не выбирает строки: константа доступна программе для собственного списка.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Color Ink = tui::ChosenPaper;
```

### `Centred`

**Краткое описание:** Разместите прямоугольник по центру указанного размера экрана.

**Параметры:** `Screen` (`core::Size`, in); `Width`, `Height` (`std::int32_t`, in): желаемый размер; `core::Rect Centred(core::Size Screen, std::int32_t Width, std::int32_t Height) noexcept`.

**Возвращает:** `core::Rect`: рассчитанная область.

**Побочные эффекты:** Нет.

**Как работает:** Ограничивает каждую сторону диапазоном от 1 до `max(1, Screen.side)`. Начало вычисляет как половину разности размера экрана и ограниченной стороны.

**Примечания:** Для неположительного Screen результат всё равно имеет минимум 1 × 1 и не гарантирует вхождения в экран.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Rect Example() { return tui::Centred({80, 25}, 40, 10); }
```

### `DrawWindow`

**Краткое описание:** Нарисуйте окно с рамкой, подписями и тенью.

**Параметры:** `Out` (`Frame &`, in-out); `Area` (`core::Rect`, in); `Title`, `Note`, `Hint` (`std::string_view`, in); `core::Rect DrawWindow(Frame &Out, core::Rect Area, std::string_view Title, std::string_view Note, std::string_view Hint)`.

**Возвращает:** `core::Rect`: `{Area.X + 2, Area.Y + 1, Area.Width - 4, Area.Height - 2}`.

**Побочные эффекты:** Затемняет ячейки справа и снизу, заполняет окно, заменяет рамку и подписи; форматирование может выделять память и передавать исключения.

**Как работает:** Тень смешивает оба цвета прежних ячеек с чёрным при весе 72 из 255. Затем рисует фон и рамку глифами, выводит Title и Hint через PutText с пробелами вокруг; Note помещает справа только если после Title хватает места с зазором. Возвращает внутреннюю область с боковым отступом в две ячейки.

**Примечания:** Не создаёт слой, окно GUI, обработчик ввода или сохранённую подложку. Отдельные записи отсекаются Frame, но возвращённый Inside не отсекается. Для непустого Inside задавайте Width >= 5 и Height >= 3; координаты с запасом под тень должны быть представимы в int32_t.

**Типичное применение:** Используйте Centred → DrawWindow → собственное содержимое Inside; публикация выполняется поверхностью.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Rect Example(tui::Frame &Out) {
    return tui::DrawWindow(Out, tui::Centred(Out.Size(), 40, 10),
                           "Painter", "demo", "esc closes");
}
```

### `DrawRule`

**Краткое описание:** Проведите разделитель с соединением с рамкой окна.

**Параметры:** `Out` (`Frame &`, in-out); `Inside` (`core::Rect`, in): результат DrawWindow; `Row` (`std::int32_t`, in): абсолютная строка кадра; `void DrawRule(Frame &Out, core::Rect Inside, std::int32_t Row)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет ячейки разделителя и два соединения с рамкой.

**Как работает:** Восстанавливает левую рамку как `Inside.X - 2`, правую как `Inside.X + Inside.Width + 1`. Между ними ставит RuleAcross, на концах TeeLeft и TeeRight.

**Примечания:** Не проверяет Row на попадание во внутреннюю область окна: отсечение выполняется только границами кадра.

**Пример:**

```cpp
#include "painter/tui/window.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Frame &Out) {
    const auto Inside = tui::DrawWindow(Out, {1, 1, 20, 8}, "Title", "", "");
    tui::DrawRule(Out, Inside, Inside.Y + 2);
}
```
