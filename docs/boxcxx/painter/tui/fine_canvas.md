# painter::tui::fine_canvas — фигуры средствами шрифта

## Назначение

[Заголовок и шаблон Cover](../../../../src/userspace/painter/include/painter/tui/fine_canvas.h),
[реализация фигур](../../../../src/userspace/painter/src/tui/fine_canvas.cpp).

`FineCanvas` рисует фигуры на [Frame](frame.md), оценивая цвета подъячеек
и сводя их в глиф через [ResolveMosaic](mosaic.md). Результат остаётся
текстовой псевдографикой. Собственного буфера подъячеек между вызовами нет.

Геометрия использует единицы `Frame::PutPixel`: одна ячейка имеет ширину 1 и
высоту 2. Сетка подъячеек выбирается по стеклу кадра (`Frame::Mosaics()`): четыре строки
при `DISPLAY_MOSAIC_OCTANTS`, иначе три при `DISPLAY_MOSAIC_SEXTANTS`, иначе
две. Подъячейка имеет ширину 0.5 и высоту 2 / строк (на двух строках — 1),
середина — на `Col + 0.25/0.75` и `Row * 2 + (r + 0.5) * 2 / строк`. Аргументы `Cell` у PutMosaic
и `Cells` у Cover, а также результат Reaching измеряются в целых ячейках.

Фигуры рисуются последовательно поверх уже приближённых ячеек. Обычные
текстовые глифы при разборе MosaicOf представлены только фоном, поэтому
сначала рисуйте фигуры, затем текст. Затронутая ячейка получает флаг Opaque
и теряет CellMark; незатронутая не меняется. Каретка кадра сохраняется.

Используйте конечные координаты и размеры в пределах целочисленных
преобразований Reaching. Для обычных фигур задавайте неотрицательные размеры
и толщину. Opacity также должен быть конечным; обычный диапазон — 0–1.
Cover ограничивает произведение покрытия и Opacity этим диапазоном.
Эти условия не проверяются статусами. В режиме Text доля покрытия
округляется до целого; в Pixels используется смешивание. При работе с
самостоятельным кадром сначала задайте `Frame::SetGlass` и
`Frame::SetMosaics` по поверхности назначения. Внутренних блокировок нет.

## Справочник

### `FineCanvas`

**Краткое описание:** Рисуйте фигуры через подъячеечные цвета на существующем кадре.

**Параметры:** `Target` (`Frame &`, in-out); `explicit FineCanvas(Frame &Target) noexcept`.

**Возвращает:** Неприменимо: объект с невладеющей ссылкой на кадр.

**Побочные эффекты:** Конструктор только запоминает адрес кадра.

**Примечания:** Кадр должен жить дольше canvas и не перемещаться во время его использования. Копирование canvas копирует указатель на тот же кадр; собственной истории рисунка нет.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::Frame &Out) { tui::FineCanvas Fine(Out); }
```

### `FineCanvas::PutMosaic`

**Краткое описание:** Замените ячейку ближайшим представлением заданного Mosaic.

**Параметры:** `Cell` (`core::Point`, in): клеточные координаты; `Wanted` (`const Mosaic &`, in); `bool PutMosaic(core::Point Cell, const Mosaic &Wanted)`.

**Возвращает:** `bool`: успех `Frame::Put`; вне кадра — `false`.

**Побочные эффекты:** Заменяет ячейку, если она внутри кадра.

**Как работает:** Вызывает `ResolveMosaic(Wanted, Target->Glass(), Target->Mosaics())`, затем `Target->Put`. Не смешивает Wanted с прежней ячейкой.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

bool Example(tui::FineCanvas &Fine, const tui::Mosaic &Wanted) { return Fine.PutMosaic({1, 1}, Wanted); }
```

### `FineCanvas::Reaching`

**Краткое описание:** Вычислите клеточный прямоугольник работы с запасом для мягкого края.

**Параметры:** `Left`, `Top`, `Right`, `Bottom` (`float`, in): границы в единицах полублоков; `static core::Rect Reaching(float Left, float Top, float Right, float Bottom) noexcept`.

**Возвращает:** `core::Rect`: прямоугольник ячеек, ещё не отсечённый кадром.

**Побочные эффекты:** Нет.

