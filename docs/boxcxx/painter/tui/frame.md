# painter::tui::frame — кадр и ячейки

## Назначение

[Заголовок](../../../../src/userspace/painter/include/painter/tui/frame.h),
[реализация](../../../../src/userspace/painter/src/tui/frame.cpp).

`Frame` владеет прямоугольным массивом `Cell` в памяти программы. Рисование
меняет этот массив; показ выполняет [Stage](stage.md), [Surface](surface.md)
или [ConsoleSurface](console_surface.md). Размер фиксируется при создании.

Одна ячейка содержит один байтовый глиф, два цвета и отметку. Клеточные
координаты отсчитываются от левого верхнего угла. Полублочные операции
используют тот же X и удвоенный Y: на ячейку приходится одна точка по
горизонтали и две по вертикали. Это псевдографика шрифта, а не доступ к
физическим пикселям.

Все примеры — самостоятельные единицы трансляции C++23. Функции с аргументом
`Frame &` предполагают успешно созданный, не перемещённый кадр. Доступ к одному
изменяемому кадру из нескольких потоков требует внешней синхронизации.

## Владение и время жизни

### `Frame::Frame(Frame &&)`

**Краткое описание:** Передайте владение при создании нового объекта.

**Параметры:** `Other` (`Frame &&`, in-out); перемещающий конструктор объявлен `noexcept`.

**Возвращает:** Неприменимо: конструктор.

**Побочные эффекты:** Передаёт ресурсы новому объекту.

**Как работает:** Стандартное перемещение передаёт вектор ячеек, сохраняя значения остальных полей согласно их операциям перемещения.

**Примечания:** Исходный Frame больше не используйте для чтения и рисования: размеры могут остаться прежними при перемещённом массиве.

**Пример:**

```cpp
#include "painter/tui/frame.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

tui::Frame Example(tui::Frame &&Other) { return tui::Frame(std::move(Other)); }
```

### `Frame::operator=`

**Краткое описание:** Замените владение ресурсами перемещением.

**Параметры:** `Other` (`Frame &&`, in-out); `Frame &operator=(Frame &&Other) noexcept`.

**Возвращает:** `Frame &`: текущий объект.

**Побочные эффекты:** Освобождает или закрывает прежние ресурсы, принимает новые.

**Как работает:** Выполняет стандартное перемещающее присваивание полей, включая вектор ячеек.

**Примечания:** Не рассчитывайте на содержимое перемещённого источника.

**Пример:**

```cpp
#include "painter/tui/frame.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Frame &Target, tui::Frame &&Other) { Target = std::move(Other); }
```

### `Frame::~Frame`

**Краткое описание:** Освободите принадлежащие объекту ресурсы при завершении времени жизни.

**Параметры:** Нет.

**Возвращает:** Неприменимо: деструктор.

**Побочные эффекты:** Уничтожает вектор ячеек.

**Как работает:** Стандартный деструктор освобождает память через деструктор вектора.

**Пример:**

```cpp
#include "painter/tui/frame.h"
#include <utility>

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Frame) {}
```

## Справочник

### `TextStyle`

**Краткое описание:** Задайте цвета текста и фона для клеточных операций.

**Параметры:** Агрегат: `Foreground` (`core::Color`) = `Colors::LightGray`; `Background` (`core::Color`) = `Colors::Black`.

**Возвращает:** Неприменимо: структура значений.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::TextStyle Style{core::Colors::White, core::Colors::Blue};
```

### `GlassKind`

**Краткое описание:** Выберите режим интерпретации рисунка ячейки.

**Параметры:** Значения `Text` = `DISPLAY_GLASS_TEXT`, `Pixels` = `DISPLAY_GLASS_PIXELS`; базовый тип `std::uint8_t`.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Примечания:** `Pixels` означает framebuffer как устройство вывода; TUI продолжает рисовать глифами. Режим сообщает DisplayServer.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::GlassKind Glass = tui::GlassKind::Text;
```

### `Visibility`

**Краткое описание:** Укажите участие всей ячейки в композиции.

**Параметры:** `Transparent`, `Opaque`; базовый тип `std::uint8_t`.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Примечания:** Прозрачность ячейки и прозрачность её цветов — разные свойства.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Visibility Coverage = tui::Visibility::Transparent;
```

### `CellMark`

**Краткое описание:** Выберите отметку поверх глифа ячейки.

**Параметры:** `None`, `Under` (линия снизу), `Bar` (линия слева); соответствуют `DISPLAY_CELL_MARK_*`. Базовый тип `std::uint8_t`.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Примечания:** `MarkColor` задаёт собственный цвет отметки. При переносе в текст консоли отметка не сохраняется.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::CellMark Mark = tui::CellMark::Under;
```

