/* eyes — a pair of eyes in a corner of the screen, looking about.
 *
 *   eyes                    in the bottom right corner
 *   eyes top left           top, bottom, left, right, middle, in any pair
 *   q, ESC                  leave; the eyes close and are gone
 *
 * There is no pointer to follow, so they look where they please: a glance
 * and a while held there, now and then a look round, a roll of the eyes, a
 * squint at the nose; they blink every few seconds and once in a while one
 * winks. Each run its irises take a colour of their own.
 *
 * The eyes are painted over the console where they stand, not over the whole
 * glass: half-block pixels, square, two to a cell. While they move they are
 * painted thirty times a second; still, four times — enough to come back at
 * once over whatever was written on them. */

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/print.h"

#include "painter/core/color.h"
#include "painter/tui/glyph.h"
#include "painter/tui/keyboard.h"
#include "painter/tui/surface.h"

BOX_FORM("takes optional words \"where they stand: top, bottom, left, right or middle; bottom right if none\"\n");

namespace {

using PainterColor = painter::core::Color;

constexpr std::uint32_t MovingMs = 33;
constexpr std::uint32_t StillMs  = 250;
constexpr float         Pi       = 3.14159265f;

/* Eye colours, light on the black of the console: every one keeps its family
 * on a sixteen-colour console too. */
const PainterColor Sclera    = PainterColor::Rgb(0xF4, 0xF4, 0xF4);
const PainterColor ScleraRim = PainterColor::Rgb(0xB8, 0xB8, 0xC0);
const PainterColor Pupil     = PainterColor::Rgb(0x0E, 0x0E, 0x10);
const PainterColor Glint     = painter::core::Colors::White;
const PainterColor Lid       = PainterColor::Rgb(0x3C, 0x3C, 0x44);
const PainterColor LidEdge   = PainterColor::Rgb(0x18, 0x18, 0x1C);
const PainterColor Empty     = painter::core::Colors::Black;
const PainterColor Irises[]  = {PainterColor::Rgb(0x2E, 0x6C, 0xD0), PainterColor::Rgb(0x2E, 0x9A, 0x5A), PainterColor::Rgb(0x8A, 0x52, 0x22),
                         PainterColor::Rgb(0x9A, 0x80, 0x30), PainterColor::Rgb(0x60, 0x78, 0x90), PainterColor::Rgb(0x8A, 0x40, 0xB0)};

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

std::uint64_t Seed = 1;

float Unit()
{
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    return static_cast<float>(Seed >> 40) / static_cast<float>(1ull << 24);
}

float Between(float Low, float High)
{
    return Low + (High - Low) * Unit();
}

/* Where an eye looks, -1..1 each way, and how shut it is, 0..1. */
struct Look {
    float X = 0, Y = 0;
    float Shut = 0;
};

/* What the pair is doing: a glance held, a look round, a roll, a squint. */
enum class Act { Hold, Round, Roll, Squint };

class Pair {
public:
    void Begin();
    /* Time passing; true while anything moves. */
    bool Step(float Seconds);
    const Look &Eye(int Which) const { return Now[Which]; }

    int   Blinks = 0;
    PainterColor Iris   = Irises[0];

private:
    void Next();
    float BlinkShut(float Since, float Length) const;

