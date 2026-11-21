# painter::gui для C

## Назначение

Заголовок: [`painter/gui.h`](../../../../src/userspace/painter/include/painter/gui.h).
Реализация: [`src/gui/c_face.cpp`](../../../../src/userspace/painter/src/gui/c_face.cpp),
измерение — [`src/gui/c_measure.cpp`](../../../../src/userspace/painter/src/gui/c_measure.cpp).

Те же области и элементы для программ на C, с простыми именами. Внутри —
`painter::gui::Area` и объекты элементов; поведение то же, что описано в
[area](area.md), [items](items.md), [changes](changes.md).

Правила:

- элемент — число (`GuiItem`), 0 — нет элемента;
- элемент создаётся под группой или под самой областью (родитель 0) и живёт до `gui_remove` (группа — вместе со всем содержимым) или до закрытия области;
- свойство, заданное повторно так же, никуда не уходит;
- места и размеры — в точках, `GUI_BY_CONTENT` — размер по содержимому;
- цвета — `0xRRGGBBAA`;
- размещения — `DISPLAY_AREA_*`, роли — `ITEM_ROLE_*` из `box/items.h`.

Программа на C, которая не вызывает `gui_measure`, не носит в себе чтение
шрифтов. Линковать нужно с `libpainter.a`, `libtypecase.a` и `libboxcxx.a`
(библиотека написана на C++).

## Типы

### `GuiArea`, `GuiItem`, `GUI_BY_CONTENT`

**Краткое описание:** Называют непрозрачную область, номер элемента и размер по содержимому.

**Параметры:**
- `GuiArea` — непрозрачная структура; указатель возвращает `gui_open`.
- `GuiItem` — `uint32_t`.
- `GUI_BY_CONTENT` — `-1.0f`.

**Возвращает:** Неприменимо: типы.

**Побочные эффекты:** Нет.

**Пример:** См. `gui_open`.

### `GUI_EVENT_*` и `GuiEvent`

**Краткое описание:** Описывают событие, которое вернул `gui_wait`.

**Параметры:**
- `kind` (`int`): `GUI_EVENT_AREA`, `SIZE`, `REFUSED`, `SHOWN`, `PAGE_FREE`, `KEY`, `ALARM`, `GONE`, `END_ASKED`, `ANIMATION_END`, `FOCUS`, `PRESS`, `TEXT` — один к одному с `painter::gui::Event::Kind`.
- `item` (`GuiItem`): элемент события.
- `width`, `height` (`float`): размер области или элемента в точках; `scale` (`float`): пикселей на точку; `seen` (`int`): область видна.
- `character` (`uint32_t`), `scancode`, `mods` (`uint8_t`): клавиша.
- `moment` (`uint64_t`): время будильника, мкс.
- `number` (`uint32_t`), `finished` (`int`): конец анимации.
- `came` (`int`): фокус пришёл (1) или ушёл (0).
- `text` (`const char *`): текст поля в UTF-8 с нулём на конце; действителен до следующего `gui_wait`.

**Возвращает:** Неприменимо: структура.

**Побочные эффекты:** Нет.

**Пример:** См. `gui_wait`.

### `GuiAnimation`

**Краткое описание:** Описывает, как изменения идут во времени.

**Параметры:**
- `milliseconds` (`uint32_t`): длительность.
- `curve` (`int`): `ITEM_CURVE_LINEAR`, `IN`, `OUT`, `IN_OUT`, `STEPS`, `SPRING`, `OWN`; неверное значение — линейная.
- `bounce` (`float`): отскок пружины; `steps` (`int`): число скачков.
- `x1`, `y1`, `x2`, `y2` (`float`): внутренние точки своей кривой Безье.
- `delay` (`uint32_t`): задержка, мс; `number`, `after`, `repeats` (`uint32_t`): номер, «после номера», повторы (`ITEM_FOREVER`); `back` (`int`): туда-обратно.

**Возвращает:** Неприменимо: структура.

**Побочные эффекты:** Нет.

**Пример:** См. `gui_animate`.

## Область

### `gui_open`

**Краткое описание:** Открывает область.

**Параметры:**
- `placement` (`int`): `DISPLAY_AREA_OVER`, `WHOLE`, `FLOW`.
- `width`, `height` (`float`): желаемый размер в точках, 0 — на выбор дисплея.
- `always` (`int`): видна и тогда, когда программа позади.

