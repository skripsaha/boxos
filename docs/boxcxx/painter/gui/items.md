# painter::gui::items

## Назначение

Заголовок: [`painter/gui/items.h`](../../../../src/userspace/painter/include/painter/gui/items.h).
Реализация: [`src/gui/items.cpp`](../../../../src/userspace/painter/src/gui/items.cpp),
холст — [`src/gui/canvas.cpp`](../../../../src/userspace/painter/src/gui/canvas.cpp).

Элементы как объекты. Элемент создаётся в области, в группе или в кадре,
получает свои свойства методами `Set…` (они возвращают сам объект, поэтому
цепляются: `Title.SetPlace({16, 8}).SetOpacity(0.8f)`) и исчезает вместе с
объектом.

Номера элементов на языке выбирает Painter (начиная с 2²⁰), программа их
не видит — кроме `Number()`, чтобы узнать свой элемент в событиях.

Два режима владения:

- **хранимый элемент** — создан в области или в хранимой группе. Объект владеет элементом: перемещён — элемент переходит с ним; уничтожен или `Remove()` — элемент удалён из области;
- **элемент кадра** — создан в `gui::Frame` внутри `Area::Draw`. Объект только описывает, каким элемент будет в этом кадре; элементом владеет кадр, и он сам решает, что изменилось ([frame](frame.md)).

Объект, чья область закрыта, ничего не делает.

Важное следствие: элемент живёт, пока жив объект. Временный объект
(`Screen.Text("…").SetPlace(…);` без сохранения) создаёт элемент и тут же
удаляет его. Храните объекты в переменных или полях класса.

## Базовые классы

### `Item`

**Краткое описание:** Хранит номер элемента и связь с его областью или кадром.

**Параметры:** Конструктор по умолчанию создаёт пустой объект; настоящие создаёт `Makes`.

**Возвращает:** Неприменимо: класс. Копировать нельзя, перемещать можно.

**Побочные эффекты:** Деструктор и перемещающее присваивание удаляют прежний хранимый элемент.

**Пример:** См. методы ниже.

### `Item::operator bool`

**Краткое описание:** Сообщает, что элемент создан и его область ещё открыта.

**Параметры:** Нет.

