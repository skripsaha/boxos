#ifndef BOXOS_DISPLAY_CONSOLE_MODEL_H
#define BOXOS_DISPLAY_CONSOLE_MODEL_H

#include <cstddef>
#include <cstdint>
#include <span>
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

    // The kernel's lines, from the log ring: `Count` bytes, each with its Tone
    // (boxos_log_tone.h), drawn in this console's colours for that tone.
    void WriteKernel(const char *Bytes, const std::uint8_t *Tones, std::size_t Count);

    // A layer folded into the text: the base scrolled up ScrollRows first,
    // then the opaque cells laid over it, then the caret where the layer
    // shows one. The region lies on this console, Cells is Width x Height,
    // ScrollRows is at most Height() and the caret inside the region — the
    // layer was checked before it was handed over (LayersCommit).
    void CommitLayer(std::int32_t X, std::int32_t Y, std::uint32_t Width,
                     std::uint32_t Height, std::span<const DisplayLayerCell> Cells,
                     std::uint32_t ScrollRows,
                     std::uint32_t CursorX, std::uint32_t CursorY,
                     bool CursorVisible);

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
