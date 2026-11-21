# box::clipboard

## Назначение

Заголовок: [`box/cxx/clipboard.h`](../../../src/userspace/boxcxx/include/box/cxx/clipboard.h).
C-основа: `box/clipboard.h`.

Буфер обмена BoxOS — **список** скопированного, один на машину, его хранит
ядро. Программа кладёт байты и получает номер, которого никогда не будет у
другой вырезки; любая программа видит список (новые первыми) и берёт любую
вырезку по номеру. Список не читает байты и сам ничего не выбрасывает:
вырезка уходит, когда её убрали, когда список очищен или когда кончились её
условия. Что это за байты и какую вырезку взять — решает программа.

Клавиши во всех программах одинаковы: Ctrl+C и Ctrl+X кладут, Ctrl+V
вставляет самую новую вырезку, Ctrl+Shift+V — выбранную из списка.

## Типы

### `box::clip_terms`

**Краткое описание:** Условия жизни вырезки.

**Параметры:**
- `takes` (`std::uint32_t`): сколько целых взятий она выдержит; 0 — такого условия нет (тогда брать можно только целиком без ограничения).
- `while_alive` (`bool`): исчезнет, когда закончится положившая её программа.

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `clipboard::put`.

### `box::clip`

**Краткое описание:** Описание вырезки из списка.

**Параметры:**
- `number` (`std::uint64_t`), `bytes` (`std::uint64_t`).
- `put_us` (`std::uint64_t`): когда положена, микросекунды с загрузки.
- `takes_left` (`std::uint32_t`): сколько взятий осталось, 0 — условия нет.
- `while_alive` (`bool`).

**Возвращает:** Неприменимо: агрегат.

**Побочные эффекты:** Нет.

**Пример:** См. `clipboard::list`.

## Функции

### `clipboard::put`

**Краткое описание:** Кладёт байты новой вырезкой в начало списка.

**Параметры:**
- `bytes` (`std::string_view`).
- `terms` (`clip_terms`): по умолчанию без условий.

**Возвращает:** (`box::result<std::uint64_t>`) номер вырезки.

**Побочные эффекты:** Меняет список; публикует `clipboard:changed`.

**Пример:**
```cpp
#include "box/cxx/clipboard.h"

void CopyPassword(std::string_view Secret) { (void)box::clipboard::put(Secret, {1, true}); }   // одно взятие, пока программа жива
```

### `clipboard::list`

**Краткое описание:** Возвращает вырезки, новые первыми.

**Параметры:**
- `most` (`std::size_t`): сколько; 0 — все.
- `from` (`std::uint64_t`): начиная с номера `from` и старше; 0 — с самой новой.

**Возвращает:** (`box::result<std::vector<clip>>`).

**Побочные эффекты:** Запросы к ядру страницами по 256.

**Пример:**
```cpp
#include "box/cxx/clipboard.h"

std::string Paste()
{
    auto Newest = box::clipboard::list(1);
    if (!Newest || Newest->empty()) return {};
    return box::clipboard::take(Newest->front()).value_or("");
}
```

### `clipboard::take`

**Краткое описание:** Берёт байты вырезки целиком или кусок.

**Параметры:**
- `take(const clip &which)` — целиком.
- `take(std::uint64_t number, std::uint64_t offset, std::span<char> into)` — кусок с `offset`, сколько поместится.

**Возвращает:** (`box::result<std::string>`) / (`box::result<std::size_t>`) сколько пришло.

**Побочные эффекты:** Целое взятие вырезки с условием `takes` тратит одно взятие; на последнем вырезка уходит.

**Пример:** См. `clipboard::list`.

### `clipboard::drop` / `clipboard::clear`

**Краткое описание:** Убирают одну вырезку или все.

**Параметры:** `number` (`std::uint64_t`) для `drop`.

**Возвращает:** `drop` — (`box::status`); `clear` — (`box::result<std::uint64_t>`) сколько ушло.

**Побочные эффекты:** Меняют список; публикуют `clipboard:changed`.

**Пример:**
```cpp
#include "box/cxx/clipboard.h"

void Forget() { (void)box::clipboard::clear(); }
```

### `clipboard::on_change`

**Краткое описание:** Подписывается на изменения списка.

**Параметры:** Нет.

**Возвращает:** (`box::subscription`) на тег `clipboard:changed`; нагрузка события — `ClipboardChange` (`payload_as<ClipboardChange>()`).

**Побочные эффекты:** Заявка Touch ([touch](touch.md)).

**Пример:**
```cpp
#include "box/cxx/clipboard.h"

void Follow()
{
    box::subscription Changes = box::clipboard::on_change();
    while (auto Event = Changes.wait(10000))
        if (auto What = Event->payload_as<ClipboardChange>()) { /* обновить показ списка */ }
}
```
