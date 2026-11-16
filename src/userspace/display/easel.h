#ifndef BOXOS_DISPLAY_EASEL_H
#define BOXOS_DISPLAY_EASEL_H

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

#include "box/color.h"
#include "box/cxx/error.h"
#include "box/display.h"
#include "box/screen.h"
#include "box/touch.h"
#include "boxos_brush.h"
#include "text_cell.h"

class Typesetter;

// A cell as the glass is to show it: the console's text cell — its colours a
// triple each, or COLOR_DEFAULT for the screen's own — and the mark a layer
// drew in it (DisplayCellMark), none in the console's own text.
struct GlassCell {
    TextCell      Cell{};
    std::uint32_t MarkColor = 0;
    std::uint8_t  Mark = DISPLAY_CELL_MARK_NONE;
};

// Where the caret stands, in cells, and whether it is shown.
struct Caret {
    std::uint32_t X = 0;
    std::uint32_t Y = 0;
    bool Visible = false;
};

// The easel — where the display puts the picture on the glass: the screen the
// kernel lends this program, and the only hand that draws on it
// (box/screen.h). A cell is drawn into the framebuffer as the typesetter's
// picture of its character (typesetter.h), each pixel the ink's colour and
// the paper's mixed as light by how much of it the picture covers, or as
// glyph and attribute into the text page; a cell is drawn only when it
// differs from what the glass already shows, and video memory is never read.
// The caret is drawn here too, by software: an eighth of its cell at the
// foot on a framebuffer, the attribute turned over on the text page. So is a
// cell's mark: a line under the glyph or a bar down its left side on a
// framebuffer, the mark's colour behind the glyph on the text page. And the
// screen's own colours are here: a colour no program chose (COLOR_DEFAULT) is
// drawn as the text colour, or behind the text as the page colour.
class Easel {
public:
    // The screen, claimed — after waiting for it to be free if the program
    // that holds it is ending. One a living program holds is not waited for
    // (errc access_denied): it may be held for good.
    static std::expected<Easel, box::error> Claim() noexcept;

    Easel(Easel &&Other) noexcept;
    Easel &operator=(Easel &&) = delete;
    Easel(const Easel &) = delete;
    Easel &operator=(const Easel &) = delete;
    ~Easel();

    std::uint32_t Cols() const noexcept { return ColsValue; }
    std::uint32_t Rows() const noexcept { return RowsValue; }

    // Whether the glass is a framebuffer, not a text page.
    bool Pixels() const noexcept { return Description.kind == SCREEN_KIND_PIXELS; }

    // How many pixels tall and wide a framebuffer is; 0 for a text page.
    std::uint32_t PixelHeight() const noexcept { return Pixels() ? Description.height : 0; }
    std::uint32_t PixelWidth() const noexcept { return Pixels() ? Description.width : 0; }

    // What the loader left on the glass, as the claim says (SCREEN_LEFT_*),
    // and the fid of the scene whose first frame it is (SCREEN_LEFT_SCENE).
    std::uint32_t Left() const noexcept { return Description.left; }
    std::uint64_t LeftScene() const noexcept { return Description.scene; }

    // Count rectangles of a canvas as big as a framebuffer onto it, pixel for
    // pixel (boxos_brush.h BoxBrushShow) — nothing while the Emergency Screen
    // has the glass. What the console shows there is no longer known to be
    // on it.
    void Show(const BoxBrushCanvas &Canvas, const BoxBrushRect *Rects, std::uint32_t Count) noexcept;

    // The typesetter whose pictures a framebuffer shows, alive as long as the
    // easel: the console as many of its cells as the glass holds, at least
    // one — a cell past the glass's edge drawn as far as the glass goes —
    // in the middle of the glass, the pixels round them in the page colour.
    // Given before the first Present; until then a framebuffer has no cells.
    // A text page has its own glyphs and cells, and is not given one.
    void SetTypesetter(Typesetter &With);

    // The screen's own colours, as COLOR_DEFAULT is drawn in them.
    std::uint32_t TextColor() const noexcept { return TextColour; }
    std::uint32_t PageColor() const noexcept { return PageColour; }

    // The screen's turns, said on the `screen` Touch: Ctrl+Alt+Esc showing the
    // kernel's Emergency Screen (nothing is drawn while it does) and giving
    // the glass back (all of it is drawn again). True when the next Present
    // has something to do.
    bool Step() noexcept;

    // The screen's text colour and page colour, two triples: what the glass
    // shows in the old ones is drawn again at the next Present.
    void Page(std::uint32_t Text, std::uint32_t PageValue) noexcept;

    // Cols() x Rows() composed cells onto the glass, and the caret.
    void Present(std::span<const GlassCell> Cells, Caret At) noexcept;

private:
    Easel(const ScreenDescription &Description, TouchTag Turns) noexcept;

    void DrawCell(std::uint32_t Col, std::uint32_t Row, const GlassCell &Cell,
                  bool CaretHere) noexcept;
    void PaintMargin() noexcept;

    ScreenDescription Description{};
    Typesetter *Type = nullptr;
    TouchTag Turns = TOUCH_TAG_INVALID;
    std::uint32_t ColsValue = 0;
    std::uint32_t RowsValue = 0;
    std::uint32_t OriginX = 0;     // where a framebuffer's first cell stands
    std::uint32_t OriginY = 0;
    std::vector<GlassCell> Shown;  // what the glass shows now
    std::uint32_t TextColour = COLOR_LIGHT_GRAY;
    std::uint32_t PageColour = COLOR_BLACK;
    bool ShownKnown = false;       // false: nothing on the glass is ours — draw it all
    Caret Drawn;                   // where the caret is on the glass
    bool Away = false;
    bool Held = false;
};

#endif
