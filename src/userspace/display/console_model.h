#ifndef BOXOS_DISPLAY_CONSOLE_MODEL_H
#define BOXOS_DISPLAY_CONSOLE_MODEL_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "box/display.h"
#include "cell_grid.h"

// The console: the programs' text from their lanes and the kernel's lines,
// laid into its grid of cells (cell_grid.h) — the console's cells item, the
// lowest thing on the glass — in the console's one order.
class ConsoleModel {
public:
    // A console of Surface's size, written into it. Throws std::bad_alloc only.
    explicit ConsoleModel(CellGrid &Surface);

    std::uint32_t Width() const noexcept { return Grid.Cols(); }
    std::uint32_t Height() const noexcept { return Grid.Rows(); }
    std::uint32_t CursorX() const noexcept { return CursorColumn; }
    std::uint32_t CursorY() const noexcept { return CursorRow; }
    std::span<const ItemCell> Cells() const noexcept { return Grid.Cells(); }

    // The rows gone up off the console since it began — a clearing taking all
    // it held: a row of the console counted so is where an area in its flow
    // stays, as the console goes on.
    std::uint64_t RowsGone() const noexcept { return Gone; }

    // A frame of the lane `Lane` (numbered from 1, as FrameSink numbers them).
    void Apply(const ConsoleRun &Run, std::uint32_t Lane);

    // The lane that hears the keyboard now, 0 for none. Its unfinished line —
    // a prompt and what is typed after it — stays under whatever another lane,
    // the kernel or a fold says while it is typed: lifted, the other text laid
    // where it stood, and laid again below, the caret where it was in it.
    void Typing(std::uint32_t Lane) noexcept { TypingLane = Lane; }

    // A lane's program went behind while it read: its unfinished line — the
    // prompt and what is typed after it, when it is the line drawn last — is
    // kept. Back: the lane reads in front again, and the line kept is laid
    // again on a row of its own, the caret where it stood in it, so what the
    // reader echoes next lands where it means to. Forget: the lane closed,
    // and nothing of it is kept for whichever lane takes its number.
    void Away(std::uint32_t Lane);
    void Back(std::uint32_t Lane);
    void Forget(std::uint32_t Lane) noexcept;

    // The kernel's lines, from the log ring: `Count` bytes, each with its Tone
    // (boxos_log_tone.h), drawn in this console's colours for that tone.
    void WriteKernel(const char *Bytes, const std::uint8_t *Tones, std::size_t Count);

    // Rows of the console given to an area in its flow: on a row of their
    // own after the text so far, the console scrolled up for them as it must,
    // the caret below them. Where they begin, counted as RowsGone counts —
    // the first of them is that minus RowsGone() on the console.
    std::uint64_t Reserve(std::uint32_t Rows);

    // Cells folded into the text — an area's, its program done with them
    // (DISP_CMD_AREA_COMMIT): the console scrolled up ScrollRows first, then
    // each cell of Cells that shows something laid over the console's from
    // X, Y on, then the caret at CaretX, CaretY when Caret. The cells lie on
    // this console, Cells is Width x Height, ScrollRows is at most Height()
    // and the caret on the console — all checked before (areas.h
    // AreasCommit). Page is the screen's page colour, what a see-through cell
    // over the page shows.
    void Fold(std::int32_t X, std::int32_t Y, std::uint32_t Width, std::uint32_t Height,
              std::span<const ItemCell> Cells, std::uint32_t Page, std::uint32_t ScrollRows,
              std::uint32_t CaretX, std::uint32_t CaretY, bool Caret);

private:
    template <typename Drawing>
    void AsAnother(std::uint32_t Lane, bool Text, Drawing &&Said);
    void Begin(std::uint32_t Lane, bool Text) noexcept;
    bool Holds(std::uint32_t Lane) const noexcept;
    std::size_t Here() const noexcept;
    void MoveTo(std::size_t Linear) noexcept;
    void Lay(const ItemCell &Cell);
    void Draw(const ConsoleRun &Run);
    void Folded(std::int32_t X, std::int32_t Y, std::uint32_t Width, std::uint32_t Height,
                std::span<const ItemCell> Cells, std::uint32_t Page, std::uint32_t ScrollRows,
                std::uint32_t CaretX, std::uint32_t CaretY, bool Caret);
    void Reserved(std::uint32_t Rows);
    void Clear(std::uint32_t Foreground, std::uint32_t Background);
    void Step(std::int32_t Delta);
    void Write(const char *Bytes, std::size_t Length,
               std::uint32_t Foreground, std::uint32_t Background);
    void WriteCell(char32_t Character, std::uint32_t Foreground, std::uint32_t Background);
    void Newline();
    void CarriageReturn();
    void Backspace(std::uint32_t Foreground, std::uint32_t Background);
    void Tab();
    void Scroll();

    CellGrid     &Grid;
    std::uint32_t CursorColumn = 0;
    std::uint32_t CursorRow = 0;
    std::uint64_t Gone = 0;

    // The lane whose text was drawn last (0: the kernel's lines, or a fold),
    // and its unfinished line: where it began and how far it reaches, as cells
    // counted from the console's first. While the typing lane's line is held
    // under another's text, Others is where that text goes on, and OthersLane
    // the lane that wrote there last.
    std::uint32_t TypingLane = 0;
    std::uint32_t LastLane = 0;
    std::size_t   LineStart = 0;
    std::size_t   LineEnd = 0;
    bool          Held = false;
    std::size_t   Others = 0;
    std::uint32_t OthersLane = 0;

    // The typing line while it is lifted off the console (AsAnother), its
    // room made once for a whole console.
    std::vector<ItemCell> Lifted;

    // The lines of lanes gone behind while they read.
    struct Kept {
        std::uint32_t         Lane = 0;
        std::vector<ItemCell> Line;
        std::size_t           Caret = 0;   // where the caret stood in it
    };
    std::vector<Kept> KeptLines;
};

#endif
