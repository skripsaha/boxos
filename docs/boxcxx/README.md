# boxcxx — C++ для BoxOS

## Назначение

Исходники: [`src/userspace/boxcxx`](../../src/userspace/boxcxx), архив `libboxcxx.a`.

boxcxx — всё, что нужно программе на C++ в BoxOS:

- **стандартная библиотека C++23** (и часть C++26), написанная для BoxOS — не порт libstdc++ или libc++;
- **рантайм Itanium C++ ABI**: исключения, раскрутка стека, RTTI, `new`/`delete`, статическая инициализация, `thread_local`;
- **`box::`** — C++-интерфейс самой системы: нити, события, сообщения, файлы с тегами, общая память, процессы, настройки.

Поверх boxcxx стоят [Painter](painter/README.md) (графика) и
[Typecase](typecase/README.md) (набор текста); под ним — boxlib, интерфейс C к
ядру.

## С чего начать

1. [Первая программа](first-program.md) — исходник, подключение к сборке, запуск.
2. [Рантайм](runtime.md) — как запускается и завершается программа, исключения, память.
3. [Стандартная библиотека в BoxOS](std/README.md) — что работает иначе, чем в Unix.
4. [box::](native/README.md) — интерфейс системы.
5. [Form](form.md) и [Souvenir](souvenir.md) — входные поля программы и файлы-результаты.
6. Графика: [painter::tui](painter/tui/README.md) для текстовых программ, [painter::gui](painter/gui/README.md) для программ из элементов.

## Как мыслить в BoxOS

- **Кабина** — адресное пространство программы; **нить** — поток выполнения в ней (`std::thread` — это нить). Запуск программы — новая кабина ([process](native/process.md), [child](native/child.md)).
- **Файлы адресуются тегами**, а не путями: `box::tagfs::query("photo,2026")`. `std::ofstream("notes.txt")` открывает файл с этим именем ([tagfs](native/tagfs.md), [ввод-вывод](std/io.md)).
- **Ввод-вывод — каналы Current** (`"screen"`, `"keyboard"`, `"file:имя"`), а не дескрипторы ([current](native/current.md)).
- **События — Touch**: подписка на тег и публикация; сигналов система не шлёт ([touch](native/touch.md)).
- **Ошибки — значения** `box::result<T>` с настоящей причиной из ядра; errno нет ([error](native/error.md)).
- **Аргументы командной строки** не приходят в `argv`: Form раскладывает слова по полям, Luggage хранит всю строку ([form](form.md), [luggage](native/luggage.md)).
- **Ожидание — на событиях**, без опроса и хрупких таймаутов: сопрограммы на `box::executor`, `wait` подписок, `Area::Wait` в графике ([executor](native/executor.md)).

## Пример

Программа ищет файлы с тегом, печатает их имена и размеры и сообщает
другим программам, сколько нашла.

```cpp
#include <print>

#include "box/cxx/tagfs.h"
#include "box/cxx/touch.h"
#include "box/form.h"

BOX_FORM("takes words \"the tags to look for\"\n");

struct Found {
    std::uint32_t Count;
};

int main()
{
    const char *Tags = form_text(0);
    if (!Tags) return 1;
    std::uint32_t Count = 0;
    for (const box::tagfs::file &Each : box::tagfs::query(Tags, "trash")) {
        std::println("{:<32} {:>10}", Each.name(), Each.size());
        Count++;
    }
    using namespace box::literals;
    (void)box::publish("demo:found"_tag, Found{Count});
    return 0;
}
```

## Сборка

Флаги компилятора, порядок линковки и стартовый код — [рантайм](runtime.md).
Полная сборка и запуск системы: `make clean && make && make run`.

## Соответствие стандарту

Подробная опись — [`CONFORMANCE.md`](../../src/userspace/boxcxx/CONFORMANCE.md):
что отсутствует, что отличается и почему, где boxcxx строже стандарта.
Соответствие держит набор `src/userspace/apps/cxxtest.cpp`.
