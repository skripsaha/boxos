# painter::core::pixel

## Назначение

Заголовок: [`painter/core/pixel.h`](../../../../src/userspace/painter/include/painter/core/pixel.h).
Реализация: [`src/core/pixel.cpp`](../../../../src/userspace/painter/src/core/pixel.cpp).

Пиксель холста так, как его хранит движок.

## Pixel

### `Pixel`

**Краткое описание:** Хранит пиксель холста: красный, зелёный и синий свет, умноженные на непрозрачность, и саму непрозрачность — четыре числа 0–65535.

**Параметры:**
- `Red`, `Green`, `Blue` (`std::uint16_t`): свет каналов по кривой sRGB (`BoxColorLight`), уже умноженный на `Alpha`.
- `Alpha` (`std::uint16_t`): непрозрачность.

**Возвращает:** Неприменимо: агрегат, 8 байт; сравнение `==` поэлементное. Прозрачный пиксель — все четыре нуля.

**Побочные эффекты:** Нет.

**Как работает:** Наложенный на другой, пиксель добавляет свой свет к тому, что он оставляет от нижнего. Свет каждого канала никогда не превышает непрозрачность: это инвариант, который движок поддерживает и при чтении чужих пикселей (`Foreign`, `TakePixels`).

**Пример:**
```cpp
#include "painter/core/pixel.h"

constexpr painter::core::Pixel Clear{};
static_assert(Clear.Alpha == 0);
```

### `Pixel::Of`

**Краткое описание:** Возвращает пиксель для цвета.

**Параметры:**
- `From` (`Color`): цвет.

**Возвращает:** (`Pixel`) свет и непрозрачность цвета; цвет экрана — прозрачный пиксель (его рисует дисплей, а не холст).

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/pixel.h"

painter::core::Pixel Red() { return painter::core::Pixel::Of(painter::core::Colors::Red); }
```

### `Pixel::ToColor`

**Краткое описание:** Возвращает цвет, ближайший к пикселю.

**Параметры:** Нет.

**Возвращает:** (`Color`) свет, делённый на непрозрачность и переведённый обратно в числа sRGB; прозрачный пиксель — `Color::Through()`.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/canvas.h"

painter::core::Color At(const painter::core::Canvas &Out, std::uint32_t X, std::uint32_t Y) { return Out.Row(Y)[X].ToColor(); }
```

### `Pixel::Over`

**Краткое описание:** Накладывает один пиксель на другой с заданной долей.

**Параметры:**
- `Below` (`Pixel`): нижний.
- `Above` (`Pixel`): верхний.
- `Share` (`std::uint32_t`): доля верхнего из 65535.

**Возвращает:** (`Pixel`) результат: свет верхнего, умноженный на долю, плюс то, что он оставляет от нижнего.

**Побочные эффекты:** Нет.

**Пример:**
```cpp
#include "painter/core/pixel.h"

painter::core::Pixel Half(painter::core::Pixel Under, painter::core::Pixel Ink) { return painter::core::Pixel::Over(Under, Ink, 32768); }
```
