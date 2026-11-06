#ifndef TYPER_GLASS_H
#define TYPER_GLASS_H

/* typer on BoxOS: the glass it paints, the ear that hears the keys, and the
 * record file on the volume. What it judges and keeps is typer.h. */

#include "typer.h"

#include "painter/tui/stage.h"

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
using PainterColor = painter::core::Color;

constexpr PainterColor Paper      = PainterColor::Rgb(0x0C, 0x0C, 0x0C);
constexpr PainterColor BarPaper   = PainterColor::Rgb(0x3A, 0x3A, 0x3A);
constexpr PainterColor TitleInk   = painter::core::Colors::White;
constexpr PainterColor HintInk    = PainterColor::Rgb(0x8C, 0x8C, 0x8C);
constexpr PainterColor BrightInk  = PainterColor::Rgb(0xEE, 0xEE, 0xEE);
constexpr PainterColor AmberInk   = painter::core::Colors::Amber;
constexpr PainterColor GoodInk    = PainterColor::Rgb(0x8C, 0xE0, 0x8C);
constexpr PainterColor BadInk     = PainterColor::Rgb(0xFF, 0x70, 0x70);

enum class Heard { Key, Quiet, Gone };

struct Figure {
    std::string Label;
    std::string Value;
    PainterColor Ink;
};

/* The whole screen: a Painter stage owns its frame, cursor, teardown, and
 * typed keyboard events while the typing model stays independent. */
class Screen {
public:
    /* A refusal is a sentence; none means the screen is held. */
    std::optional<std::string> Take();
    void                       Give();

    void          Clear(PainterColor Fill);
    void          Put(std::uint32_t Row, std::uint32_t Col, char Ch, PainterColor Fg, PainterColor Bg);
    std::uint32_t Write(std::uint32_t Row, std::uint32_t Col, std::string_view Text, PainterColor Fg,
                        PainterColor Bg);
    void          Fill(std::uint32_t Row, std::uint32_t Col, std::uint32_t Width, char Ch, PainterColor Fg,
                       PainterColor Bg);
    /* Centred on the row; the column it began at. */
    std::uint32_t Centre(std::uint32_t Row, std::string_view Text, PainterColor Fg, PainterColor Bg);
    painter::tui::Cell At(std::uint32_t Row, std::uint32_t Col) const
    {
        return Glass->FrameBuffer().Read({static_cast<std::int32_t>(Col), static_cast<std::int32_t>(Row)});
    }
    bool          Flush();
    /* Label-and-value pairs side by side, centred on the row. */
    void          Spread(std::uint32_t Row, const std::vector<Figure> &Figures);

    /* Wait up to TimeoutMs for a key (0: until one comes). */
    Heard Listen(painter::tui::KeyEvent &Key, std::uint32_t TimeoutMs);

    std::uint32_t Rows = 0;
    std::uint32_t Cols = 0;

private:
    std::optional<painter::tui::Stage> Glass;
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
