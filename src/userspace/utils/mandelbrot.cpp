/* mandelbrot — the Mandelbrot set, to look into.
 *
 *   mandelbrot             the whole set
 *   mandelbrot tour        a slow fall into its famous places, one after another
 *   mandelbrot fire        begun in a palette: classic, fire, ocean, candy, aurora
 *
 *   arrows                 look about                  + -   nearer, farther
 *   j                      the Julia set of the point in the middle, and back
 *   shift+arrows           in a Julia set, its point moved: the set changes shape
 *   c                      the next palette            a     the colours turning
 *   , .                    colours closer, wider       [ ]   less, more detail
 *   space                  the tour, and out of it     r     the whole set again
 *   h                      the bar, hidden and shown   q, ESC  leave
 *
 * The working out and the colours are mandelbrot_paint.cpp. On a framebuffer
 * the set is drawn in the screen's own pixels, on a canvas of the program's
 * (painter::gui), its bar a line of words on a plate beneath; on a text page
 * in half-block pixels, two to a cell (painter::tui). A move restarts
 * the picture coarse and it sharpens while the keys are heard; once it is
 * whole and nothing turns, the program waits on the keys alone. */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <new>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "box/alarm.h"
#include "box/clock.h"
#include "box/form.h"
#include "box/print.h"

#include "mandelbrot.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/gui/measure.h"
#include "painter/tui/keyboard.h"
#include "painter/tui/text.h"
#include "painter/tui/stage.h"

BOX_FORM("takes optional words \"tour, julia, or a palette: classic, fire, ocean, candy, aurora\"\n");

