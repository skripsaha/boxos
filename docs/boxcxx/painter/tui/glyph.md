# painter::tui::Glyphs — символы псевдографики

## Назначение

[Заголовок](../../../../src/userspace/painter/include/painter/tui/glyph.h)
содержит только `inline constexpr char32_t` в пространстве имён
`painter::tui::Glyphs`. Отдельного файла реализации нет. Каждая константа —
символ Unicode, а не номер глифа: какой глиф его рисует, решает дисплей.

Кладите константу в ячейку через `Frame::Put` или `Frame::Fill`. Внутри
текста UTF-8 для `Frame::Write`, `Frame::Centre` или `PutText` пишите сам
символ в UTF-8: `"←"`, а не `Glyphs::Left`.

## Справочник

### `Glyphs::UpperHalf`

**Краткое описание:** Используйте символ: верхняя половина.

**Параметры:** Нет; `inline constexpr char32_t UpperHalf = U'▀'` (U+2580).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::UpperHalf;
```

### `Glyphs::LowerHalf`

**Краткое описание:** Используйте символ: нижняя половина.

**Параметры:** Нет; `inline constexpr char32_t LowerHalf = U'▄'` (U+2584).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::LowerHalf;
```

### `Glyphs::Full`

**Краткое описание:** Используйте символ: полный блок.

**Параметры:** Нет; `inline constexpr char32_t Full = U'█'` (U+2588).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Full;
```

### `Glyphs::LightShade`

**Краткое описание:** Используйте символ: редкая штриховка.

**Параметры:** Нет; `inline constexpr char32_t LightShade = U'░'` (U+2591).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::LightShade;
```

### `Glyphs::MidShade`

**Краткое описание:** Используйте символ: средняя штриховка.

**Параметры:** Нет; `inline constexpr char32_t MidShade = U'▒'` (U+2592).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::MidShade;
```

### `Glyphs::DenseShade`

**Краткое описание:** Используйте символ: плотная штриховка.

**Параметры:** Нет; `inline constexpr char32_t DenseShade = U'▓'` (U+2593).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::DenseShade;
```

### `Glyphs::Dot`

**Краткое описание:** Используйте символ: точка.

**Параметры:** Нет; `inline constexpr char32_t Dot = U'·'` (U+00B7).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Dot;
```

### `Glyphs::Bullet`

**Краткое описание:** Используйте символ: маркер.

**Параметры:** Нет; `inline constexpr char32_t Bullet = U'•'` (U+2022).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Bullet;
```

### `Glyphs::Tick`

**Краткое описание:** Используйте символ: знак корня, используемый как отметка.

**Параметры:** Нет; `inline constexpr char32_t Tick = U'√'` (U+221A).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Tick;
```

### `Glyphs::Left`

**Краткое описание:** Используйте символ: стрелка влево.

**Параметры:** Нет; `inline constexpr char32_t Left = U'←'` (U+2190).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Left;
```

### `Glyphs::Right`

**Краткое описание:** Используйте символ: стрелка вправо.

**Параметры:** Нет; `inline constexpr char32_t Right = U'→'` (U+2192).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Right;
```

### `Glyphs::Up`

**Краткое описание:** Используйте символ: стрелка вверх.

**Параметры:** Нет; `inline constexpr char32_t Up = U'↑'` (U+2191).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Up;
```

### `Glyphs::Down`

**Краткое описание:** Используйте символ: стрелка вниз.

**Параметры:** Нет; `inline constexpr char32_t Down = U'↓'` (U+2193).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Down;
```

### `Glyphs::Pointer`

**Краткое описание:** Используйте символ: треугольник вправо.

**Параметры:** Нет; `inline constexpr char32_t Pointer = U'►'` (U+25BA).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::Pointer;
```

### `Glyphs::LeftHalf`

**Краткое описание:** Используйте символ: левая половина.

**Параметры:** Нет; `inline constexpr char32_t LeftHalf = U'▌'` (U+258C).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::LeftHalf;
```

### `Glyphs::RightHalf`

**Краткое описание:** Используйте символ: правая половина.

**Параметры:** Нет; `inline constexpr char32_t RightHalf = U'▐'` (U+2590).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::RightHalf;
```

### `Glyphs::RuleAcross`

**Краткое описание:** Используйте символ: горизонтальная линия.

**Параметры:** Нет; `inline constexpr char32_t RuleAcross = U'─'` (U+2500).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::RuleAcross;
```

### `Glyphs::DoubleRule`

**Краткое описание:** Используйте символ: двойная горизонтальная линия.

**Параметры:** Нет; `inline constexpr char32_t DoubleRule = U'═'` (U+2550).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::DoubleRule;
```

### `Glyphs::RuleDown`

**Краткое описание:** Используйте символ: вертикальная линия.

**Параметры:** Нет; `inline constexpr char32_t RuleDown = U'│'` (U+2502).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::RuleDown;
```

### `Glyphs::CornerTL`

**Краткое описание:** Используйте символ: левый верхний угол.

**Параметры:** Нет; `inline constexpr char32_t CornerTL = U'┌'` (U+250C).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::CornerTL;
```

### `Glyphs::CornerTR`

**Краткое описание:** Используйте символ: правый верхний угол.

**Параметры:** Нет; `inline constexpr char32_t CornerTR = U'┐'` (U+2510).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::CornerTR;
```

### `Glyphs::CornerBL`

**Краткое описание:** Используйте символ: левый нижний угол.

**Параметры:** Нет; `inline constexpr char32_t CornerBL = U'└'` (U+2514).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::CornerBL;
```

### `Glyphs::CornerBR`

**Краткое описание:** Используйте символ: правый нижний угол.

**Параметры:** Нет; `inline constexpr char32_t CornerBR = U'┘'` (U+2518).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::CornerBR;
```

### `Glyphs::TeeLeft`

**Краткое описание:** Используйте символ: соединение с левой вертикальной рамкой, ответвление вправо.

**Параметры:** Нет; `inline constexpr char32_t TeeLeft = U'├'` (U+251C).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::TeeLeft;
```

### `Glyphs::TeeRight`

**Краткое описание:** Используйте символ: соединение с правой вертикальной рамкой, ответвление влево.

**Параметры:** Нет; `inline constexpr char32_t TeeRight = U'┤'` (U+2524).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::TeeRight;
```

### `Glyphs::MoreLeft`

**Краткое описание:** Используйте символ: двойная угловая кавычка влево: продолжение строки.

**Параметры:** Нет; `inline constexpr char32_t MoreLeft = U'«'` (U+00AB).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::MoreLeft;
```

### `Glyphs::MoreRight`

**Краткое описание:** Используйте символ: двойная угловая кавычка вправо: продолжение строки.

**Параметры:** Нет; `inline constexpr char32_t MoreRight = U'»'` (U+00BB).

**Возвращает:** Неприменимо: константа символа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char32_t Glyph = tui::Glyphs::MoreRight;
```