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
 * The working out and the colours are mandelbrot_paint.cpp. A move restarts
 * the picture coarse and it sharpens while the keys are heard; once it is
 * whole and nothing turns, the program waits on the keys alone. */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <new>
#include <optional>
#include <string>

#include "box/clock.h"
#include "box/form.h"
#include "box/print.h"

#include "mandelbrot.h"
#include "painter/tui/stage.h"

BOX_FORM("takes optional words \"tour, julia, or a palette: classic, fire, ocean, candy, aurora\"\n");

namespace {

using Color = painter::core::Color;
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

class Explorer {
public:
    std::optional<Stage> Screen;

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
    void StatusBar();
    void KeysBar();

    FractalView        View;
    FractalView        Saved;      /* the Mandelbrot view a Julia set was opened from */
    FractalPicture     Picture;
    std::vector<Color> Pixels;
    int                Palette  = 0;
    float              Spread   = 24.0f;
    float              Turn     = 0.0f;
    bool               Turning  = false;
    double             Detail   = 1.0;
    Bar                Showing  = Bar::Keys;
    bool               Touring  = false;
    bool               Leaving  = false;   /* the tour on its way out of a sight */
    std::size_t        Visiting = 0;
    std::uint64_t      Began    = 0;
};

void Explorer::Begin(const char *const *Words, std::size_t Count)
{
    const painter::core::Size Dimensions = Screen->Size();
    Picture.Resize(Dimensions.Width, Dimensions.Height * 2);
    /* The set is about 3.4 wide and 2.6 high: the view is as wide as both need. */
    HomeWidth = std::max(3.4, 2.6 * Dimensions.Width / (Dimensions.Height * 2.0));
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
    case 'c':  Palette = (Palette + 1) % FractalPaletteCount(); break;
    case 'a':  Turning = !Turning; break;
    case ',':  Spread = std::max(4.0f, Spread / 1.25f); break;
    case '.':  Spread = std::min(400.0f, Spread * 1.25f); break;
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
    if (Turning) Turn = std::fmod(Turn + Seconds * 48.0f, 256.0f);
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

void Explorer::StatusBar()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const painter::core::Size Dimensions = Screen->Size();
    int Row = Dimensions.Height - 1;
    int Col = 1;
    Col += static_cast<int>(FrameBuffer.Write({Col, Row}, View.Julia ? "julia" : "mandelbrot",
                                              TextStyle{KeyInk, BarPaper}));

    int         Digits = std::clamp(static_cast<int>(std::log10(HomeWidth / View.Width)) + 4, 4, 15);
    double      X = View.Julia ? View.JuliaX : View.CentreX, Y = View.Julia ? View.JuliaY : View.CentreY;
    std::string Where  = std::format("{}{:.{}f} {:+.{}f}i", View.Julia ? "c = " : "", X, Digits, Y, Digits);
    Col += 2;
    Col += static_cast<int>(FrameBuffer.Write({Col, Row}, Where, TextStyle{BarInk, BarPaper}));

    std::string Depth = std::format("x{:.3g}", HomeWidth / View.Width);
    Col += 2;
    Col += static_cast<int>(FrameBuffer.Write({Col, Row}, Depth, TextStyle{BarInk, BarPaper}));
    std::string More = std::format("{} steps  {}", FractalSteps(View, Detail), FractalPalettes(Palette).Name);
    if (Touring) More += std::format("  tour: {}", Sights[Visiting].Name);
    if (!Picture.Whole()) More += "  drawing";
    FrameBuffer.Write({Col + 2, Row}, More, TextStyle{HintInk, BarPaper});
}

void Explorer::KeysBar()
{
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    const painter::core::Size Dimensions = Screen->Size();
    int Row = Dimensions.Height - 1, Col = 1;
    for (auto [Keys, What] : {std::pair{"\x1B\x18\x19\x1A", "look"}, {"+-", "zoom"}, {"j", "julia"}, {"c", "colours"},
                              {"a", "turn"}, {"space", "tour"}, {"[]", "detail"}, {"r", "home"}, {"q", "leave"}}) {
        int Wide = static_cast<int>(std::string_view(Keys).size() + std::string_view(What).size()) + 3;
        if (Col + Wide > Dimensions.Width) break;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, Keys, TextStyle{KeyInk, BarPaper}));
        Col += 1;
        Col += static_cast<int>(FrameBuffer.Write({Col, Row}, What, TextStyle{HintInk, BarPaper}));
        Col += 2;
    }
}

void Explorer::Draw(float Shown)
{
    /* Counts run longer the deeper the view: the palette spreads with them, or a
     * deep picture is colour noise from pixel to pixel. */
    float Wider = std::sqrt(static_cast<float>(FractalSteps(View, Detail)) / 150.0f);
    FractalColour(Picture, FractalPalettes(Palette), Spread * Wider, Turn, Pixels);
    painter::tui::Frame &FrameBuffer = Screen->FrameBuffer();
    FrameBuffer.PutPixels(Pixels);
    if (Showing == Bar::Keys && Shown >= KeysShown) Showing = Bar::Status;
    if (Showing == Bar::Hidden) return;
    const painter::core::Size Dimensions = Screen->Size();
    FrameBuffer.Fill({0, Dimensions.Height - 1, Dimensions.Width, 1}, ' ', TextStyle{BarInk, BarPaper});
    if (Showing == Bar::Keys) KeysBar();
    else StatusBar();
}

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}

int main()
{
    try {
        const char *const *Words;
        std::size_t        Count;
        if (form_words(0, &Words, &Count) != 0) return 1;

        Explorer Look;
        std::expected<Stage, StageError> Opened = Stage::Open(40, 12);
        if (!Opened) {
            Say(Opened.error().Describe("mandelbrot"));
            return 1;
        }
        Look.Screen = std::move(*Opened);
        Look.Begin(Words, Count);

        std::uint64_t Began = clock_uptime_us(), Last = Began;
        bool          Painted = true;
        for (;;) {
            std::uint64_t Now = clock_uptime_us();
            Look.Step(static_cast<float>(Now - Last) / 1e6f);
            Last = Now;
            Look.Draw(static_cast<float>(Now - Began) / 1e6f);
            if (!(Painted = static_cast<bool>(Look.Screen->Present()))) break;

            const painter::tui::Event Event = Look.Screen->Wait(Look.Wait());
            if (Event.Kind == EventKind::Gone) break;
            if (Event.Kind == EventKind::Key && !Look.OnKey(Event.Key)) break;
        }
        double Deepest = Look.HomeWidth / Look.Nearest;
        (void)Look.Screen->Close();
        Say(Painted ? std::format("[MANDELBROT] looked {:.3g} times nearer than the whole set", Deepest)
                    : "The screen stopped taking frames.");
        return Painted ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a picture the size of this screen.");
        return 1;
    }
}
