#ifndef BOXOS_DISPLAY_CONSOLE_MODEL_H
#define BOXOS_DISPLAY_CONSOLE_MODEL_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "box/display.h"

class ConsoleModel {
public:
    ConsoleModel(std::uint32_t Width, std::uint32_t Height);

    std::uint32_t Width() const noexcept { return WidthValue; }
    std::uint32_t Height() const noexcept { return HeightValue; }
    std::uint32_t CursorX() const noexcept { return CursorColumn; }
    std::uint32_t CursorY() const noexcept { return CursorRow; }
    const std::vector<TextCell> &Cells() const noexcept { return Surface; }

    void Apply(const ConsoleRun &Run);

private:
    void Clear(std::uint32_t Foreground, std::uint32_t Background);
    void Step(std::int32_t Delta);
    void Write(const char *Bytes, std::size_t Length,
               std::uint32_t Foreground, std::uint32_t Background);
    void WriteCell(char Glyph, std::uint32_t Foreground, std::uint32_t Background);
    void Newline();
    void CarriageReturn();
    void Backspace(std::uint32_t Foreground, std::uint32_t Background);
    void Tab();
    void Scroll();

    std::uint32_t WidthValue;
    std::uint32_t HeightValue;
    std::uint32_t CursorColumn = 0;
    std::uint32_t CursorRow = 0;
    std::vector<TextCell> Surface;
};

#endif