**Возвращает:** (`GuiArea *`) область или `NULL`.

**Побочные эффекты:** Открывает область у дисплея, выделяет память под `GuiArea`.

**Пример:**
```c
#include "painter/gui.h"

int main(void)
{
    GuiArea *screen = gui_open(DISPLAY_AREA_OVER, 240, 80, 0);
    if (!screen) return 1;
    GuiItem plate = gui_rectangle(screen, 0, ITEM_ROLE_PANEL);
    gui_set_size(screen, plate, 240, 80);
    GuiItem hello = gui_text(screen, 0, "Привет из C", ITEM_ROLE_HEADING);
    gui_set_place(screen, hello, 16, 24);
    gui_present(screen);

    GuiEvent heard;
    while (gui_wait(screen, &heard, 1) == 0) {
        if (heard.kind == GUI_EVENT_GONE || heard.kind == GUI_EVENT_END_ASKED) break;
        if (heard.kind == GUI_EVENT_KEY && heard.character == 27) break;
    }
    gui_close(screen);
    return 0;
}
```

### `gui_close`

**Краткое описание:** Закрывает область со всеми её элементами.

**Параметры:**
- `area` (`GuiArea *`): область; `NULL` допустим.

**Возвращает:** `void`.

**Побочные эффекты:** Освобождает память, закрывает клавиатуру, если `gui_wait` её открыл.

**Пример:** См. `gui_open`.

### `gui_width` / `gui_height` / `gui_scale` / `gui_on_text_page`

**Краткое описание:** Возвращают размер области в точках, масштаб и признак текстовой страницы.

**Параметры:**
- `area` (`const GuiArea *`).

**Возвращает:** `float` (0 для `NULL`; масштаб — 1), `int` для `gui_on_text_page`.

**Побочные эффекты:** Нет.

**Пример:**
```c
#include "painter/gui.h"

float half_width(const GuiArea *screen) { return gui_width(screen) / 2; }
```

### `gui_less_motion` / `gui_motion_speed`

**Краткое описание:** Возвращают, хочет ли человек меньше движения, и скорость анимаций в процентах.

**Параметры:**
- `area` (`const GuiArea *`).

**Возвращает:** `int` (0 или 1), `uint32_t` (100 для `NULL`).

**Побочные эффекты:** Нет.

**Примечания:** Сразу после `gui_open` возвращают значения по умолчанию (0 и 100) до первого события `GUI_EVENT_AREA` — см. примечание к `Area` в [area](area.md).

**Пример:**
```c
#include "painter/gui.h"

int animate_self(const GuiArea *screen) { return !gui_less_motion(screen); }
```

## Элементы

### `gui_text` / `gui_rectangle` / `gui_ellipse` / `gui_group`

**Краткое описание:** Создают текст, прямоугольник, эллипс или группу.

**Параметры:**
- `area` (`GuiArea *`).
- `parent` (`GuiItem`): группа, созданная `gui_group`, или 0 — сама область.
- `utf8` (`const char *`): текст — для `gui_text`; `NULL` — пустой.
- `role` (`int`): `ITEM_ROLE_*`.

**Возвращает:** (`GuiItem`) номер или 0 (нет области, неизвестная группа, нехватка памяти).

**Побочные эффекты:** Записывают создание элемента.

**Пример:**
```c
#include "painter/gui.h"

GuiItem button(GuiArea *screen, GuiItem group, const char *label) { return gui_text(screen, group, label, ITEM_ROLE_BUTTON); }
```

### `gui_remove`

**Краткое описание:** Удаляет элемент (группу — со всем содержимым).

**Параметры:**
- `area` (`GuiArea *`), `item` (`GuiItem`).

**Возвращает:** `void`.

**Побочные эффекты:** Записывает удаление.

**Пример:**
```c
#include "painter/gui.h"

void drop(GuiArea *screen, GuiItem item) { gui_remove(screen, item); }
```

### Свойства: `gui_set_text`, `gui_set_place`, `gui_set_size`, `gui_set_fill`, `gui_set_corners`, `gui_set_opacity`, `gui_set_text_size`, `gui_set_text_weight`, `gui_show`, `gui_hide`

**Краткое описание:** Задают свойства элемента.