    Look  Now[2];
    float GoalX = 0, GoalY = 0;
    Act   Doing = Act::Hold;
    float Left  = 1.0f;     /* seconds left of what is being done */
    float Spent = 0;
    float ToBlink = 3.0f;
    float Blinking = -1;    /* seconds into a blink, < 0 when none */
    int   Winking  = -1;    /* the eye that winks, -1 for both */
};

void Pair::Begin()
{
    Seed = cpu_rdtsc() | 1;
    Iris = Irises[static_cast<std::size_t>(Unit() * (sizeof(Irises) / sizeof(Irises[0])))];
    Next();
}

/* The next thing to do: mostly a glance somewhere and a while held there. */
void Pair::Next()
{
    float Pick = Unit();
    Spent = 0;
    if (Pick < 0.08f) {
        Doing = Act::Round;
        Left  = 2.4f;
    } else if (Pick < 0.12f) {
        Doing = Act::Roll;
        Left  = 1.4f;
    } else if (Pick < 0.15f) {
        Doing = Act::Squint;
        Left  = 1.2f;
    } else {
        Doing = Act::Hold;
        Left  = Between(0.6f, 3.0f);
        float Turn = Between(0, 2 * Pi), Far = std::sqrt(Unit());
        GoalX = std::cos(Turn) * Far;
        GoalY = std::sin(Turn) * Far * 0.8f;
    }
}

/* How shut a blink is `Since` seconds in: down fast, up a little slower. */
float Pair::BlinkShut(float Since, float Length) const
{
    float Down = Length * 0.4f;
    if (Since < Down) return Since / Down;
    return std::max(0.0f, 1.0f - (Since - Down) / (Length - Down));
}

bool Pair::Step(float Seconds)
{
    Left  -= Seconds;
    Spent += Seconds;
    if (Left <= 0) Next();

    float Goal[2][2];
    switch (Doing) {
    case Act::Round: {
        float Phase = Spent / 2.4f;
        float X     = Phase < 0.33f ? -0.9f : Phase < 0.66f ? 0.9f : 0.0f;
        Goal[0][0] = Goal[1][0] = X;
        Goal[0][1] = Goal[1][1] = 0.1f;
        break;
    }
    case Act::Roll: {
        float Turn = Spent / 1.4f * 2 * Pi - Pi / 2;
        Goal[0][0] = Goal[1][0] = std::cos(Turn) * 0.9f;
        Goal[0][1] = Goal[1][1] = std::sin(Turn) * 0.8f;
        break;
    }
    case Act::Squint:
        Goal[0][0] = 0.85f;
        Goal[1][0] = -0.85f;
        Goal[0][1] = Goal[1][1] = 0.35f;
        break;
    default:
        Goal[0][0] = Goal[1][0] = GoalX;
        Goal[0][1] = Goal[1][1] = GoalY;
        break;
    }

    /* Eyes jump to where they look and settle: a saccade, not a slide. */
    bool  Moving = false;
    float Pull   = std::min(1.0f, Seconds * (Doing == Act::Roll ? 20.0f : 14.0f));
    for (int Which = 0; Which < 2; Which++) {
        float Dx = Goal[Which][0] - Now[Which].X, Dy = Goal[Which][1] - Now[Which].Y;
        if (Dx * Dx + Dy * Dy > 1e-4f) Moving = true;
        Now[Which].X += Dx * Pull;
        Now[Which].Y += Dy * Pull;
    }

    constexpr float BlinkLength = 0.18f;
    ToBlink -= Seconds;
    if (Blinking < 0 && ToBlink <= 0) {
        Blinking = 0;
        Winking  = Unit() < 0.08f ? static_cast<int>(Unit() * 2) : -1;
        ToBlink  = Between(2.0f, 6.5f);
        Blinks++;
    }
    for (Look &One : Now) One.Shut = 0;
    if (Blinking >= 0) {
        float Length = Winking >= 0 ? BlinkLength * 2.2f : BlinkLength;
        float Shut   = BlinkShut(Blinking, Length);
        for (int Which = 0; Which < 2; Which++)
            if (Winking < 0 || Winking == Which) Now[Which].Shut = Shut;
        Blinking += Seconds;
        if (Blinking >= Length) Blinking = -1;
        Moving = true;
    }
    return Moving;
}

/* The pixels of the pair, each eye an ellipse: the white shaded toward its
 * rim, the iris with a darker ring, the pupil, a glint, and the lids. */
class EyePicture {
public:
    void Size(int Scale)
    {
        EyeW   = Scale > 1 ? 10 * Scale : 12;   /* a small eye is a little bigger, or its pupil is a line */
        EyeH   = Scale > 1 ? 12 * Scale : 14;
        Gap    = 2 * Scale;
        Width  = EyeW * 2 + Gap;
        Height = EyeH;
        Dots.assign(static_cast<std::size_t>(Width) * Height, Empty);
    }