**Как работает:** Начало — `floor(Left) - 1`, `floor(Top / 2) - 1`. Правая и нижняя исключающие границы — `floor(Right) + 2` и `floor(Bottom / 2) + 2`.

**Примечания:** Здесь X — ширина ячейки, Y — высота половины ячейки. Числа должны быть конечными и давать представимые в `std::int32_t` границы и размеры.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

core::Rect Example() { return tui::FineCanvas::Reaching(2.0f, 4.0f, 8.0f, 12.0f); }
```

### `FineCanvas::Cover`

**Краткое описание:** Наложите цвет на фигуру, заданную функцией знакового расстояния.

**Параметры:** `Cells` (`core::Rect`, in): клеточная область работы; `Ink` (`core::Color`, in); `Outside` (`Distance`, in): вызываемый объект `Outside(float X, float Y)`; `Opacity` (`float`, in) = `1.0f`. Сигнатура: `template<class Distance> void Cover(core::Rect Cells, core::Color Ink, Distance Outside, float Opacity = 1.0f)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет затронутые ячейки кадра результатом `ResolveMosaic`; не публикует слой.

**Как работает:** Отсекает Cells размером кадра и разбирает текущую ячейку через `MosaicOf` по самой тонкой сетке, которую рисует стекло кадра. В центрах подъячеек этой сетки оценивает расстояние; у края конечными разностями с шагом `0.05f` оценивает направление и ширину пересечения. Долю покрытия умножает на Opacity, ограничивает диапазоном 0–1 и смешивает цвет через `Color::Mix`. Для Text предварительно округляет покрытие до 0 или 1; затронутую ячейку сразу сводит через `ResolveMosaic`.

**Примечания:** Outside должен давать отрицательное значение внутри фигуры и корректное расстояние вблизи края, не только логический признак. Cells должен охватывать всю фигуру с мягким краем; вне него работа не выполняется. Исключения Outside не перехватываются; частичный рисунок остаётся.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::FineCanvas &Fine) {
    Fine.Cover({0, 0, 8, 4}, core::Colors::Cyan,
               [](float X, float) { return X - 3.0f; });
}
```

### `FineCanvas::Fill`

**Краткое описание:** Заполните прямоугольник с обработкой края.

**Параметры:** Все координаты и размеры имеют тип `float`, цвет `Ink` — `core::Color`, `Opacity` (`float`) по умолчанию `1.0f`; все параметры in. `Left`, `Top` — начало; `Across`, `Down` — ширина и высота. `void Fill(float Left, float Top, float Across, float Down, core::Color Ink, float Opacity = 1.0f)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет затронутые ячейки кадра результатом `ResolveMosaic`; не публикует слой.

**Как работает:** Вызывает `Rounded` с радиусом `0.0f`. Вся обработка покрытия выполняется через Cover.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::FineCanvas &Fine) { Fine.Fill(2, 4, 8, 6, core::Colors::Blue); }
```

### `FineCanvas::Disc`

**Краткое описание:** Нарисуйте заполненный круг.

**Параметры:** Все координаты и размеры имеют тип `float`, цвет `Ink` — `core::Color`, `Opacity` (`float`) по умолчанию `1.0f`; все параметры in. `Across`, `Down` — центр; `Radius` — радиус. `void Disc(float Across, float Down, float Radius, core::Color Ink, float Opacity = 1.0f)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет затронутые ячейки кадра результатом `ResolveMosaic`; не публикует слой.

**Как работает:** Использует расстояние от центра минус Radius и ограничивает работу прямоугольником Reaching.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::FineCanvas &Fine) { Fine.Disc(10, 10, 4, core::Colors::Red); }
```

### `FineCanvas::Ellipse`

**Краткое описание:** Нарисуйте заполненный эллипс.

**Параметры:** Все координаты и размеры имеют тип `float`, цвет `Ink` — `core::Color`, `Opacity` (`float`) по умолчанию `1.0f`; все параметры in. `Across`, `Down` — центр; `ReachAcross`, `ReachDown` — полуоси. `void Ellipse(float Across, float Down, float ReachAcross, float ReachDown, core::Color Ink, float Opacity = 1.0f)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет затронутые ячейки кадра результатом `ResolveMosaic`; не публикует слой.