**Параметры:**
- `area` (`GuiArea *`), `item` (`GuiItem`): элемент, созданный через этот интерфейс; для других номеров вызов ничего не делает.
- `gui_set_text`: `utf8` (`const char *`).
- `gui_set_place`: `x`, `y` (`float`).
- `gui_set_size`: `width`, `height` (`float`), `GUI_BY_CONTENT` допустим.
- `gui_set_fill`: `rgba` (`uint32_t`, `0xRRGGBBAA`) — одна заливка цветом.
- `gui_set_corners`: `radius` (`float`) — для всех четырёх углов.
- `gui_set_opacity`: `opacity` (`float`, 0–1).
- `gui_set_text_size`: `points` (`float`); `gui_set_text_weight`: `weight` (`int`, 100–900) — каждая сохраняет второе значение стиля.

**Возвращает:** `void`.

**Побочные эффекты:** Записывают свойство.

**Пример:**
```c
#include "painter/gui.h"

void warn(GuiArea *screen, GuiItem line)
{
    gui_set_fill(screen, line, 0xFFB040FFu);
    gui_set_text_weight(screen, line, 600);
}
```

### Клавиатура: `gui_set_focusable`, `gui_set_disabled`, `gui_focus`

**Краткое описание:** Делают элемент доступным клавиатуре или неактивным и переводят фокус.

**Параметры:**
- `area` (`GuiArea *`), `item` (`GuiItem`); для `gui_focus` 0 — снять фокус.
- `reached`, `disabled` (`int`).

**Возвращает:** `void`.

**Побочные эффекты:** Свойства; `gui_focus` — запись в шаг.

**Примечания:** Текст в роли `ITEM_ROLE_BUTTON` — кнопка (нажатие приходит `GUI_EVENT_PRESS`), в роли `ITEM_ROLE_FIELD` — поле, которое редактирует дисплей (`GUI_EVENT_TEXT`).

**Пример:**
```c
#include "painter/gui.h"

void start_on(GuiArea *screen, GuiItem field) { gui_focus(screen, field); }
```

## Шаги, время и события

### `gui_animate`

**Краткое описание:** Задаёт, как идут во времени все последующие изменения.

**Параметры:**
- `area` (`GuiArea *`).
- `how` (`const GuiAnimation *`): анимация; `NULL` — снова сразу.

**Возвращает:** `void`.

**Побочные эффекты:** Меняет режим записи изменений области до следующего вызова.

**Пример:**
```c
#include "painter/gui.h"

void slide(GuiArea *screen, GuiItem item)
{
    GuiAnimation how = {0};
    how.milliseconds = 300;
    how.curve        = ITEM_CURVE_OUT;
    gui_animate(screen, &how);
    gui_set_place(screen, item, 16, 120);
    gui_animate(screen, 0);
    gui_present(screen);
}
```

### `gui_present`

**Краткое описание:** Отправляет накопленные изменения одним шагом.

**Параметры:**
- `area` (`GuiArea *`).

**Возвращает:** (`int`) 1 — ушло всё; 0 — остаток ждёт следующего вызова (или нет области).

**Побочные эффекты:** Как у `Area::Present`.

**Пример:** См. `gui_open`.

### `gui_wait`

**Краткое описание:** Ждёт следующее событие: слово дисплея, клавишу или будильник.

**Параметры:**
- `area` (`GuiArea *`).
- `event` (`GuiEvent *`, out): куда записать событие.
- `keys` (`int`): ненулевое — ждать и клавиши (клавиатура открывается при первом таком вызове и слушается дальше).

**Возвращает:** (`int`) 0 — событие записано; −1 — `area` или `event` равны `NULL`.

**Побочные эффекты:** Блокирует поток без таймаута.

**Пример:** См. `gui_open`.

### `gui_measure`

**Краткое описание:** Измеряет текст так, как его разложит дисплей.

**Параметры:**
- `area` (`const GuiArea *`).
- `utf8` (`const char *`), `role` (`int`).
- `width` (`float`): наибольшая ширина, `GUI_BY_CONTENT` — без ограничения.
- `out_width`, `out_height` (`float *`, out): результат в точках; `NULL` допустим.

**Возвращает:** `void`; при ошибке (нет области, нет памяти) — нули.

**Побочные эффекты:** Как у `Measure` в [measure](measure.md).

**Пример:**
```c
#include "painter/gui.h"

float label_width(const GuiArea *screen, const char *text)
{
    float width = 0, height = 0;
    gui_measure(screen, text, ITEM_ROLE_LABEL, GUI_BY_CONTENT, &width, &height);
    return width;
}
```