namespace {

using Color = painter::core::Color;
namespace gui = painter::gui;
using painter::tui::EventKind;
using painter::tui::KeyEvent;
using painter::tui::Stage;
using painter::tui::StageError;
using painter::tui::TextStyle;

constexpr std::uint64_t WorkUs     = 40000;   /* of working out between two frames */
constexpr std::uint64_t TurnUs     = 40000;   /* a frame while the colours turn */
constexpr float         KeysShown  = 8.0f;
constexpr double        DeepestWidth = 2e-13;   /* past this a double has no more to say */
constexpr double        Nearer     = 1.5;
constexpr double        TourIn     = 0.82;    /* each whole picture of the tour */
constexpr double        TourOut    = 1.0 / 0.55;

const Color BarPaper = Color::Rgb(0x3A, 0x3A, 0x3A);
const Color BarInk   = Color::Rgb(0xEE, 0xEE, 0xEE);
const Color KeyInk   = painter::core::Colors::Amber;
const Color HintInk  = Color::Rgb(0x8C, 0x8C, 0x8C);

/* Where the tour goes: points whose neighbourhood stays rich as deep as a
 * double reaches — Seahorse Valley, the double spiral near it, and two
 * Misiurewicz points, whose dendrites repeat themselves forever. */
struct Sight {
    double X, Y;
    const char *Name;
};
const Sight Sights[] = {{-0.743643887037151, 0.131825904205330, "seahorse valley"},
                        {-0.77568377, 0.13646737, "double spiral"},
                        {0.0, 1.0, "the dendrite at i"},
                        {-0.10109636384562, 0.95628651080914, "a Misiurewicz point"}};

enum class Bar { Keys, Status, Hidden };

/* Words of the bar, each in its colour. */
struct Words {
    std::string Said;
    Color       Ink;
};

/* What was heard while the explorer waited. */
enum class Heard { Quiet, Key, Gone };

/* Where the picture is shown — what the explorer asks of a glass. */
class Glass {
public:
    virtual ~Glass() = default;
    // The picture's size, in the glass's pixels.
    virtual painter::core::Size Pixels() const = 0;
    // How many characters of the bar's words go across.
    virtual int Across() const = 0;
    // The picture, Pixels() of colours row by row, of which rows [first,
    // past) changed; the bar's words, or none for no bar.
    virtual void Show(const std::vector<Color> &Picture, std::pair<int, int> Rows, const std::vector<Words> *Line) = 0;
    virtual bool Present() = 0;
    // A key, the keyboard gone, or Ms milliseconds gone by (0: wait on).
    virtual Heard Wait(std::uint32_t Ms, KeyEvent &Key) = 0;
    virtual void  Close() = 0;
};

/* A text page: half-block pixels, two to a cell, the bar the last row. */
class CellsGlass final : public Glass {
public:
    explicit CellsGlass(Stage Opened) : Screen(std::move(Opened)) {}
    painter::core::Size Pixels() const override { return {Screen.Size().Width, Screen.Size().Height * 2}; }
    int                 Across() const override { return Screen.Size().Width - 2; }
    void Show(const std::vector<Color> &Picture, std::pair<int, int>, const std::vector<Words> *Line) override
    {
        painter::tui::Frame &FrameBuffer = Screen.FrameBuffer();
        FrameBuffer.PutPixels(Picture);
        if (!Line) return;
        const painter::core::Size Dimensions = Screen.Size();
        const int                 Row        = Dimensions.Height - 1;
        FrameBuffer.Fill({0, Row, Dimensions.Width, 1}, ' ', TextStyle{BarInk, BarPaper});
        int Col = 1;
        for (const Words &Each : *Line) Col += static_cast<int>(FrameBuffer.Write({Col, Row}, Each.Said, TextStyle{Each.Ink, BarPaper}));
    }
    bool  Present() override { return static_cast<bool>(Screen.Present()); }
    Heard Wait(std::uint32_t Ms, KeyEvent &Key) override
    {
        const painter::tui::Event Event = Screen.Wait(Ms);
        if (Event.Kind == EventKind::Gone) return Heard::Gone;
        if (Event.Kind != EventKind::Key) return Heard::Quiet;
        Key = Event.Key;
        return Heard::Key;
    }
    void Close() override { (void)Screen.Close(); }

private:
    Stage Screen;
};

/* A framebuffer: the whole screen while the program is in front, the set on
 * a canvas of its own pixels, the bar a line of words on a plate beneath. */
class PixelsGlass final : public Glass {
public:
    PixelsGlass(gui::Area Opened, painter::tui::Keyboard Heard)
        : Screen(std::move(Opened)), Keys(std::move(Heard)),
          Board(Screen.Canvas({static_cast<std::int32_t>(std::lround(Screen.Size().X * Screen.Scale())),
                               static_cast<std::int32_t>(std::lround(Screen.Size().Y * Screen.Scale()))})),
          Plate(Screen.Rectangle()), Line(Screen.Text("", gui::Role::Code))
    {
        const painter::core::PointF Letter = gui::Measure(Screen, "0", gui::Role::Code);
        Advance                            = std::max(1.0f, Letter.X);
        High                               = Letter.Y + 2 * Inset;
        Plate.SetPlace({0, Screen.Size().Y - High}).SetSize({Screen.Size().X, High}).SetFill(gui::Fill::Of(BarPaper));
        Line.SetPlace({Advance, Screen.Size().Y - High + Inset});
    }
    painter::core::Size Pixels() const override { return Board.Size(); }
    int                 Across() const override { return static_cast<int>(Screen.Size().X / Advance) - 2; }
    void Show(const std::vector<Color> &Picture, std::pair<int, int> Rows, const std::vector<Words> *Said) override
    {
        /* Only the rows that changed drawn and given: the rest of the canvas
         * is the picture shown. */
        const painter::core::Size Size = Board.Size();
        const std::int32_t        From = std::clamp(Rows.first, 0, Size.Height), Past = std::clamp(Rows.second, 0, Size.Height);
        if (From < Past)
            (void)Board.Draw(
                [&](painter::core::Canvas &Page) {
                    for (std::int32_t Y = From; Y < Past; Y++) {
                        painter::core::Pixel *Row = Page.Row(static_cast<std::uint32_t>(Y));
                        const Color *Colours = Picture.data() + static_cast<std::size_t>(Y) * static_cast<std::size_t>(Size.Width);
                        for (std::int32_t X = 0; X < Size.Width; X++) Row[X] = painter::core::Pixel::Of(Colours[X]);
                    }
                },
                painter::core::Rect{0, From, Size.Width, Past - From});
        if (!Said) {
            Plate.Hide();
            Line.Hide();
            return;
        }
        std::string            Text;
        std::vector<gui::Span> Spans;
        for (const Words &Each : *Said) {
            gui::TextStyle Ink;
            Ink.Color = Each.Ink;
            Spans.push_back(gui::Span{static_cast<std::uint32_t>(Text.size()), static_cast<std::uint32_t>(Text.size() + Each.Said.size()), Ink});
            Text += Each.Said;
        }
        Plate.Show();
        Line.SetText(Text).SetSpans(Spans).Show();
    }
    bool  Present() override { return Screen.Present(); }
    Heard Wait(std::uint32_t Ms, KeyEvent &Key) override
    {
        if (Ms) (void)alarm_set_us(clock_uptime_us() + std::uint64_t{Ms} * 1000u);
        else (void)alarm_cancel();
        for (;;) {
            const gui::Event Event = Screen.Wait(&Keys);
            switch (Event.What) {
            case gui::Event::Kind::Key: Key = Event.Key; return Heard::Key;
            case gui::Event::Kind::Gone: return Heard::Gone;
            case gui::Event::Kind::Alarm: return Heard::Quiet;
            default: break;
            }
        }
    }
    void Close() override { (void)Keys.Close(); }

private:
    static constexpr float Inset = 3;
    gui::Area              Screen;
    painter::tui::Keyboard Keys;
    gui::Canvas            Board;
    gui::Shape             Plate;
    gui::Text              Line;
    float                  Advance = 8, High = 20;
};

class Explorer {
public:
    std::unique_ptr<Glass> Screen;