    void Draw(const Pair &Eyes)
    {
        std::fill(Dots.begin(), Dots.end(), Empty);
        for (int Which = 0; Which < 2; Which++) DrawEye(Which * (EyeW + Gap), Eyes.Eye(Which), Eyes.Iris);
    }

    PainterColor At(int X, int Y) const
    {
        return Y < Height ? Dots[static_cast<std::size_t>(Y) * Width + X] : Empty;
    }

    int Width = 0, Height = 0;

private:
    void DrawEye(int Left, const Look &Eye, PainterColor Iris);

    int                EyeW = 10, EyeH = 12, Gap = 2;
    std::vector<PainterColor> Dots;
};

void EyePicture::DrawEye(int Left, const Look &Eye, PainterColor Iris)
{
    float Rx = EyeW / 2.0f, Ry = EyeH / 2.0f;
    float Cx = Left + Rx, Cy = Ry;
    /* A shade under round numbers, so no lone pixel sticks out at the four points of a circle. */
    float Ri = Rx * 0.5f - 0.3f, Rp = std::max(1.5f, Ri * 0.5f);
    float Ix = Cx + Eye.X * (Rx - Ri - 0.5f), Iy = Cy + Eye.Y * (Ry - Ri - 0.5f);
    float LidTop = Cy - Ry + Eye.Shut * Ry * 1.1f, LidLow = Cy + Ry - Eye.Shut * Ry * 0.9f;

    for (int Y = 0; Y < EyeH; Y++) {
        for (int X = Left; X < Left + EyeW; X++) {
            float Px = X + 0.5f, Py = Y + 0.5f;
            float Ex = (Px - Cx) / Rx, Ey = (Py - Cy) / Ry, Edge = Ex * Ex + Ey * Ey;
            if (Edge > 1.0f) continue;
            PainterColor Ink;
            if (Py < LidTop || Py > LidLow) {
                bool Rim = Py >= LidTop - 1.0f && Py < LidTop;
                Ink = Eye.Shut > 0.02f && Rim ? LidEdge : Lid;
            } else {
                float Dx = Px - Ix, Dy = Py - Iy, Far = std::sqrt(Dx * Dx + Dy * Dy);
                if (Far <= Rp) Ink = Pupil;
                else if (Far <= Ri) Ink = Far > Ri - 1.0f ? PainterColor::Mix(Iris, painter::core::Colors::Black, 110) : Iris;
                else Ink = PainterColor::Mix(Sclera, ScleraRim, static_cast<std::uint8_t>(std::clamp(Edge * Edge * 255.0f, 0.0f, 255.0f)));
                float Gx = Px - (Ix - Rp * 0.6f), Gy = Py - (Iy - Rp * 0.6f);
                if (Far <= Ri && Gx * Gx + Gy * Gy <= std::max(0.6f, Rp * Rp * 0.18f)) Ink = Glint;
            }
            Dots[static_cast<std::size_t>(Y) * Width + X] = Ink;
        }
    }
}

/* Where the pair stands on the glass, in cells. */
struct Place {
    int Row = 0, Col = 0;
};

Place Where(const char *const *Words, std::size_t Count, int Cols, int Rows, int Wide, int High)
{
    int Across = 2, Down = 2;   /* 0 left/top, 1 middle, 2 right/bottom */
    for (std::size_t At = 0; At < Count; At++) {
        std::string_view Word = Words[At];
        if (Word == "left") Across = 0;
        if (Word == "right") Across = 2;
        if (Word == "top") Down = 0;
        if (Word == "bottom") Down = 2;
        if (Word == "middle" || Word == "centre" || Word == "center") {
            bool Said = false;
            for (std::size_t Other = 0; Other < Count; Other++) {
                std::string_view Two = Words[Other];
                Said = Said || Two == "left" || Two == "right" || Two == "top" || Two == "bottom";
            }
            if (!Said) Across = Down = 1;
        }
    }
    if (Count == 1 && (std::string_view(Words[0]) == "top" || std::string_view(Words[0]) == "bottom")) Across = 1;
    if (Count == 1 && (std::string_view(Words[0]) == "left" || std::string_view(Words[0]) == "right")) Down = 1;

    Place At;
    At.Col = Across == 0 ? 2 : Across == 1 ? (Cols - Wide) / 2 : Cols - Wide - 2;
    At.Row = Down == 0 ? 2 : Down == 1 ? (Rows - High) / 2 : Rows - High - 1;   /* row 0 is the clock's */
    return At;
}

/* Replace the entire retained footprint at `At`. Transparent cells reveal the
 * base surface, so a moving eye never leaves its previous frame behind. */
box::status Paint(const EyePicture &Picture, painter::tui::Surface &Screen,
                  painter::tui::Frame &Frame, Place At)
{
    if (auto Cleared = Screen.Clear(); !Cleared) return Cleared;
    Frame.ClearTransparent();
    int Rows = (Picture.Height + 1) / 2;
    for (int Row = 0; Row < Rows; Row++) {
        for (int X = 0; X < Picture.Width; X++) {
            PainterColor Top = X < Picture.Width ? Picture.At(X, Row * 2) : Empty;
            PainterColor Low = X < Picture.Width ? Picture.At(X, Row * 2 + 1) : Empty;
            bool  Inked = X < Picture.Width && (Top != Empty || Low != Empty);
            if (Inked)
                Frame.Put({X, Row}, painter::tui::Cell{painter::tui::Glyphs::UpperHalf, Top, Low});
        }
    }
    return Screen.Composite({At.Col, At.Row}, Frame);
}

}

