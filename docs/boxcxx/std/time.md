# Время

## Назначение

`<chrono>` и `<ctime>` в BoxOS: часы, календарь, часовые пояса и
форматирование времени. Установка часового пояса машины — расширение
`box::clock::set_zone` ([clock](../native/clock.md)); замеры и ровная
частота кадров — [timing](../native/timing.md).

## Часы

### `steady_clock` / `system_clock` / `utc_clock` / `tai_clock` / `gps_clock` / `file_clock`

**Краткое описание:** Стандартные часы C++, все с `now()`.

**Параметры:** Нет.

**Возвращает:** Как в стандарте.

**Побочные эффекты:** Чтение часов системы.

**Как работает:** `steady_clock` — монотонные часы от загрузки (для измерения интервалов). `system_clock` — настенное время. `utc_clock`, `tai_clock`, `gps_clock` учитывают 27 високосных секунд; `clock_cast` связывает любые два вида часов. Эпоха `file_clock` — эпоха Unix, поэтому `file_time` печатается как `sys_time`. `to_time_t`/`from_time_t` работают в `std::time_t`.

**Пример:**
```cpp
#include <chrono>
#include <print>

void Stamp()
{
    const auto Now = std::chrono::system_clock::now();
    std::println("{:%Y-%m-%d %H:%M:%S} UTC", std::chrono::floor<std::chrono::seconds>(Now));
}
```

## Часовые пояса

### `locate_zone` / `zoned_time` / `tzdb`

**Краткое описание:** Полная база часовых поясов IANA внутри программы.

**Параметры:** Как в стандарте ([time.zone]).

**Возвращает:** Как в стандарте.

**Побочные эффекты:** Нет системных вызовов и чтения диска.

**Как работает:** База **IANA tzdata 2026c** (598 имён, 341 пояс, 257 ссылок, 16 631 переход, 27 високосных секунд) вшита в образ — файла `/usr/share/zoneinfo` нет. Таблица (116 КБ) лежит в одном объектном файле: программа, которая использует только длительности, её не тянет.

**Примечания:** Обновление базы — через TagFS, а не сеть: файл таблицы с тегом `clock:tzdb` читается `reload_tzdb()`, `remote_version()` сообщает его версию. Загружается таблица, версия которой **отличается** (не обязательно новее): человек может сознательно вернуть прошлую. Таблица из файла — единственный недоверенный ввод библиотеки, она строго проверяется.

**Пример:**
```cpp
#include <chrono>
#include <print>

void InTokyo()
{
    const std::chrono::zoned_time Tokyo{"Asia/Tokyo", std::chrono::system_clock::now()};
    std::println("в Токио {:%H:%M}", Tokyo);
}
```

### `current_zone`

**Краткое описание:** Возвращает часовой пояс машины.

**Параметры:** Нет.

**Возвращает:** (`const time_zone *`) пояс из настройки тега `clock:zone`; `Etc/UTC`, если ничего не задано или имя неизвестно базе.

**Побочные эффекты:** Читает настройку при каждом вызове (не кэширует), поэтому ответ всегда текущий.

**Примечания:** Установить пояс — `box::clock::set_zone`, команда `timezone`; узнать о смене без опроса — Touch `clock:zone` ([clock](../native/clock.md)).

**Пример:**
```cpp
#include <chrono>
#include <print>

void Local()
{
    const std::chrono::zoned_time Here{std::chrono::current_zone(), std::chrono::system_clock::now()};
    std::println("{:%H:%M} ({})", Here, Here.get_time_zone()->name());
}
```

### `chrono::parse` / `from_stream`

**Краткое описание:** Читают время и даты из текста.

**Параметры:** Как в стандарте.

**Возвращает:** Как в стандарте.

**Побочные эффекты:** Нет.

**Примечания:** `%z` и `%Ez` принимают смещение с двоеточием (`+03:00`) — строже стандарта не стали, чтобы читать обычный ISO 8601. Названия месяцев и дней читаются через фасет `time_get` единственной локали `"C"`.

**Пример:**
```cpp
#include <chrono>
#include <sstream>

bool Read(std::chrono::sys_seconds &Out)
{
    std::istringstream In("2026-11-21 12:30:00");
    In >> std::chrono::parse("%F %T", Out);
    return !In.fail();
}
```

## Библиотека C

### `<ctime>`: `time` / `clock` / `localtime` / `gmtime` / `strftime`

**Краткое описание:** Время в стиле C.

**Параметры:** Как в стандарте C.

**Возвращает:** Как в стандарте C.

**Побочные эффекты:** Чтение часов.

**Как работает:** `time_t` — арифметический тип (как требует стандарт; структура BoxOS с этим именем переименована в `BoxTime`). `clock()` — настоящее процессорное время процесса, измеренное TSC по каждому отрезку работы, включая текущий. Результаты `gmtime`, `localtime`, `asctime`, `ctime` хранятся отдельно для каждой нити.

**Примечания:** **`localtime` — это `gmtime`**: C-уровень местного времени не знает. Для местного времени используйте `std::chrono::zoned_time` с `current_zone()`.

**Пример:**
```cpp
#include <cstdio>
#include <ctime>

void Cpu()
{
    const std::clock_t Start = std::clock();
    volatile double X = 0;
    for (int I = 0; I < 1000000; I++) X = X + I;
    std::printf("%.3f с процессора\n", static_cast<double>(std::clock() - Start) / CLOCKS_PER_SEC);
}
```