### `Cell`

**Краткое описание:** Опишите глиф, цвета, видимость и отметку одной ячейки.

**Параметры:** Агрегат: `Glyph` (`char`) = `' '`; `Foreground` = `LightGray`, `Background` = `Black`; `VisibilityMode` = `Opaque`; `Mark` = `None`; `MarkColor` = `LightGray`. Все три цвета имеют тип `core::Color`.

**Возвращает:** Неприменимо: структура данных.

**Побочные эффекты:** Нет.

**Примечания:** `Glyph` — байтовый индекс шрифта, не UTF-8. `operator==` сравнивает все поля, включая цвета скрытой ячейки.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Cell Value{'A', core::Colors::White, core::Colors::Black};
```

### `Cell::TransparentCell`

**Краткое описание:** Создайте ячейку, показывающую содержимое нижнего слоя.

**Параметры:** Нет; `static constexpr Cell TransparentCell() noexcept`.

**Возвращает:** `Cell`: пробел, оба цвета `Color::Through()`, видимость `Transparent`, отметка `None`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Cell Empty = tui::Cell::TransparentCell();
```

### `Cell::IsTransparent`

**Краткое описание:** Проверьте прозрачность всей ячейки.

**Параметры:** Нет; `bool IsTransparent() const noexcept`.

**Возвращает:** `bool`: равно ли `VisibilityMode` значению `Transparent`.

**Побочные эффекты:** Нет.

**Примечания:** Цвета и отметка в проверке не участвуют.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(const tui::Cell &Value) { return Value.IsTransparent(); }
```

### `Cell::operator==`

**Краткое описание:** Сравните все поля двух ячеек.

**Параметры:** Два аргумента `const Cell &`, in; `friend bool operator==(const Cell &, const Cell &) noexcept = default`.

**Возвращает:** `bool`: равны ли глиф, цвета, видимость, отметка и её цвет.

**Побочные эффекты:** Нет.

**Примечания:** Равенство полей не является сравнением видимого изображения; разные коды и цвета могут выглядеть одинаково на конкретном устройстве.

**Пример:**

```cpp
#include "painter/tui/frame.h"

bool Example(const painter::tui::Cell &A, const painter::tui::Cell &B)
{
    return A == B;
}
```

### `Frame`

**Краткое описание:** Храните прямоугольный кадр и необязательную позицию каретки.

**Параметры:** Создаётся фабрикой `Create`; копирование запрещено, перемещение разрешено (`noexcept`), деструктор стандартный.

**Возвращает:** Неприменимо: владеющий класс.

**Побочные эффекты:** Создание выделяет память, уничтожение освобождает её.

**Примечания:** Кадр не открывает слой и не рисует экран. Внутренних блокировок нет. После перемещения используйте исходный объект для уничтожения или присваивания: согласованность его размеров и перемещённого массива для рисования не гарантируется.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::Frame::Create({20, 4}); }
```

### `Frame::Create`

**Краткое описание:** Создайте кадр заданного размера.

**Параметры:** `Bounds` (`core::Size`, in): положительные ширина и высота в ячейках; `static box::result<Frame> Create(core::Size Bounds) noexcept`.

**Возвращает:** `box::result<Frame>`: кадр или `out_of_range` / `no_memory`.

**Побочные эффекты:** Выделяет массив `Cell`.

**Как работает:** Проверяет пустой размер, возможность удвоения высоты и число ячеек. Создаёт вектор из стандартных `Cell`, переводя `bad_alloc` в `no_memory`, а `length_error` в `out_of_range`.

**Примечания:** Начальные ячейки непрозрачны, каретки нет, `Glass()` равен `Pixels`.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example() { return tui::Frame::Create({80, 25}); }
```

### `Frame::Size`

**Краткое описание:** Получите размер кадра в ячейках.

**Параметры:** Нет; `core::Size Size() const noexcept`.

**Возвращает:** `core::Size`: сохранённые ширина и высота.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Size Example(const tui::Frame &Out) { return Out.Size(); }
```

### `Frame::PixelHeight`

**Краткое описание:** Получите высоту кадра в единицах полублоков.

**Параметры:** Нет; `std::int32_t PixelHeight() const noexcept`.

**Возвращает:** `std::int32_t`: `Size().Height * 2`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

std::int32_t Example(const tui::Frame &Out) { return Out.PixelHeight(); }
```

### `Frame::Glass`

**Краткое описание:** Получите назначенный кадру режим вывода.

**Параметры:** Нет; `GlassKind Glass() const noexcept`.

**Возвращает:** `GlassKind`: текущий режим.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::GlassKind Example(const tui::Frame &Out) { return Out.Glass(); }
```

### `Frame::SetGlass`