int main()
{
    const char *const *Words;
    std::size_t        Count;
    if (form_words(0, &Words, &Count) != 0) return 1;

    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open("eyes", 30, 10);
    if (!Opened) {
        Say(Opened.error().Describe("eyes"));
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    painter::core::Size Dimensions = Screen.Size();
    EyePicture Picture;
    Picture.Size(Dimensions.Height >= 40 && Dimensions.Width >= 120 ? 2 : 1);
    Place At = Where(Words, Count, Dimensions.Width, Dimensions.Height, Picture.Width, (Picture.Height + 1) / 2);

    std::expected<painter::tui::Keyboard, painter::tui::StageError> Input = painter::tui::Keyboard::Open("eyes");
    if (!Input) {
        Say(Input.error().Describe("eyes"));
        return 1;
    }
    painter::tui::Keyboard Keys = std::move(*Input);
    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({Picture.Width, (Picture.Height + 1) / 2});
    if (!Created) {
        (void)Keys.Close();
        return 1;
    }
    painter::tui::Frame Overlay = std::move(*Created);
    Pair Eyes;
    Eyes.Begin();

    std::uint64_t Last = clock_uptime_us();
    for (;;) {
        std::uint64_t Now    = clock_uptime_us();
        bool          Moving = Eyes.Step(std::min(0.5f, static_cast<float>(Now - Last) / 1e6f));
        Last = Now;
        Picture.Draw(Eyes);
        if (!Paint(Picture, Screen, Overlay, At)) break;

        painter::tui::Event Event = Keys.Wait(Moving ? MovingMs : StillMs);
        if (Event.Kind == painter::tui::EventKind::Gone) break;
        if (Event.Kind == painter::tui::EventKind::Key && !Event.Key.IsExtended() &&
            (Event.Key.Ascii == 'q' || Event.Key.Ascii == 'Q' ||
             Event.Key.Ascii == painter::tui::KeyCodes::Escape))
            break;
    }
    (void)Keys.Close();
    (void)Screen.Clear();
    (void)Screen.Close();
    Say(std::format("[EYES] {} blink{}", Eyes.Blinks, Eyes.Blinks == 1 ? "" : "s"));
    return 0;
}
