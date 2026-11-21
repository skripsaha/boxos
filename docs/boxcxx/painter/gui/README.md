# painter::gui

## Назначение

`painter::gui` — программы из элементов. Программа получает от дисплея
**область** (`Area`) и создаёт в ней **элементы**: текст, прямоугольник,
эллипс, произвольный контур, группу, картинку, холст своих пикселей, сетку
ячеек. Каждый элемент говорит только то, что отличается от его **роли**
(`Role`): место, размер, заливки, обводку, тени, текст и стиль. Всё
несказанное берётся из темы человека: шрифты, цвета, скругления, тени.

Программа не рисует пиксели сама и не перерисовывает экран. Она сообщает,
что изменилось («у элемента 3 текст теперь 12:47»), а дисплей хранит
элементы, рисует заново только изменившееся, проигрывает анимации по
часам, пока программа спит, ведёт фокус клавиатуры, рисует кнопки и поля и
сам редактирует текст в полях.

## Модули

- [area](area.md) — `Area`, `Placement`, `Glass`, `Event`: область, показ шагов, ожидание событий, блоки памяти.
- [items](items.md) — элементы как объекты: `Text`, `Shape`, `Group`, `Picture`, `Canvas`, `Cells` и их методы `Set…`.
- [changes](changes.md) — значения свойств (`Fill`, `Stroke`, `Shadow`, `TextStyle`, `TextBox`, `Animation`, роли, цвета темы) и `Changes` — запись изменений на языке элементов.
- [frame](frame.md) — кадр, описанный целиком (`Area::Draw`), и как он сравнивается с прошлым.
- [blocks](blocks.md) — блоки памяти: пиксели картинки и две страницы холста.
- [measure](measure.md) — измерение текста заранее, ровно как его разложит дисплей.
- [c_face](c_face.md) — тот же интерфейс для программ на C (`painter/gui.h`).

Рисование пикселей на холсте описано в [painter::core](../core/README.md),
внешний вид ролей — в [painter::items](../items/README.md).

## Главные понятия

**Область.** Её место и размер выбирает дисплей. Программа только говорит,
чего хочет (`Placement`: поверх всего, весь экран, в потоке консоли; и
желаемый размер), и узнаёт, что получила. Области в потоке консоли лежат на
её тексте и прокручиваются вместе с ним.

**Точки.** Места и размеры задаются в точках (`float`), с точностью до 1/256.
Дисплей кладёт точку на стекло в своём масштабе (`Area::Scale()` пикселей
на точку) — одна точка на пиксель на экране высотой 1080, две на 2160, как
настроит человек. Поэтому программа выглядит одинаково на любом экране.
Место элемента отсчитывается от угла его группы (или области).

**Шаги.** Изменения копятся в программе и уходят дисплею одним шагом при
`Area::Present()`. Дисплей показывает шаг целиком или пока никак, никогда
наполовину. Если кольцо обмена заполнено, программа не ждёт: изменения
остаются у неё, у каждого свойства побеждает последнее слово, и они уходят
со следующим `Present`.

**Только разница.** Painter помнит последнее сказанное значение каждого
свойства. Свойство, сказанное ещё раз так же, никуда не уходит — программа
может каждый раз говорить про элемент всё целиком.

**Два способа описывать.** Элементы как объекты ([items](items.md)):
создал, меняешь `Set…`, объект уничтожен — элемент исчез. Или кадр целиком
([frame](frame.md)): каждый раз описываешь всё, Painter сам находит разницу
с прошлым кадром.

**Роли и тема.** Роль говорит, что это за элемент: `Page`, `Panel`, `Body`,
`Heading`, `Code`, `Hint`, `Label`, `Button`, `Field`, `Icon`, `Separator`,
`Selection`. Тема человека (`settings display theme`) решает, как роль
выглядит: акцентный цвет, скругления, тени. То, что программа сказала сама,
стоит выше темы — если только человек не настоял (`theme insist`): тогда
всё с ролью выглядит, как велит тема.

**Текстовая страница.** На стекле `Glass::Text` текст пишется в ячейки по
символу (ячейка — 8×16 точек), строка — ряд; остальные элементы видны своей
подписью (`SetCaption`). Все изменения там применяются сразу, без анимаций.

**Время.** Всё, сказанное внутри `Area::Animate`, дисплей проводит во
времени по заданной кривой (линейно, с ускорением, пружиной, ступенями,
своей кривой Безье). Числа и цвета идут через промежуточные значения
(цвета — через OKLab), остальное (текст, картинка, роль) плавно сменяет
вид. Человек решает, сколько движения ему нужно: при «меньше движения»
перемещения происходят сразу, а все анимации идут с его скоростью
(`settings display motion`).

**Клавиатура.** Кнопки и поля дисплей обслуживает сам: Tab и Shift+Tab
переводят фокус в порядке создания, стрелки — к ближайшему в их сторону,
Enter или Space нажимают, в поле дисплей сам двигает каретку и правит
текст, Escape выходит из поля. По аккорду (`settings display keys point`,
по умолчанию `ctrl+.`) на всём доступном появляются короткие метки. Программа
получает смысл: `Event::Kind::Press`, `Text`, `Focus`. Клавиши, которые
дисплей не использовал, приходят программе как обычно.