**Краткое описание:** Назначьте режим для выбора способов аппроксимации рисунка.

**Параметры:** `Value` (`GlassKind`, in); `void SetGlass(GlassKind Value) noexcept`.

**Возвращает:** `void`.

**Побочные эффекты:** Меняет метаданные кадра.

**Примечания:** Не меняет видеорежим и не перерисовывает существующие ячейки. Самостоятельному кадру назначайте `Surface::Glass()` или `ConsoleSurface::Glass()` перед работой `FineCanvas`.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Frame &Out) { Out.SetGlass(tui::GlassKind::Text); }
```

### `Frame::Clear`

**Краткое описание:** Заполните кадр непрозрачными пробелами и уберите каретку.

**Параметры:** `Style` (`TextStyle`, in), по умолчанию `{}`; `void Clear(TextStyle Style = {})`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет все ячейки, сбрасывает отметки ячеек и `Caret()`.

**Как работает:** Линейно заполняет вектор значением `Cell{' ', Style.Foreground, Style.Background}`. Позиция каретки сбрасывается отдельно.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Frame &Out) { Out.Clear({core::Colors::White, core::Colors::Blue}); }
```

### `Frame::ClearTransparent`

**Краткое описание:** Очистите кадр прозрачными ячейками и уберите каретку.

**Параметры:** Нет; `void ClearTransparent() noexcept`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет все ячейки и сбрасывает каретку.

**Как работает:** Заполняет вектор значениями `Cell::TransparentCell()`. Это подготовка буфера, а не команда удаления серверного слоя.

**Типичное применение:** Используйте перед рисованием фигуры для `Surface::Composite`.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Frame &Out) { Out.ClearTransparent(); }
```

### `Frame::Put`

**Краткое описание:** Замените одну ячейку внутри кадра.

**Параметры:** `At` (`core::Point`, in), `Value` (`Cell`, in); перегрузка `bool Put(core::Point At, char Glyph, TextStyle Style = {})` создаёт непрозрачную ячейку без отметки.

**Возвращает:** `bool`: `true`, если координата внутри кадра.

**Побочные эффекты:** Заменяет ячейку при допустимых координатах.

**Как работает:** Проверяет обе координаты; вне кадра возвращает `false` без записи. Перегрузка с `Cell` сохраняет все переданные поля.

**Примечания:** Каретка кадра не меняется.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(tui::Frame &Out) { return Out.Put({1, 1}, 'A'); }
```

### `Frame::Fill`

**Краткое описание:** Заполните пересечение прямоугольника с кадром.

**Параметры:** `Area` (`core::Rect`, in), `Value` (`Cell`, in); перегрузка `std::size_t Fill(core::Rect Area, char Glyph, TextStyle Style = {})`.

**Возвращает:** `std::size_t`: число заменённых ячеек; `0` для пустого пересечения.

**Побочные эффекты:** Изменяет ячейки пересечения.

**Как работает:** Сначала вычисляет `Area.Intersect(Size())`, затем проходит строки и столбцы пересечения. С `char` создаются непрозрачные ячейки без отметок.

**Примечания:** Не сбрасывает `Caret()`.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

std::size_t Example(tui::Frame &Out) { return Out.Fill({-1, 0, 4, 2}, ' '); }
```

### `Frame::Write`

**Краткое описание:** Запишите строку готовых глифов в одну строку кадра.

**Параметры:** `At` (`core::Point`, in), `Text` (`std::string_view`, in), `Style` (`TextStyle`, in) = `{}`; `std::size_t Write(core::Point At, std::string_view Text, TextStyle Style = {})`.

**Возвращает:** `std::size_t`: число действительно записанных ячеек.

**Побочные эффекты:** Заменяет ячейки непрозрачными глифами с указанными цветами и без отметок.

**Как работает:** Читает по одному байту. Слева пропускает байты, соответствующие отрицательным столбцам; справа прекращает запись. Недопустимая строка `At.Y` сразу даёт `0`.

**Примечания:** Не декодирует UTF-8, не выполняет табуляцию и перенос. Для UTF-8 используйте [PutText](text.md).

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

std::size_t Example(tui::Frame &Out) { return Out.Write({0, 0}, "Painter"); }
```

### `Frame::Centre`

**Краткое описание:** Запишите строку глифов с горизонтальным центрированием.

**Параметры:** `Row` (`std::int32_t`, in), `Text` (`std::string_view`, in), `Style` (`TextStyle`, in) = `{}`; `std::size_t Centre(std::int32_t Row, std::string_view Text, TextStyle Style = {})`.

**Возвращает:** `std::size_t`: результат `Write`.

**Побочные эффекты:** Заменяет ячейки строки.

