# painter::gui::measure

## Назначение

Заголовок: [`painter/gui/measure.h`](../../../../src/userspace/painter/include/painter/gui/measure.h).
Реализация: [`src/gui/measure.cpp`](../../../../src/userspace/painter/src/gui/measure.cpp),
книга шрифтов программы — `src/gui/fonts.cpp`.

Измерение текста заранее — ровно так, как его разложит дисплей. Нужно,
когда размер зависит от текста: ширина кнопки, колонка таблицы, место
следующей строки.

## Функции

### `Measure`

**Краткое описание:** Возвращает размер, который займёт текст в области.

**Параметры:**
- `On` (`const Area &`): область — от неё берётся вид стекла.
- `Utf8` (`std::string_view`): текст.
- `Of` (`gui::Role`): роль, по умолчанию `Body`.
- `Width` (`float`): наибольшая ширина в точках; `ByContent` (по умолчанию) — без ограничения.
- `Style` (`const TextStyle &`): то, что меняется относительно роли; по умолчанию ничего.

**Возвращает:** (`core::PointF`) ширина и высота в точках; для `Button` и `Field` — вместе с плашкой.

**Побочные эффекты:** При первом вызове читает системные шрифты (все файлы с тегом `font`) в книгу шрифтов программы; блокирует общую книгу на время раскладки.

**Как работает:**
- На framebuffer текст записывается так же, как его сказал бы элемент, читается обратно так же, как читает дисплей (поэтому каждое число округляется так же), и раскладывается тем же кодом, что у дисплея (`painter::items::LayText`), в тех же шрифтах: системные, затем открытые программой (`Area::OpenFont`).
- На текстовой странице: символ — ячейка 8 точек, строка (до `\n`) — ряд 16 точек.

**Примечания:**
- Тема при измерении — по умолчанию (Inter для текста, JetBrains Mono NL для кода): семейства тема пока не меняет, поэтому результат совпадает с тем, что нарисует дисплей.
- Может бросить `std::bad_alloc`.
- Программа, которая ничего не измеряет, не носит в себе чтение шрифтов.

**Типичное применение:** Разложить ряд кнопок или подписей вплотную; выбрать размер панели под текст.

**Пример:**
```cpp
#include <string_view>
#include <vector>

#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/gui/measure.h"

std::vector<painter::gui::Text> Buttons(painter::gui::Area &Screen, std::initializer_list<std::string_view> Names)
{
    namespace gui = painter::gui;
    std::vector<gui::Text> Made;
    float X = 16;
    for (std::string_view Name : Names) {
        Made.push_back(Screen.Text(Name, gui::Role::Button));
        Made.back().SetPlace({X, 16});
        X += gui::Measure(Screen, Name, gui::Role::Button).X + 8;
    }
    return Made;
}
```

### `detail::MeasureOn` (внутренний)

**Краткое описание:** Измеряет текст на заданном виде стекла без области.

**Параметры:**
- `On` (`gui::Glass`), `Utf8`, `Of`, `Width`, `Style`: как у `Measure`.

**Возвращает:** (`core::PointF`).

**Побочные эффекты:** Как у `Measure`.

**Примечания:** Используется `Measure` и стендом проверки Painter.

**Пример:** Не приводится: внутренняя функция.