## Первая программа

Панель со счётчиком и кнопкой. Нажатие кнопки (Enter или Space на ней)
увеличивает число, и дисплей проводит смену цифр пружиной. Escape выходит.

```cpp
#include <chrono>
#include <format>

#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/tui/keyboard.h"

int main()
{
    namespace gui = painter::gui;
    using namespace std::chrono_literals;

    auto Opened = gui::Area::Open(gui::Placement::Over, {320, 160});
    if (!Opened) return 1;
    gui::Area &Screen = *Opened;   // ссылка, а не копия: Area лучше не перемещать
    auto Keys = painter::tui::Keyboard::Open();
    if (!Keys) return 2;

    gui::Shape Plate = Screen.Rectangle(gui::Role::Panel);
    Plate.SetSize({320, 160});
    gui::Text Count = Screen.Text("0", gui::Role::Heading);
    Count.SetPlace({24, 20});
    gui::Text More = Screen.Text("Ещё", gui::Role::Button);
    More.SetPlace({24, 96}).Focus();
    Screen.Present();

    int Pressed = 0;
    for (;;) {
        const gui::Event Heard = Screen.Wait(&*Keys);
        if (Heard.What == gui::Event::Kind::Gone || Heard.What == gui::Event::Kind::EndAsked) break;
        if (Heard.What == gui::Event::Kind::Key && Heard.Key.Character == painter::tui::KeyCodes::Escape) break;
        if (Heard.What == gui::Event::Kind::Press && Heard.Item == More.Number()) {
            Screen.Animate(gui::Animation::Spring(400ms, 0.3f), [&] { Count.SetText(std::format("{}", ++Pressed)); });
            Screen.Present();
        }
    }
    return 0;   // объекты элементов уничтожаются раньше области
}
```

## Кадр целиком

Тот же список каждый раз описывается заново; Painter отправляет только
разницу. `Key` сохраняет за строкой её элемент, даже если строки поменялись
местами.

```cpp
#include <string>
#include <vector>

#include "painter/gui/area.h"
#include "painter/gui/frame.h"

struct Row {
    std::uint64_t Id;
    std::string   Name;
};

void Show(painter::gui::Area &Screen, const std::vector<Row> &Rows)
{
    namespace gui = painter::gui;
    Screen.Draw([&](gui::Frame &F) {
        F.Rectangle(gui::Role::Panel).SetSize({300, 40.0f + 28.0f * static_cast<float>(Rows.size())});
        float Y = 20;
        for (const Row &Each : Rows) {
            F.Text(Each.Name).Key(Each.Id).SetPlace({20, Y});
            Y += 28;
        }
    });
}
```

## Свои пиксели

Холст — настоящие пиксели экрана. `Draw` даёт страницу, которую дисплей
сейчас не показывает, и ждёт, если обе заняты.

```cpp
#include "painter/core/canvas.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"

bool Sunset(painter::gui::Area &Screen, painter::gui::Canvas &Picture)
{
    namespace core = painter::core;
    const bool Drawn = Picture.Draw([&](core::Canvas &Page) {
        const core::RectF All{0, 0, static_cast<float>(Page.Width()), static_cast<float>(Page.Height())};
        const core::Gradient Sky = core::Gradient::Linear({0, 0}, {0, All.Height},
                                                          {{0.0f, core::Colors::DarkBlue}, {1.0f, core::Colors::Coral}});
        (void)Page.Fill(core::Path::Rect(All), Sky);
        (void)Page.Fill(core::Path::Circle({All.Width / 2, All.Height * 0.8f}, All.Height / 5), core::Colors::Amber);
    });
    Screen.Present();
    return Drawn;
}
```

Холст создаётся так: `gui::Canvas Picture = Screen.Canvas({400, 300});` —
размер в пикселях, а не в точках.

## Сборка

Программа на `painter::gui` линкуется со стартовым кодом, объектами
программы, `libpainter.a`, `libtypecase.a`, `libboxcxx.a`, `libbox.a` и
`-lgcc` — Typecase нужен для измерения текста и для `Canvas::FillText`.
Пример — правило `spell.elf` в
[Makefile утилит](../../../../src/userspace/utils/Makefile):

```make
spell.elf: $(BOXLIB_START) $(SPELL_OBJS) $(PAINTER) $(TYPECASE) $(BOXCXX) $(BOXLIB) $(BOXLIB_DIR)/user.ld $(NAMEPLATE_TOOL)
	$(call NameplateLink,$(CXX),$(LDFLAGS),$(BOXLIB_START) $(SPELL_OBJS) $(PAINTER) $(TYPECASE) $(BOXCXX) $(BOXLIB) -lgcc)
```

Живые примеры в дереве: `utils/spell.cpp` (анимации букв), `utils/bloom.cpp`,
`utils/ripple.cpp`, `utils/mandelbrot.cpp` (холст), `utils/itemcheck`
(проверка всего языка, включая интерфейс C).

## Потоки и время жизни

Областью говорит один поток — тот, что её открыл. Объекты элементов
ссылаются на свою область; объект, переживший область, ничего не делает.
Область нельзя копировать и перемещающе присваивать; перемещающий
конструктор есть, но см. примечание к `Area::Open` в [area](area.md).