**Как работает:** Если число байтов меньше ширины кадра, выбирает начало `(Width - Text.size()) / 2`; иначе начинает с нуля. Вызывает `Write`, который отсекает правый остаток.

**Примечания:** Центрирование считает байты, поэтому предназначено для готовых однобайтовых глифов.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

std::size_t Example(tui::Frame &Out) { return Out.Centre(0, "Painter"); }
```

### `Frame::PutPixel`

**Краткое описание:** Задайте цвет одной половины ячейки.

**Параметры:** `Pixel` (`core::Point`, in): X в столбцах, Y в полустро́ках; `Ink` (`core::Color`, in); `bool PutPixel(core::Point Pixel, core::Color Ink)`.

**Возвращает:** `bool`: находится ли точка внутри `Width × PixelHeight()`.

**Побочные эффекты:** Меняет глиф на `Glyphs::UpperHalf`, видимость на `Opaque` и один цвет.

**Как работает:** Чётный Y заменяет `Foreground`, нечётный — `Background` ячейки `{X, Y / 2}`. Второй цвет сохраняется, даже если ранее ячейка содержала текст.

**Примечания:** Отметка и каретка сохраняются. После `ClearTransparent` незатронутая половина остаётся `Through`.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(tui::Frame &Out) { return Out.PutPixel({2, 3}, core::Colors::Red); }
```

### `Frame::BlendPixel`

**Краткое описание:** Смешайте цвет с одной половиной ячейки.

**Параметры:** `Pixel` (`core::Point`, in), `Ink` (`core::Color`, in), `Weight` (`std::uint8_t`, in): вес 0–255; `bool BlendPixel(core::Point Pixel, core::Color Ink, std::uint8_t Weight)`.

**Возвращает:** `bool`: допустима ли координата.

**Побочные эффекты:** Устанавливает верхний полублок и `Opaque`, смешивает выбранный цвет.

**Как работает:** Выбирает половину по чётности Y, как `PutPixel`, и присваивает ей `Color::Mix(Current, Ink, Weight)`. Смешивание учитывает непрозрачность цветов.

**Примечания:** Даже нулевой вес при допустимой координате меняет глиф и видимость.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(tui::Frame &Out) { return Out.BlendPixel({2, 3}, core::Colors::Red, 128); }
```

### `Frame::PutPixels`

**Краткое описание:** Замените цвета всех половин кадра из построчного массива.

**Параметры:** `Pixels` (`std::span<const core::Color>`, in): ровно `Width * Height * 2` цветов; `bool PutPixels(std::span<const core::Color> Pixels)`.

**Возвращает:** `bool`: совпадает ли длина массива с требуемой.

**Побочные эффекты:** Для всего кадра ставит верхний полублок, `Opaque` и оба цвета.

**Как работает:** Пара строк пиксельного массива образует строку ячеек: верхняя строка даёт `Foreground`, нижняя — `Background`. Несовпадение размера возвращает `false` до изменения кадра.

**Примечания:** Отметки ячеек и `Caret()` не сбрасываются.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(tui::Frame &Out, std::span<const core::Color> Pixels) { return Out.PutPixels(Pixels); }
```

### `Frame::Read`

**Краткое описание:** Получите копию ячейки по координате.

**Параметры:** `At` (`core::Point`, in); `Cell Read(core::Point At) const noexcept`.

**Возвращает:** `Cell`: ячейка кадра или `Cell{}` вне его границ.

**Побочные эффекты:** Нет.

**Примечания:** Значение вне границ — непрозрачный пробел стандартных цветов, а не прозрачная ячейка.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

tui::Cell Example(const tui::Frame &Out) { return Out.Read({0, 0}); }
```

### `Frame::SetCaret`

**Краткое описание:** Установите или снимите единственную каретку кадра.

**Параметры:** `At` (`std::optional<core::Point>`, in); `bool SetCaret(std::optional<core::Point> At) noexcept`.

**Возвращает:** `bool`: `true` для допустимой координаты и `std::nullopt`; `false` вне кадра.

**Побочные эффекты:** Меняет позицию каретки.

**Как работает:** Недопустимая координата снимает прежнюю каретку, а не оставляет её на месте. Содержимое ячеек не меняется.

**Примечания:** Это позиция каретки слоя; `CellMark` — отдельная отметка конкретной ячейки.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(tui::Frame &Out) { return Out.SetCaret(core::Point{1, 0}); }
```

### `Frame::Caret`

**Краткое описание:** Получите текущую позицию каретки.

**Параметры:** Нет; `std::optional<core::Point> Caret() const noexcept`.

**Возвращает:** `std::optional<core::Point>`: позиция или `std::nullopt`.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/frame.h"

namespace tui = painter::tui;
namespace core = painter::core;

auto Example(const tui::Frame &Out) { return Out.Caret(); }
```
