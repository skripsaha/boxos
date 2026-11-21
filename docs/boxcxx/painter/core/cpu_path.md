# painter::core::cpu_path

## Назначение

Заголовок: [`painter/core/cpu_path.h`](../../../../src/userspace/painter/include/painter/core/cpu_path.h).
Реализация: [`src/core/cpu_path.cpp`](../../../../src/userspace/painter/src/core/cpu_path.cpp),
пути — `lanes_plain.cpp`, `lanes_sse2.cpp`, `lanes_avx2.cpp`.

Строки пикселей движок обрабатывает одним из трёх путей: обычным кодом, с
SSE2 (есть на любом x86-64) или с AVX2 (Haswell и новее). Все три дают
одинаковые биты; самый быстрый из доступных выбирается при первой работе
движка. Программе выбирать ничего не нужно — модуль нужен проверкам,
которые сверяют пути друг с другом, и для диагностики.

## Функции

### `CpuPath`

**Краткое описание:** Называет путь обработки строк.

**Параметры:**
- `Plain`, `Sse2`, `Avx2`.

**Возвращает:** Неприменимо: перечисление.

**Побочные эффекты:** Нет.

**Пример:** См. `CpuPathName`.

### `BestCpuPath` / `ChosenCpuPath`

**Краткое описание:** Возвращают самый быстрый путь процессора и путь, который используется сейчас.

**Параметры:** Нет.

**Возвращает:** (`CpuPath`). AVX2 считается доступным, только если система сохраняет его регистры (XCR0).

**Побочные эффекты:** Нет.

**Пример:** См. `CpuPathName`.

### `UseCpuPath`

**Краткое описание:** Переключает все холсты на заданный путь.

**Параметры:**
- `Path` (`CpuPath`).

**Возвращает:** (`bool`) `false`, если у процессора этого пути нет (ничего не меняется).

**Побочные эффекты:** Меняет путь для всей программы.

**Пример:**
```cpp
#include "painter/core/cpu_path.h"

bool Reference() { return painter::core::UseCpuPath(painter::core::CpuPath::Plain); }
```

### `CpuPathName`

**Краткое описание:** Возвращает название пути.

**Параметры:**
- `Path` (`CpuPath`).

**Возвращает:** (`const char *`) строка для вывода.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include <print>

#include "painter/core/cpu_path.h"

void Tell() { std::println("painter: {}", painter::core::CpuPathName(painter::core::ChosenCpuPath())); }
```
