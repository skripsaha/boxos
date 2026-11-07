#ifndef STAGE_H
#define STAGE_H

/* stage — the whole glass, for a program that owns the screen.
 *
 * A frame of cells laid down by one vga_paint a time (one op, one commit,
 * nothing of it in the log ring), pixels two to a cell's height by the upper
 * half block, the ear that hears the keys, and the screen given back however
 * the program leaves — before anything it says, so its words land on a clear
 * console.
 *
 * Every colour a program lays here should keep its family on a sixteen-colour
 * text console too, which projects a saturated #rrggbb by hue (dark under
 * lightness 128, light above) and a grey by lightness (typer_screen.h spells
 * the rule out). */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "box/color.h"
#include "box/keyboard.h"
#include "box/touch.h"
#include "box/vga.h"

enum class StageHeard { Key, Quiet, Gone };

class Stage {
public:
    Stage() = default;
    ~Stage() { Give(); }
    Stage(const Stage &)            = delete;
    Stage &operator=(const Stage &) = delete;

    /* The screen held, at least LeastCols x LeastRows; a refusal is a sentence
     * for the person, `Who` the program. */
    std::optional<std::string> Take(std::string_view Who, int LeastCols, int LeastRows);
    /* Safe twice, and on a screen never taken. */
    void Give();

    void Clear(Color Paper);
    void Put(int Row, int Col, char Ch, Color Ink, Color Paper);
    void Fill(int Row, int Col, int Width, char Ch, Color Ink, Color Paper);
    /* The column after the text. */
    int  Write(int Row, int Col, std::string_view Text, Color Ink, Color Paper);
    /* Centred on the row; the column it began at. */
    int  Centre(int Row, std::string_view Text, Color Ink, Color Paper);
    TextCell &At(int Row, int Col) { return Cells[static_cast<std::size_t>(Row) * Cols + Col]; }

    /* Cols x Rows*2 pixels, row by row, laid as half blocks over the frame. */
    void Pixels(const std::vector<Color> &Pixels);

    /* The frame on the glass; false when the screen would not take it. */
    bool Show();

    /* A key within TimeoutMs (0: until one comes). */
    StageHeard Listen(kb_event_t &Key, std::uint32_t TimeoutMs);

    int Rows = 0;
    int Cols = 0;

private:
    std::vector<TextCell> Cells;
    TouchTag              Ear = TOUCH_TAG_INVALID;
};

/* Said to the person, past the glass, whatever an arrow says. */
void StageSay(std::string_view Text);

/* The console font's own glyphs, by what they are. */
namespace Glyph {
constexpr char UpperHalf  = static_cast<char>(0xDF);
constexpr char LowerHalf  = static_cast<char>(0xDC);
constexpr char Full       = static_cast<char>(0xDB);
constexpr char LightShade = static_cast<char>(0xB0);
constexpr char MidShade   = static_cast<char>(0xB1);
constexpr char Dot        = static_cast<char>(0xFA);
constexpr char Bullet     = static_cast<char>(0x07);
constexpr char Tick       = static_cast<char>(0xFB);
constexpr char Left       = 0x1B;
constexpr char Right      = 0x1A;
constexpr char Up         = 0x18;
constexpr char Down       = 0x19;
constexpr char Pointer    = 0x10;
}

#endif
