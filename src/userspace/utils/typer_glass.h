#ifndef TYPER_GLASS_H
#define TYPER_GLASS_H

/* typer on BoxOS: the glass it paints, the ear that hears the keys, and the
 * record file on the volume. What it judges and keeps is typer.h. */

#include "typer.h"

#include "box/color.h"
#include "box/keyboard.h"
#include "box/touch.h"
#include "box/vga.h"

constexpr std::uint32_t ScreenColsLeast = 80;
constexpr std::uint32_t ScreenRowsLeast = 25;

/* Every colour here is chosen for both screens. The text console projects each
 * #rrggbb by dominant hue (BoxColorToVga4, boxos_color.h): a colour whose
 * saturation (chroma over max, in 255ths) is under 85 is a grey, by lightness
 * (L=(max+min)/2) black under 43, dark grey under 128, light grey under 213,
 * white above; anything more saturated takes its hue family, dark under L 128
 * and light above. So the paper and bars are exact greys: a faintly blue
 * #0E1118 came out as bright blue on the whole background.
 *   Paper   #0c0c0c L12  black        BarPaper #3a3a3a L58  dark grey
 *   HintInk #8c8c8c L140 light grey   BrightInk #eeeeee L238 white
 *   AmberInk yellow (light), GoodInk light green, BadInk light red */
constexpr Color Paper      = COLOR_RGB(0x0C, 0x0C, 0x0C);
constexpr Color BarPaper   = COLOR_RGB(0x3A, 0x3A, 0x3A);
constexpr Color TitleInk   = COLOR_WHITE;
constexpr Color HintInk    = COLOR_RGB(0x8C, 0x8C, 0x8C);
constexpr Color BrightInk  = COLOR_RGB(0xEE, 0xEE, 0xEE);
constexpr Color AmberInk   = COLOR_AMBER;
constexpr Color GoodInk    = COLOR_RGB(0x8C, 0xE0, 0x8C);
constexpr Color BadInk     = COLOR_RGB(0xFF, 0x70, 0x70);

enum class Heard { Key, Quiet, Gone };

struct Figure {
    std::string Label;
    std::string Value;
    Color       Ink;
};

/* The whole glass: a frame of cells laid down by one vga_paint, and the ear
 * that hears the keys. */
class Screen {
public:
    /* A refusal is a sentence; none means the screen is held. */
    std::optional<std::string> Take();
    void                       Give();

    void          Clear(Color Fill);
    void          Put(std::uint32_t Row, std::uint32_t Col, char Ch, Color Fg, Color Bg);
    std::uint32_t Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text, Color Fg, Color Bg);
    void          Fill(std::uint32_t Row, std::uint32_t Col, std::uint32_t Width, char Ch, Color Fg, Color Bg);
    /* Centred on the row; the column it began at. */
    std::uint32_t Centre(std::uint32_t Row, std::string_view Text, Color Fg, Color Bg);
    const TextCell &At(std::uint32_t Row, std::uint32_t Col) const { return Cells[Row * Cols + Col]; }
    bool          Flush();
    /* Label-and-value pairs side by side, centred on the row. */
    void          Spread(std::uint32_t Row, const std::vector<Figure> &Figures);

    /* Wait up to TimeoutMs for a key (0: until one comes). */
    Heard Listen(kb_event_t &Key, std::uint32_t TimeoutMs);

    std::uint32_t Rows = 0;
    std::uint32_t Cols = 0;

private:
    std::vector<TextCell> Cells;
    TouchTag              Ear = TOUCH_TAG_INVALID;
};

/* The width the passage is wrapped to on a screen this wide. */
std::uint32_t TextWidth(std::uint32_t Cols);

struct TypingView {
    const Attempt           &Run;
    const Task              &Job;
    const std::vector<Line> &Lines;
    std::string_view         Note;
    std::uint64_t            NowMs;
};

void PaintTyping(Screen &Glass, const TypingView &View);

enum class Standing { NoRecord, First, Beaten, Short };

struct Report {
    const Attempt &Run;
    const Task    &Job;
    std::uint64_t  ElapsedMs;
    Standing       Place;
    unsigned       Best;     /* the speed to beat, when there was one */
    std::string    Trouble;  /* why the record was not read or kept */
};

/* The results; one beat of the flourish a call. */
void PaintResults(Screen &Glass, const Report &Result, unsigned Beat);

/* The record file's text, empty when there is none yet. A refusal is a sentence. */
std::optional<std::string> ReadRecordText(std::string &Text);
std::optional<std::string> AppendRecordLine(std::string_view Line);

#endif /* TYPER_GLASS_H */