    void Begin(const char *const *Words, std::size_t Count);
    /* False to leave. */
    bool OnKey(const KeyEvent &Key);
    void Step(float Seconds);
    void Draw(float Shown);
    std::uint32_t Wait() const;

    double Nearest = 1e9;   /* the narrowest view looked at */
    double HomeWidth = 3.4; /* the whole set, as wide as it must be to be whole on this glass */

private:
    FractalView HomeView(bool Julia, double Jx = 0, double Jy = 0) const
    {
        return Julia ? FractalView{0.0, 0.0, HomeWidth, true, Jx, Jy} : FractalView{-0.6, 0.0, HomeWidth, false, 0, 0};
    }

    void Restart();
    void Zoom(double By);
    void Pan(int Dx, int Dy);
    void Julia();
    void Tour();
    std::vector<Words> StatusBar() const;
    std::vector<Words> KeysBar() const;

    FractalView        View;
    FractalView        Saved;      /* the Mandelbrot view a Julia set was opened from */
    FractalPicture     Picture;
    std::vector<Color> Pixels;
    int                Palette  = 0;
    float              Spread   = 24.0f;
    float              Turn     = 0.0f;
    bool               Turning  = false;
    double             Detail   = 1.0;
    bool               Recolour = true;    /* every pixel coloured anew at the next draw */
    float              Coloured = 0.0f;    /* the spread the pixels were coloured with */
    Bar                Showing  = Bar::Keys;
    bool               Touring  = false;
    bool               Leaving  = false;   /* the tour on its way out of a sight */
    std::size_t        Visiting = 0;
    std::uint64_t      Began    = 0;
};

void Explorer::Begin(const char *const *Words, std::size_t Count)
{
    const painter::core::Size Dimensions = Screen->Pixels();
    Picture.Resize(Dimensions.Width, Dimensions.Height);
    /* The set is about 3.4 wide and 2.6 high: the view is as wide as both need. */
    HomeWidth = std::max(3.4, 2.6 * Dimensions.Width / static_cast<double>(Dimensions.Height));
    View      = HomeView(false);
    Began     = clock_uptime_us();
    for (std::size_t At = 0; At < Count; At++) {
        if (std::strcmp(Words[At], "tour") == 0) Touring = true;
        if (std::strcmp(Words[At], "julia") == 0) Julia();
        for (int Which = 0; Which < FractalPaletteCount(); Which++)
            if (std::strcmp(Words[At], FractalPalettes(Which).Name) == 0) Palette = Which;
    }
    Restart();
}

void Explorer::Restart()
{
    Nearest = std::min(Nearest, View.Width);
    Picture.Restart(View, FractalSteps(View, Detail));
}

void Explorer::Zoom(double By)
{
    View.Width = std::clamp(View.Width / By, DeepestWidth, HomeWidth * 2);
    Restart();
}

void Explorer::Pan(int Dx, int Dy)
{
    View.CentreX += Dx * View.Width / 8;
    View.CentreY += Dy * View.Width / 8;
    Restart();
}

/* The Julia set of the point in the middle, drawn whole; and back to where
 * the Mandelbrot set was left. */
void Explorer::Julia()
{
    if (View.Julia) {
        View = Saved;
    } else {
        Saved = View;
        View  = HomeView(true, Saved.CentreX, Saved.CentreY);
    }
    Touring = false;
    Restart();
}

/* A step of the tour, taken once the picture before it is whole: nearer the
 * sight, the sight staying where it is on the glass; out again once a double
 * can go no deeper, and on to the next. */
void Explorer::Tour()
{
    const Sight &To = Sights[Visiting];
    if (!Leaving && View.Width * TourIn < DeepestWidth) Leaving = true;
    if (Leaving && View.Width >= HomeWidth) {
        Leaving = false;
        Visiting = (Visiting + 1) % (sizeof(Sights) / sizeof(Sights[0]));
        View    = HomeView(false);
        Restart();
        return;
    }
    double By    = Leaving ? TourOut : TourIn;
    View.Width   = std::min(HomeWidth, View.Width * By);
    View.CentreX = To.X + (View.CentreX - To.X) * (Leaving ? 1.0 : By);
    View.CentreY = To.Y + (View.CentreY - To.Y) * (Leaving ? 1.0 : By);
    Restart();
}

bool Explorer::OnKey(const KeyEvent &Key)
{
    if (Key.IsExtended()) {
        bool Shift = Key.IsShift();
        int  Dx = 0, Dy = 0;
        switch (Key.Scancode) {
        case painter::tui::KeyCodes::Left:  Dx = -1; break;
        case painter::tui::KeyCodes::Right: Dx = 1; break;
        case painter::tui::KeyCodes::Up:    Dy = 1; break;
        case painter::tui::KeyCodes::Down:  Dy = -1; break;
        case painter::tui::KeyCodes::PageUp:  Zoom(Nearer); return true;
        case painter::tui::KeyCodes::PageDown:  Zoom(1 / Nearer); return true;
        default: return true;
        }
        Touring = false;
        if (Shift && View.Julia) {
            View.JuliaX += Dx * 0.005;
            View.JuliaY += Dy * 0.005;
            Restart();
        } else {
            Pan(Dx, Dy);
        }
        return true;
    }
    char32_t Ch = Key.Character;
    if (Ch >= 'A' && Ch <= 'Z') Ch = static_cast<char>(Ch - 'A' + 'a');
    switch (Ch) {
    case 'q':
    case painter::tui::KeyCodes::Escape: return false;
    case '+':
    case '=':
    case '\n':
    case '\r': Touring = false; Zoom(Nearer); break;
    case '-':
    case '_':  Touring = false; Zoom(1 / Nearer); break;
    case 'j':  Julia(); break;
    case 'c':  Palette = (Palette + 1) % FractalPaletteCount(); Recolour = true; break;
    case 'a':  Turning = !Turning; break;
    case ',':  Spread = std::max(4.0f, Spread / 1.25f); Recolour = true; break;
    case '.':  Spread = std::min(400.0f, Spread * 1.25f); Recolour = true; break;
    case '[':  Detail = std::max(0.25, Detail / 1.4); Restart(); break;
    case ']':  Detail = std::min(16.0, Detail * 1.4); Restart(); break;
    case ' ':
        if (!Touring && View.Julia) Julia();
        Touring = !Touring;
        Leaving = false;
        break;
    case 'r':
        Touring = false;
        View    = HomeView(View.Julia, View.JuliaX, View.JuliaY);
        Restart();
        break;
    case 'h':  Showing = Showing == Bar::Hidden ? Bar::Status : Bar::Hidden; break;
    default: break;
    }
    return true;
}

/* Time passing: the picture worked on, the tour on once it is whole, the
 * colours turned. */
void Explorer::Step(float Seconds)
{
    if (!Picture.Whole()) Picture.Work(WorkUs);
    else if (Touring) Tour();
    if (Turning) {
        Turn     = std::fmod(Turn + Seconds * 48.0f, 256.0f);
        Recolour = true;
    }
}

std::uint32_t Explorer::Wait() const
{
    if (!Picture.Whole() || Touring) return 1;
    if (Turning) return static_cast<std::uint32_t>(TurnUs / 1000);
    /* The keys give way to where the view is once they have been read. */
    std::uint64_t Shown = clock_uptime_us() - Began, Until = static_cast<std::uint64_t>(KeysShown * 1e6f);
    if (Showing == Bar::Keys) return Shown >= Until ? 1 : static_cast<std::uint32_t>((Until - Shown) / 1000 + 1);
    return 0;
}

std::vector<Words> Explorer::StatusBar() const
{
    const int          Digits = std::clamp(static_cast<int>(std::log10(HomeWidth / View.Width)) + 4, 4, 15);
    const double       X = View.Julia ? View.JuliaX : View.CentreX, Y = View.Julia ? View.JuliaY : View.CentreY;
    std::string        More = std::format("{} steps  {}", FractalSteps(View, Detail), FractalPalettes(Palette).Name);
    if (Touring) More += std::format("  tour: {}", Sights[Visiting].Name);
    if (!Picture.Whole()) More += "  drawing";
    return {{View.Julia ? "julia" : "mandelbrot", KeyInk},
            {std::format("  {}{:.{}f} {:+.{}f}i", View.Julia ? "c = " : "", X, Digits, Y, Digits), BarInk},
            {std::format("  x{:.3g}", HomeWidth / View.Width), BarInk},
            {"  " + More, HintInk}};
}

std::vector<Words> Explorer::KeysBar() const
{
    std::vector<Words> Line;
    int                Used = 0;
    for (auto [Keys, What] : {std::pair{"\u2190\u2191\u2193\u2192", "look"}, {"+-", "zoom"}, {"j", "julia"}, {"c", "colours"},
                              {"a", "turn"}, {"space", "tour"}, {"[]", "detail"}, {"r", "home"}, {"q", "leave"}}) {
        const int Wide = static_cast<int>(painter::tui::CellsOf(Keys) + std::string_view(What).size()) + 3;
        if (Used + Wide > Screen->Across()) break;
        Line.push_back({Keys, KeyInk});
        Line.push_back({std::string(" ") + What + "  ", HintInk});
        Used += Wide;
    }
    return Line;
}

void Explorer::Draw(float Shown)
{
    /* Counts run longer the deeper the view: the palette spreads with them, or a
     * deep picture is colour noise from pixel to pixel. */
    const float Wider = std::sqrt(static_cast<float>(FractalSteps(View, Detail)) / 150.0f);
    /* Coloured anew where the counts changed — every pixel when the colours
     * or how widely they spread changed. */
    std::pair<int, int> Rows = Picture.TakeTouched();
    if (Recolour || Spread * Wider != Coloured) {
        Rows     = {0, Picture.Height};
        Recolour = false;
        Coloured = Spread * Wider;
    }
    if (Rows.first < Rows.second) FractalColour(Picture, FractalPalettes(Palette), Coloured, Turn, Pixels, Rows.first, Rows.second);
    if (Showing == Bar::Keys && Shown >= KeysShown) Showing = Bar::Status;
    if (Showing == Bar::Hidden) {
        Screen->Show(Pixels, Rows, nullptr);
        return;
    }
    const std::vector<Words> Line = Showing == Bar::Keys ? KeysBar() : StatusBar();
    Screen->Show(Pixels, Rows, &Line);
}

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

/* The glass the screen has: a framebuffer's pixels — the whole screen while
 * the program is in front, and the keyboard — or a text page's cells. */
std::unique_ptr<Glass> OpenGlass()
{
    if (std::expected<gui::Area, box::error> Area = gui::Area::Open(gui::Placement::Whole); Area && Area->On() == gui::Glass::Pixels)
        if (std::expected<painter::tui::Keyboard, StageError> Keys = painter::tui::Keyboard::Open())
            return std::make_unique<PixelsGlass>(std::move(*Area), std::move(*Keys));
    std::expected<Stage, StageError> Opened = Stage::Open(40, 12);
    if (!Opened) {
        Say(Opened.error().Describe("mandelbrot"));
        return nullptr;
    }
    return std::make_unique<CellsGlass>(std::move(*Opened));
}

}

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;

        Explorer Look;
        Look.Screen = OpenGlass();
        if (!Look.Screen) return 1;
        Look.Begin(Words, Count);

        std::uint64_t Began = clock_uptime_us(), Last = Began;
        bool          Painted = true;
        for (;;) {
            std::uint64_t Now = clock_uptime_us();
            Look.Step(static_cast<float>(Now - Last) / 1e6f);
            Last = Now;
            Look.Draw(static_cast<float>(Now - Began) / 1e6f);
            if (!(Painted = Look.Screen->Present())) break;

            KeyEvent    Key;
            const Heard Said = Look.Screen->Wait(Look.Wait(), Key);
            if (Said == Heard::Gone) break;
            if (Said == Heard::Key && !Look.OnKey(Key)) break;
        }
        double Deepest = Look.HomeWidth / Look.Nearest;
        Look.Screen->Close();
        Look.Screen.reset();
        Say(Painted ? std::format("[MANDELBROT] looked {:.3g} times nearer than the whole set", Deepest)
                    : "The screen stopped taking frames.");
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a picture the size of this screen.");
        return 1;
    }
}