**Как работает:** Ограничивает полуоси снизу значениями `0.25f` и `0.5f`. Расстояние у края приближает через нормированные координаты и наклон; в центре возвращает отрицательную меньшую полуось.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::FineCanvas &Fine) { Fine.Ellipse(10, 10, 5, 3, core::Colors::Green); }
```

### `FineCanvas::Ring`

**Краткое описание:** Нарисуйте кольцо между двумя радиусами.

**Параметры:** Все координаты и размеры имеют тип `float`, цвет `Ink` — `core::Color`, `Opacity` (`float`) по умолчанию `1.0f`; все параметры in. `Across`, `Down` — центр; `Outer`, `Inner` — внешний и внутренний радиусы. `void Ring(float Across, float Down, float Outer, float Inner, core::Color Ink, float Opacity = 1.0f)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет затронутые ячейки кадра результатом `ResolveMosaic`; не публикует слой.

**Как работает:** Функция расстояния — максимум `distance - Outer` и `Inner - distance`; область работы строится по Outer.

**Примечания:** Используйте `0 <= Inner <= Outer`; автоматической перестановки радиусов нет.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::FineCanvas &Fine) { Fine.Ring(10, 10, 5, 3, core::Colors::Yellow); }
```

### `FineCanvas::Rounded`

**Краткое описание:** Нарисуйте прямоугольник со скруглёнными углами.

**Параметры:** Все координаты и размеры имеют тип `float`, цвет `Ink` — `core::Color`, `Opacity` (`float`) по умолчанию `1.0f`; все параметры in. `Left`, `Top` — начало; `Across`, `Down` — размеры; `Radius` — радиус углов. `void Rounded(float Left, float Top, float Across, float Down, float Radius, core::Color Ink, float Opacity = 1.0f)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет затронутые ячейки кадра результатом `ResolveMosaic`; не публикует слой.

**Как работает:** Строит расстояние до прямоугольника от его центра с учётом Radius, затем передаёт его Cover. Функция не ограничивает радиус автоматически.

**Примечания:** Для обычного скруглённого прямоугольника задавайте неотрицательный радиус не больше половины меньшего размера.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::FineCanvas &Fine) { Fine.Rounded(2, 4, 12, 8, 2, core::Colors::Blue); }
```

### `FineCanvas::Line`

**Краткое описание:** Нарисуйте толстый отрезок с округлыми концами.

**Параметры:** `From`, `To` (`core::PointF`, in): концы; `Thick` (`float`, in): полная толщина; `Ink` (`core::Color`, in); `Opacity` (`float`, in) = `1.0f`; `void Line(core::PointF From, core::PointF To, float Thick, core::Color Ink, float Opacity = 1.0f)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет затронутые ячейки кадра результатом `ResolveMosaic`; не публикует слой.

**Как работает:** Вычисляет расстояние до ближайшей точки отрезка и вычитает половину толщины. При совпадении концов расстояние считается до одной точки.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::FineCanvas &Fine) { Fine.Line({2, 4}, {12, 8}, 2, core::Colors::White); }
```

### `FineCanvas::Polygon`

**Краткое описание:** Заполните многоугольник по последовательности вершин.

**Параметры:** `Corners` (`std::span<const core::PointF>`, in); `Ink` (`core::Color`, in); `Opacity` (`float`, in) = `1.0f`; `void Polygon(std::span<const core::PointF> Corners, core::Color Ink, float Opacity = 1.0f)`.

**Возвращает:** `void`.

**Побочные эффекты:** Заменяет затронутые ячейки кадра результатом `ResolveMosaic`; не публикует слой.

**Как работает:** Менее трёх вершин ничего не рисуют. Для остальных находит ограничивающий прямоугольник, расстояние до ближайшего ребра и знак по чётности пересечений горизонтального луча. Последняя вершина соединяется с первой.

**Примечания:** Рёбра не обязаны задавать выпуклый контур; для самопересечения используется правило чётности.

**Пример:**

```cpp
#include "painter/tui/fine_canvas.h"

namespace tui = painter::tui;
namespace core = painter::core;

void Example(tui::FineCanvas &Fine) {
    const core::PointF Corners[] = {{2, 4}, {8, 4}, {5, 10}};
    Fine.Polygon(Corners, core::Colors::Magenta);
}
```
