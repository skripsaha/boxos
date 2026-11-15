# painter::tui::Glyphs — коды псевдографики

## Назначение

[Заголовок](../../../../src/userspace/painter/include/painter/tui/glyph.h)
содержит только `inline constexpr char` в пространстве имён
`painter::tui::Glyphs`. Отдельного файла реализации нет. Индексы
соответствуют шрифту BoxOS; это байты глифов, а не UTF-8.

Передавайте константу в `Frame::Put`, `Frame::Fill` или в строку готовых
глифов для `Frame::Write`. Не передавайте такой байт как текст UTF-8 в
`PutText`: управляющий или недопустимый байт будет показан квадратом.

На платформе с знаковым char значения выше 0x7F могут быть отрицательными.
Для числового сравнения преобразуйте их в `unsigned char`. Сам индекс
глифа от этого не меняется.

## Справочник

### `Glyphs::UpperHalf`

**Краткое описание:** Используйте глиф: верхняя половина.

**Параметры:** Нет; `inline constexpr char UpperHalf = static_cast<char>(0xDF)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::UpperHalf;
```

### `Glyphs::LowerHalf`

**Краткое описание:** Используйте глиф: нижняя половина.

**Параметры:** Нет; `inline constexpr char LowerHalf = static_cast<char>(0xDC)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::LowerHalf;
```

### `Glyphs::Full`

**Краткое описание:** Используйте глиф: полный блок.

**Параметры:** Нет; `inline constexpr char Full = static_cast<char>(0xDB)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Full;
```

### `Glyphs::LightShade`

**Краткое описание:** Используйте глиф: редкая штриховка.

**Параметры:** Нет; `inline constexpr char LightShade = static_cast<char>(0xB0)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::LightShade;
```

### `Glyphs::MidShade`

**Краткое описание:** Используйте глиф: средняя штриховка.

**Параметры:** Нет; `inline constexpr char MidShade = static_cast<char>(0xB1)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::MidShade;
```

### `Glyphs::DenseShade`

**Краткое описание:** Используйте глиф: плотная штриховка.

**Параметры:** Нет; `inline constexpr char DenseShade = static_cast<char>(0xB2)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::DenseShade;
```

### `Glyphs::Dot`

**Краткое описание:** Используйте глиф: точка.

**Параметры:** Нет; `inline constexpr char Dot = static_cast<char>(0xFA)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Dot;
```

### `Glyphs::Bullet`

**Краткое описание:** Используйте глиф: маркер.

**Параметры:** Нет; `inline constexpr char Bullet = static_cast<char>(0x07)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Bullet;
```

### `Glyphs::Tick`

**Краткое описание:** Используйте глиф: знак корня, используемый как отметка.

**Параметры:** Нет; `inline constexpr char Tick = static_cast<char>(0xFB)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Tick;
```

### `Glyphs::Left`

**Краткое описание:** Используйте глиф: стрелка влево.

**Параметры:** Нет; `inline constexpr char Left = static_cast<char>(0x1B)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Left;
```

### `Glyphs::Right`

**Краткое описание:** Используйте глиф: стрелка вправо.

**Параметры:** Нет; `inline constexpr char Right = static_cast<char>(0x1A)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Right;
```

### `Glyphs::Up`

**Краткое описание:** Используйте глиф: стрелка вверх.

**Параметры:** Нет; `inline constexpr char Up = static_cast<char>(0x18)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Up;
```

### `Glyphs::Down`

**Краткое описание:** Используйте глиф: стрелка вниз.

**Параметры:** Нет; `inline constexpr char Down = static_cast<char>(0x19)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Down;
```

### `Glyphs::Pointer`

**Краткое описание:** Используйте глиф: треугольник вправо.

**Параметры:** Нет; `inline constexpr char Pointer = static_cast<char>(0x10)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::Pointer;
```

### `Glyphs::LeftHalf`

**Краткое описание:** Используйте глиф: левая половина.

**Параметры:** Нет; `inline constexpr char LeftHalf = static_cast<char>(0xDD)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::LeftHalf;
```

### `Glyphs::RightHalf`

**Краткое описание:** Используйте глиф: правая половина.

**Параметры:** Нет; `inline constexpr char RightHalf = static_cast<char>(0xDE)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::RightHalf;
```

### `Glyphs::RuleAcross`

**Краткое описание:** Используйте глиф: горизонтальная линия.

**Параметры:** Нет; `inline constexpr char RuleAcross = static_cast<char>(0xC4)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::RuleAcross;
```

### `Glyphs::DoubleRule`

**Краткое описание:** Используйте глиф: двойная горизонтальная линия.

**Параметры:** Нет; `inline constexpr char DoubleRule = static_cast<char>(0xCD)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::DoubleRule;
```

### `Glyphs::RuleDown`

**Краткое описание:** Используйте глиф: вертикальная линия.

**Параметры:** Нет; `inline constexpr char RuleDown = static_cast<char>(0xB3)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::RuleDown;
```

### `Glyphs::CornerTL`

**Краткое описание:** Используйте глиф: левый верхний угол.

**Параметры:** Нет; `inline constexpr char CornerTL = static_cast<char>(0xDA)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::CornerTL;
```

### `Glyphs::CornerTR`

**Краткое описание:** Используйте глиф: правый верхний угол.

**Параметры:** Нет; `inline constexpr char CornerTR = static_cast<char>(0xBF)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::CornerTR;
```

### `Glyphs::CornerBL`

**Краткое описание:** Используйте глиф: левый нижний угол.

**Параметры:** Нет; `inline constexpr char CornerBL = static_cast<char>(0xC0)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::CornerBL;
```

### `Glyphs::CornerBR`

**Краткое описание:** Используйте глиф: правый нижний угол.

**Параметры:** Нет; `inline constexpr char CornerBR = static_cast<char>(0xD9)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::CornerBR;
```

### `Glyphs::TeeLeft`

**Краткое описание:** Используйте глиф: соединение с левой вертикальной рамкой, ответвление вправо.

**Параметры:** Нет; `inline constexpr char TeeLeft = static_cast<char>(0xC3)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::TeeLeft;
```

### `Glyphs::TeeRight`

**Краткое описание:** Используйте глиф: соединение с правой вертикальной рамкой, ответвление влево.

**Параметры:** Нет; `inline constexpr char TeeRight = static_cast<char>(0xB4)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::TeeRight;
```

### `Glyphs::MoreLeft`

**Краткое описание:** Используйте глиф: двойная угловая кавычка влево: продолжение строки.

**Параметры:** Нет; `inline constexpr char MoreLeft = static_cast<char>(0xAE)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::MoreLeft;
```

### `Glyphs::MoreRight`

**Краткое описание:** Используйте глиф: двойная угловая кавычка вправо: продолжение строки.

**Параметры:** Нет; `inline constexpr char MoreRight = static_cast<char>(0xAF)`.

**Возвращает:** Неприменимо: константа байтового глифа.

**Побочные эффекты:** Нет.

**Пример:**

```cpp
#include "painter/tui/glyph.h"

namespace tui = painter::tui;

char Glyph = tui::Glyphs::MoreRight;
```