**Возвращает:** (`bool`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Update(painter::gui::Text &Line) { if (Line) Line.SetText("обновлено"); }
```

### `Item::Number`

**Краткое описание:** Возвращает номер элемента на языке элементов.

**Параметры:** Нет.

**Возвращает:** (`std::uint32_t`) номер; 0 у пустого объекта.

**Побочные эффекты:** Нет.

**Типичное применение:** Сравнить с `Event::Item`, чтобы понять, какую кнопку нажали; передать номер в методы `Changes`, которых нет у объекта.

**Пример:**
```cpp
#include "painter/gui/area.h"
#include "painter/gui/items.h"

bool Pressed(const painter::gui::Event &Heard, const painter::gui::Text &Button)
{
    return Heard.What == painter::gui::Event::Kind::Press && Heard.Item == Button.Number();
}
```

### `Item::Remove`

**Краткое описание:** Удаляет элемент из области сейчас.

**Параметры:** Нет.

**Возвращает:** `void`.

**Побочные эффекты:** Для хранимого элемента — запись удаления и возврат номера; объект становится пустым. Для элемента кадра — только отвязывает объект.

**Как работает:** Если элемент уже ушёл вместе со своей группой (дисплей удалил его с группой), удаление повторно не записывается.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Dismiss(painter::gui::Shape &Banner) { Banner.Remove(); }
```

## Общие методы: `Words<Self>`

Все методы возвращают `Self &` и ничего не делают у пустого объекта.

### `SetPlace` / `SetSize`

**Краткое описание:** Задают место элемента от угла родителя и его размер, в точках.

**Параметры:**
- `At` (`core::PointF`): место.
- `Size` (`core::PointF`): размер; сторона `ByContent` — по содержимому.

**Возвращает:** (`Self &`) этот объект.

**Побочные эффекты:** Записывают свойство, если оно изменилось.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Lay(painter::gui::Shape &Plate) { Plate.SetPlace({16, 16}).SetSize({280, 120}); }
```

### `SetOpacity` / `Show` / `Hide`

**Краткое описание:** Задают непрозрачность элемента (0–1), показывают и скрывают его.

**Параметры:**
- `Opacity` (`float`): 0–1.

**Возвращает:** (`Self &`).

**Побочные эффекты:** Записывают свойство. Скрытие внутри `Animate` — плавное исчезновение.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Toggle(painter::gui::Group &Menu, bool Open) { Open ? Menu.Show() : Menu.Hide(); }
```

### `SetFocusable` / `SetDisabled` / `Focus`

**Краткое описание:** Делают элемент доступным или недоступным клавиатуре, неактивным, или переводят на него фокус.

**Параметры:**
- `Reached` (`bool`), `Disabled` (`bool`).

**Возвращает:** (`Self &`).

**Побочные эффекты:** Свойства; `Focus` — запись в шаг.

**Примечания:** Кнопки и поля доступны клавиатуре без слов. Фокус переходит по Tab в порядке создания элементов.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Start(painter::gui::Text &Name) { Name.Focus(); }
```

### `SetTurn` / `SetScale` / `SetAnchor`

**Краткое описание:** Поворачивают и масштабируют элемент вокруг его точки привязки.

**Параметры:**
- `Degrees` (`float`): по часовой.
- `Scale` (`core::PointF`): по осям.
- `At` (`core::PointF`): точка привязки от угла элемента, в точках.

**Возвращает:** (`Self &`).

**Побочные эффекты:** Свойства.

**Как работает:** Пока поворот или масштаб идёт в анимации, дисплей кладёт уже нарисованную картинку и перерисовывает её чётко, когда движение закончится.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Spin(painter::gui::Shape &Wheel) { Wheel.SetAnchor({32, 32}).SetTurn(90); }
```

### `SetRole` / `SetCaption`

**Краткое описание:** Меняют роль элемента и задают его словесное описание.

**Параметры:**
- `Role` (`gui::Role`).
- `Utf8` (`std::string_view`): подпись.

**Возвращает:** (`Self &`).

**Побочные эффекты:** Свойства.

**Примечания:** Подпись видна на текстовой странице вместо фигуры, картинки или холста и предназначена для чтения вслух. Секретный текст не читается никогда.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Name(painter::gui::Picture &Logo) { Logo.SetCaption("Логотип BoxOS"); }
```

### `Key`

**Краткое описание:** Говорит, каким элементом прошлого кадра является этот элемент кадра.

**Параметры:**
- `Key` (`std::uint64_t`): ключ, устойчивый между кадрами (например, идентификатор строки).

**Возвращает:** (`Self &`).

**Побочные эффекты:** Только в кадре; у хранимого элемента ничего не делает.

**Пример:** См. [frame](frame.md).

## Краска: `PaintWords<Self>`

### `SetFill` / `SetFills` / `SetStroke` / `SetShadow` / `SetShadows`

**Краткое описание:** Задают заливки, обводку и тени фигуры или текста.

**Параметры:**
- `One` (`const Fill &`) или `Fills` (`std::span<const Fill>`): заливка или заливки, первая — нижняя.
- `Line` (`const Stroke &`).
- `One` (`const Shadow &`) или `Shadows` (`std::span<const Shadow>`).

**Возвращает:** (`Self &`).

**Побочные эффекты:** Свойства.

**Примечания:** У текста заливка — это цвет букв (вместо цвета роли). Значения описаны в [changes](changes.md).

**Пример:**
```cpp
#include "painter/gui/items.h"

void Warn(painter::gui::Text &Line) { Line.SetFill(painter::gui::Fill::Of(painter::core::Colors::Amber)); }
```

## Элементы

### `Shape`

**Краткое описание:** Фигура: прямоугольник (по умолчанию), эллипс или произвольный контур.

**Параметры:** Создаётся `Rectangle`, `Ellipse`, `Outline` у области, группы или кадра.

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/gui/area.h"
#include "painter/gui/items.h"

painter::gui::Shape Dot(painter::gui::Area &Screen)
{
    painter::gui::Shape Made = Screen.Ellipse();
    Made.SetPlace({10, 10}).SetSize({12, 12}).SetFill(painter::gui::Fill::OfTheme(painter::gui::ThemeColor::Accent));
    return Made;
}
```

### `Shape::SetRectangle` / `SetEllipse` / `SetOutline` / `SetCorners`

**Краткое описание:** Меняют форму фигуры и скругления углов.

**Параметры:**
- `Outline` (`const core::Path &`): контур в точках фигуры; `EvenOdd` (`bool`): правило дырок.
- `Round` (`const core::Corners &`): радиусы углов.

**Возвращает:** (`Shape &`).

**Побочные эффекты:** Свойства.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Pill(painter::gui::Shape &Plate) { Plate.SetSize({120, 32}).SetCorners(painter::core::Corners::All(16)); }
```

### `Text`

**Краткое описание:** Текст: строка UTF-8, набранная шрифтами системы или программы.

**Параметры:** Создаётся `Makes::Text(utf8, role)`; роль по умолчанию `Body`.

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Нет.

**Как работает:** Без размера текст занимает столько, сколько нужно (размер по содержимому). Текст в роли `Button` или `Field` носит собственную плашку вокруг слов.

**Пример:** См. [README](README.md).

### `Text::SetText` / `SetStyle` / `SetSpans` / `SetBox` / `SetSecret`

**Краткое описание:** Задают сам текст, его стиль, отрезки со своим стилем, раскладку и секретность.

**Параметры:**
- `Utf8` (`std::string_view`).
- `Style` (`const TextStyle &`): только то, что меняется относительно роли.
- `Spans` (`std::span<const Span>`).
- `Box` (`const TextBox &`).
- `Secret` (`bool`).

**Возвращает:** (`Text &`).

**Побочные эффекты:** Свойства.

**Примечания:** Внутри `Animate` смена текста — плавный переход старых букв в новые. Текст поля, отредактированный человеком, приходит событием `Event::Kind::Text`; программа может сказать его снова — ничего не уйдёт.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Mask(painter::gui::Text &Pin) { Pin.SetSecret(true).SetText("1234"); }
```

### `Picture`

**Краткое описание:** Картинка из блока памяти.

**Параметры:** Создаётся `Makes::Picture(block, fit, sharp)`.

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Нет.

**Пример:** См. [blocks](blocks.md).

### `Picture::SetPicture`

**Краткое описание:** Показывает в элементе другой блок картинки или меняет вписывание.

**Параметры:**
- `Block` (`const PictureBlock &`).
- `Fit` (`std::uint8_t`): `ITEM_FIT_STRETCH` (по умолчанию), `CONTAIN`, `COVER`, `CENTER`.
- `Sharp` (`bool`): пиксели квадратами при увеличении.

**Возвращает:** (`Picture &`).

**Побочные эффекты:** Свойство.

**Пример:**
```cpp
#include "painter/gui/items.h"

void Pixelart(painter::gui::Picture &Sprite, const painter::gui::PictureBlock &Block) { Sprite.SetPicture(Block, ITEM_FIT_CONTAIN, true); }
```

### `Canvas`

**Краткое описание:** Холст собственных пикселей программы — настоящих пикселей экрана.

**Параметры:** Создаётся `Makes::Canvas(core::Size Pixels)` у области или хранимой группы; в кадре холст не создаётся.

**Возвращает:** Неприменимо: класс. Копировать нельзя; перемещать можно.

**Побочные эффекты:** Создание открывает блок с двумя страницами; уничтожение удаляет элемент и закрывает блок.

**Как работает:** Две страницы: программа рисует на одной, пока дисплей компонует другую. Дисплей показывает страницу прямо оттуда, где программа её нарисовала, без копирования, и держит её, пока не получит следующую.

**Примечания:** Размер холста задаётся в пикселях. Чтобы холст был чётким, умножьте размер в точках на `Area::Scale()`. Если дисплей не дал блок, объект пустой.

**Пример:**
```cpp
#include <cmath>

#include "painter/gui/area.h"
#include "painter/gui/items.h"

painter::gui::Canvas Board(painter::gui::Area &Screen)
{
    const float Scale = Screen.Scale();
    painter::gui::Canvas Made = Screen.Canvas({static_cast<std::int32_t>(std::lround(300 * Scale)),
                                               static_cast<std::int32_t>(std::lround(200 * Scale))});
    Made.SetPlace({20, 20}).SetSize({300, 200});
    return Made;
}
```

### `Canvas::Size`

**Краткое описание:** Возвращает размер холста в пикселях.

**Параметры:** Нет.

**Возвращает:** (`core::Size`).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/gui/items.h"

std::int32_t Pixels(const painter::gui::Canvas &Picture) { return Picture.Size().Width * Picture.Size().Height; }
```

### `Canvas::Draw`

**Краткое описание:** Рисует следующую картинку холста на свободной странице.

**Параметры:**
- `Paint` (вызываемый объект `void(core::Canvas &)`): рисует на странице.
- `Changed` (`std::optional<core::Rect>`): если задан, изменилась только эта часть; остальное — картинка, показанная прежде. По умолчанию — весь холст, и каждый пиксель должен быть нарисован.

**Возвращает:** (`bool`) `false`, если область закрыта или блока нет.

**Побочные эффекты:** Может ждать, пока дисплей отпустит страницу (отправляя накопленные изменения, чтобы он смог её отпустить, и откладывая события для `Next`/`Wait`). Отдаёт нарисованную страницу дисплею (`BlockChanged`); на экран она попадёт со следующим `Present`.

**Как работает:** Рисуется страница, не отданная последней. Если задан `Changed`, сначала копируется из показанной страницы то, чего на этой странице нет: изменения прошлого шага (или вся картинка, если на странице ещё не рисовали). Дисплей компонует только изменившуюся часть.

**Примечания:** Страница — это `core::Canvas` поверх общей памяти; ссылку на неё нельзя хранить после `Paint`. Дисплей читает пиксели как есть и ограничивает свет каждого пикселя его непрозрачностью, поэтому ошибка рисования портит только свою картинку.

**Типичное применение:** Анимация по будильнику: на каждом `Alarm` — `Draw`, затем `Present`.

**Пример:**
```cpp
#include "painter/core/canvas.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"

bool Dot(painter::gui::Area &Screen, painter::gui::Canvas &Picture, painter::core::PointF At)
{
    namespace core = painter::core;
    const core::Rect Near{static_cast<std::int32_t>(At.X) - 6, static_cast<std::int32_t>(At.Y) - 6, 12, 12};
    const bool Drawn = Picture.Draw([&](core::Canvas &Page) {
        (void)Page.Fill(core::Path::Circle(At, 5), core::Colors::Coral);
    }, Near);
    (void)Screen.Present();
    return Drawn;
}
```

### `Cells`

**Краткое описание:** Сетка ячеек в виде консоли: символ, цвет, фон и отметка каждой ячейки, своя каретка.

**Параметры:** Создаётся `Makes::Cells(Cols, Rows)` у области или хранимой группы.

**Возвращает:** Неприменимо: класс.

**Побочные эффекты:** Нет.

**Примечания:** Уходят только изменившиеся ячейки. Обычно удобнее `painter::tui`, который построен на этом элементе.

**Пример:**
```cpp
#include "painter/gui/area.h"
#include "painter/gui/items.h"

painter::gui::Cells Terminal(painter::gui::Area &Screen) { return Screen.Cells(80, 25); }
```

### `Cells::SetGrid` / `SetCells` / `SetCaret`

**Краткое описание:** Меняют размер сетки, пишут отрезок ячеек и ставят каретку.

**Параметры:**
- `Cols`, `Rows` (`std::uint32_t`).
- `Row`, `Col` (`std::uint32_t`), `Run` (`std::span<const ItemCell>`).
- `At` (`std::optional<core::Point>`).

**Возвращает:** (`Cells &`).

**Побочные эффекты:** См. `Changes::Grid`, `Cells`, `Caret` в [changes](changes.md).

**Пример:**
```cpp
#include "painter/gui/items.h"

void Prompt(painter::gui::Cells &Grid) { Grid.SetCaret(painter::core::Point{2, 0}); }
```

### `Group`

**Краткое описание:** Группа элементов: они лежат в ней, видны сквозь неё как одно целое и обрезаются ею, если она обрезает.

**Параметры:** Создаётся `Makes::Group(role)`; в группе вложенная группа создаётся `Subgroup(role)` (в C++ имя `Group` внутри класса занято самим классом).

**Возвращает:** Неприменимо: класс. Элементы создаются в группе так же, как в области: `Rows.Text(…)`, `Rows.Rectangle(…)`.

**Побочные эффекты:** Удаление группы удаляет всё, что в ней.

**Как работает:** Непрозрачность группы применяется к её содержимому целиком (а не к каждому элементу отдельно). Место элементов отсчитывается от угла группы.

**Пример:**
```cpp
#include "painter/gui/area.h"
#include "painter/gui/items.h"

struct Card {
    painter::gui::Group Box;
    painter::gui::Shape Plate;
    painter::gui::Text  Title;
};

Card MakeCard(painter::gui::Area &Screen)
{
    namespace gui = painter::gui;
    Card Made{Screen.Group(), {}, {}};
    Made.Box.SetPlace({40, 40}).SetSize({200, 80}).SetClip(true).SetCorners(painter::core::Corners::All(12));
    Made.Plate = Made.Box.Rectangle(gui::Role::Panel);
    Made.Plate.SetSize({200, 80});
    Made.Title = Made.Box.Text("Карточка", gui::Role::Heading);
    Made.Title.SetPlace({16, 16});
    return Made;
}
```

### `Group::SetClip` / `Group::SetCorners`

**Краткое описание:** Включают обрезку содержимого границами группы и задают скругления этих границ.

**Параметры:**
- `Clip` (`bool`).
- `Round` (`const core::Corners &`).

**Возвращает:** (`Group &`).

**Побочные эффекты:** Свойства.

**Пример:** См. `Group`.

## Создание: `Makes<Self>`

### `Text` / `Rectangle` / `Ellipse` / `Outline` / `Group` / `Picture` / `Canvas` / `Cells`

**Краткое описание:** Создают элемент в области, группе или кадре.

**Параметры:**
- `Text(std::string_view Utf8, Role = Body)`.
- `Rectangle(Role = None)`.
- `Ellipse(Role = None)`.
- `Outline(const core::Path &, bool EvenOdd = false, Role = None)`.
- `Group(Role = None)`.
- `Picture(const PictureBlock &, std::uint8_t Fit = ITEM_FIT_STRETCH, bool Sharp = false)`.
- `Canvas(core::Size Pixels)` — только у области и хранимой группы.
- `Cells(std::uint32_t Cols, std::uint32_t Rows)` — только у области и хранимой группы.

**Возвращает:** Объект элемента; в кадре `Canvas` и `Cells` возвращают пустой объект.

**Побочные эффекты:** Записывают создание элемента (хранимого) или описывают его в кадре.

**Примечания:** Элемент ложится выше всех соседей; порядок создания — порядок обхода Tab.

**Пример:** См. `Group`.
