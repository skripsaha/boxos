/* crab — a crab on the glass, living on its own.
 *
 *   crab                    it walks out and goes about its day
 *   finish crab             it is gone
 *
 * It lives on by itself, as eyes and the clock do (box/life.h stays): whoever
 * starts it gets the prompt back at once, and it wanders over the screen as a
 * notice, covering only its own cells and nothing beneath it elsewhere. It
 * takes no keys.
 *
 * It walks sideways on six legs, one set and then the other, eyes on stalks
 * turned the way it goes. Between walks it rests, waves a claw, snaps both,
 * digs in with a spray of sand, or blows bubbles that rise and pop. Its
 * shell, claws and legs are shaded pixel art in a ramp of five tones from
 * one colour (settings crab color), drawn at whatever size the glass and
 * settings crab size give. Painted while it moves twenty times a second,
 * at rest ten. */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/life.h"
#include "box/print.h"
#include "box/settings.h"
#include "box/cxx/settings.h"

#include "painter/core/color.h"
#include "painter/tui/glyph.h"
#include "painter/tui/surface.h"

BOX_FORM("");
BOX_LIFE("stays");
BOX_SETTINGS("speed number from 20 to 300 percent default 100 \"how fast the crab walks and works\"\n"
             "size number from 40 to 250 percent default 100 \"how large the crab is\"\n"
             "color color default #D8453B \"the crab's shell\"\n"
             "shadow on-or-off default on \"a dark shadow on the ground under the crab\"\n");

namespace {

using PainterColor = painter::core::Color;

constexpr std::uint32_t ActiveMs = 50;
constexpr std::uint32_t RestMs   = 100;
constexpr float         Pi       = 3.14159265f;

/* The sprite is drawn in units: 56 across, 44 down, the ground at 41.5. */
constexpr float SpriteWide = 56.0f;
constexpr float SpriteHigh = 44.0f;
constexpr float GroundY    = 41.5f;
constexpr float MiddleX    = 28.0f;
constexpr int   FirstRow   = 3;   /* the clock and its load are above */

const PainterColor Empty     = PainterColor::Through();
const PainterColor Sclera    = PainterColor::Rgb(0xFF, 0xF4, 0xD6);
const PainterColor Pupil     = PainterColor::Rgb(0x14, 0x1A, 0x30);
const PainterColor Glint     = painter::core::Colors::White;
const PainterColor Shadow    = PainterColor::Rgb(0x24, 0x26, 0x30);
const PainterColor SandLight = PainterColor::Rgb(0xE6, 0xCC, 0x92);
const PainterColor SandDark  = PainterColor::Rgb(0xB8, 0x9A, 0x60);
const PainterColor BubbleRim = PainterColor::Rgb(0xBF, 0xE6, 0xF2);

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

void Ease(float &Value, float Goal, float Rate, float Seconds)
{
    Value += (Goal - Value) * std::min(1.0f, Rate * Seconds);
}

struct Spot {
    float X = 0, Y = 0;
};

Spot Lerp(Spot From, Spot To, float Part)
{
    return {From.X + (To.X - From.X) * Part, From.Y + (To.Y - From.Y) * Part};
}

/* What the person set, read again whenever the file is saved. */
struct Look {
    float        Speed = 1.0f;
    float        Size  = 1.0f;
    PainterColor Shell = PainterColor::Rgb(0xD8, 0x45, 0x3B);
    bool         Shadowed = true;
};

Look ReadLook()
{
    using box::settings;
    return Look{.Speed = static_cast<float>(settings::number("speed")) / 100.0f,
                .Size  = static_cast<float>(settings::number("size")) / 100.0f,
                .Shell = PainterColor::FromRaw(settings::color("color")),
                .Shadowed = settings::on("shadow")};
}

/* Five tones from shadow to highlight, and the line around a shape. */
struct Ramp {
    PainterColor Tone[5];
    PainterColor Outline;
};

Ramp MakeRamp(PainterColor Base)
{
    const PainterColor Deep = PainterColor::Rgb(0x14, 0x06, 0x18);
    const PainterColor Pale = PainterColor::Rgb(0xFF, 0xF2, 0xE0);
    return Ramp{{PainterColor::Mix(Base, Deep, 150), PainterColor::Mix(Base, Deep, 70), Base,
                 PainterColor::Mix(Base, Pale, 70), PainterColor::Mix(Base, Pale, 150)},
                PainterColor::Mix(Base, Deep, 205)};
}

Ramp Flat(PainterColor Ink)
{
    return Ramp{{Ink, Ink, Ink, Ink, Ink}, Ink};
}

/* The pixels of the sprite. Shapes are given in units and drawn as lit
 * solids: the light comes from the upper left, the tone is the nearest of the
 * ramp's five, and every shape has a line of one pixel around it. */
class Pixmap {
public:
    void Size(float Zoom)
    {
        Scale  = Zoom;
        Width  = static_cast<int>(std::ceil(SpriteWide * Zoom));
        Height = static_cast<int>(std::ceil(SpriteHigh * Zoom)) + 2;
        Height += Height & 1;
        Dots.assign(static_cast<std::size_t>(Width) * Height, Empty);
    }

    void Clear(int Rows)
    {
        std::fill(Dots.begin(), Dots.end(), Empty);
        Shift = Rows;
    }

    PainterColor At(int X, int Y) const { return Dots[static_cast<std::size_t>(Y) * Width + X]; }

    void Oval(float Cx, float Cy, float Rx, float Ry, const Ramp &Ink);
    void Round(Spot From, Spot To, float RadiusFrom, float RadiusTo, const Ramp &Ink);
    void Ring(float Cx, float Cy, float Radius, PainterColor Ink);
    void Dot(float X, float Y, PainterColor Ink) { Put(Whole(X * Scale), Whole(Y * Scale), Ink); }

    int Width = 0, Height = 0;

private:
    static int Whole(float Value) { return static_cast<int>(std::floor(Value)); }

    void Put(int X, int Y, PainterColor Ink)
    {
        Y += Shift;
        if (X >= 0 && X < Width && Y >= 0 && Y < Height) Dots[static_cast<std::size_t>(Y) * Width + X] = Ink;
    }

    void Plot(int X, int Y, float Beyond, float Nx, float Ny, const Ramp &Ink);

    std::vector<PainterColor> Dots;
    float                     Scale = 1.0f;
    int                       Shift = 0;
};

void Pixmap::Plot(int X, int Y, float Beyond, float Nx, float Ny, const Ramp &Ink)
{
    if (Beyond > 1.0f) return;
    if (Beyond > 0.0f) {
        Put(X, Y, Ink.Outline);
        return;
    }
    float Flat = std::min(1.0f, Nx * Nx + Ny * Ny);
    float Lit  = std::clamp(-0.5f * Nx - 0.6f * Ny + 0.62f * std::sqrt(1.0f - Flat), 0.0f, 1.0f);
    int   Band = Lit < 0.25f ? 0 : Lit < 0.5f ? 1 : Lit < 0.75f ? 2 : Lit < 0.9f ? 3 : 4;
    Put(X, Y, Ink.Tone[Band]);
}

void Pixmap::Oval(float Cx, float Cy, float Rx, float Ry, const Ramp &Ink)
{
    Cx *= Scale;
    Cy *= Scale;
    Rx *= Scale;
    Ry *= Scale;
    for (int Y = Whole(Cy - Ry - 1); Y <= Whole(Cy + Ry + 1); Y++) {
        for (int X = Whole(Cx - Rx - 1); X <= Whole(Cx + Rx + 1); X++) {
            float Ex = (X + 0.5f - Cx) / Rx, Ey = (Y + 0.5f - Cy) / Ry;
            float Far = std::sqrt(Ex * Ex + Ey * Ey);
            Plot(X, Y, (Far - 1.0f) * std::min(Rx, Ry), Ex, Ey, Ink);
        }
    }
}

/* A rod from one end to the other, thicker at one: a limb, a finger. */
void Pixmap::Round(Spot From, Spot To, float RadiusFrom, float RadiusTo, const Ramp &Ink)
{
    float Ax = From.X * Scale, Ay = From.Y * Scale, Bx = To.X * Scale, By = To.Y * Scale;
    float Ra = std::max(0.8f, RadiusFrom * Scale), Rb = std::max(0.8f, RadiusTo * Scale);
    float Dx = Bx - Ax, Dy = By - Ay, Long = Dx * Dx + Dy * Dy, Reach = std::max(Ra, Rb) + 1.0f;
    for (int Y = Whole(std::min(Ay, By) - Reach); Y <= Whole(std::max(Ay, By) + Reach); Y++) {
        for (int X = Whole(std::min(Ax, Bx) - Reach); X <= Whole(std::max(Ax, Bx) + Reach); X++) {
            float Px = X + 0.5f - Ax, Py = Y + 0.5f - Ay;
            float Along = Long > 0 ? std::clamp((Px * Dx + Py * Dy) / Long, 0.0f, 1.0f) : 0.0f;
            float Ox = Px - Dx * Along, Oy = Py - Dy * Along;
            float Radius = Ra + (Rb - Ra) * Along, Far = std::sqrt(Ox * Ox + Oy * Oy);
            Plot(X, Y, Far - Radius, Ox / Radius, Oy / Radius, Ink);
        }
    }
}

void Pixmap::Ring(float Cx, float Cy, float Radius, PainterColor Ink)
{
    Cx *= Scale;
    Cy *= Scale;
    Radius = std::max(1.2f, Radius * Scale);
    for (int Y = Whole(Cy - Radius - 1); Y <= Whole(Cy + Radius + 1); Y++) {
        for (int X = Whole(Cx - Radius - 1); X <= Whole(Cx + Radius + 1); X++) {
            float Far = std::sqrt((X + 0.5f - Cx) * (X + 0.5f - Cx) + (Y + 0.5f - Cy) * (Y + 0.5f - Cy));
            if (std::fabs(Far - Radius) < 0.6f) Put(X, Y, Ink);
        }
    }
}

/* How the crab stands and moves, as the picture takes it. */
struct Pose {
    float Bob      = 0;       /* the body's drop, units */
    float Stalk    = 1;       /* 1 eyes held high, 0 drawn in */
    float Walk     = 0;       /* the legs' phase */
    float Walking  = 0;       /* how far the legs reach, 0..1 */
    float Raise[2] = {0, 0};  /* each claw lifted, 0..1; [0] left, [1] right */
    float Open[2]  = {0, 0};  /* each claw's jaws, 0 shut, 1 wide */
    float Wave     = 0;
    float LookX = 0, LookY = 0;
    float Blink    = 0;       /* 0 open, 1 shut */
    bool  Mouth    = false;   /* round, as it blows */
};

/* A grain of sand flung up, or a bubble rising. */
struct Speck {
    Spot  At, Move;
    float Age = 0, Life = 1, Size = 1;
    bool  Bubble = false;
};

constexpr float Gravity = 70.0f;

enum class Act { Rest, Walk, Wave, Dig, Blow, Snap };

class Crab {
public:
    void Begin(int Cols, int Rows, int Wide, int High);
    /* Time passing; true while the crab does something that moves. */
    bool Step(float Seconds);
    void Fit(int Cols, int Rows, int Wide, int High);
    void Tune(const Look &Set) { Pace = Set.Speed; }

    Spot                     Where;   /* the upper left, in cells */
    Pose                     Now;
    std::vector<Speck>       Specks;

private:
    void  Next();
    void  Choose();
    void  Move(float Seconds);
    void  Behave(float Seconds);
    void  Blinks(float Seconds);
    void  Throw(float Seconds);
    void  Fly(float Seconds);
    float Reach(float Low, float High) const;

    int   Cols = 0, Rows = 0, Wide = 0, High = 0;
    Spot  Goal;
    Act   Doing = Act::Rest;
    float Left = 1.0f, Spent = 0, Pace = 1.0f;
    float ToBlink = 2.0f, Blinking = -1, ToThrow = 0;
};

void Crab::Begin(int ColsNow, int RowsNow, int WideNow, int HighNow)
{
    Seed = cpu_rdtsc() | 1;
    Fit(ColsNow, RowsNow, WideNow, HighNow);
    Where = {Reach(0, static_cast<float>(Cols - Wide)), Reach(FirstRow, static_cast<float>(Rows - High))};
    Goal  = Where;
    Next();
}

void Crab::Fit(int ColsNow, int RowsNow, int WideNow, int HighNow)
{
    Cols = ColsNow;
    Rows = RowsNow;
    Wide = WideNow;
    High = HighNow;
    Where.X = std::clamp(Where.X, 0.0f, static_cast<float>(std::max(0, Cols - Wide)));
    Where.Y = std::clamp(Where.Y, static_cast<float>(FirstRow), static_cast<float>(std::max(FirstRow, Rows - High)));
}

float Crab::Reach(float Low, float High) const
{
    return Between(Low, std::max(Low, High));
}

/* After a rest the crab usually walks; now and then it does something else. */
void Crab::Next()
{
    float Pick = Unit();
    Spent = 0;
    if (Doing != Act::Rest) {
        Doing = Act::Rest;
        Left  = Between(0.8f, 2.6f);
        return;
    }
    if (Pick < 0.52f) {
        Doing = Act::Walk;
        Choose();
    } else if (Pick < 0.66f) {
        Doing = Act::Wave;
        Left  = 2.2f;
    } else if (Pick < 0.78f) {
        Doing = Act::Dig;
        Left  = 2.6f;
    } else if (Pick < 0.90f) {
        Doing = Act::Blow;
        Left  = 3.0f;
        ToThrow = 0.3f;
    } else {
        Doing = Act::Snap;
        Left  = 1.6f;
    }
}

/* A place to walk to: along the way for the most part, a little up or down. */
void Crab::Choose()
{
    float Far = Unit() < 0.2f ? 8.0f : 3.0f;
    Goal.X = std::clamp(Where.X + (Unit() < 0.5f ? -1.0f : 1.0f) * Between(10.0f, static_cast<float>(Cols) / 3.0f),
                        0.0f, static_cast<float>(std::max(0, Cols - Wide)));
    Goal.Y = std::clamp(Where.Y + Between(-Far, Far), static_cast<float>(FirstRow),
                        static_cast<float>(std::max(FirstRow, Rows - High)));
    Left = 60.0f;
}

void Crab::Move(float Seconds)
{
    float Dx = Goal.X - Where.X, Dy = (Goal.Y - Where.Y) * 2.0f;
    float Far = std::hypot(Dx, Dy);
    if (Far < 0.6f) {
        Left = 0;
        return;
    }
    float Stride = std::min(Far, 13.0f * Pace * Seconds);
    Where.X += Dx / Far * Stride;
    Where.Y += Dy / Far * Stride / 2.0f;
    Now.Walk += Stride * 0.5f;
    Ease(Now.LookX, Dx > 0 ? 1.0f : -1.0f, 6.0f, Seconds);
    Ease(Now.LookY, 0.0f, 6.0f, Seconds);
}

/* The poses of each doing, eased toward from wherever the last left them. */
void Crab::Behave(float Seconds)
{
    float Raise[2] = {0, 0}, Open[2] = {0, 0}, Bob = 0, Stalk = 1, Walking = 0;
    bool  Mouth = false;
    switch (Doing) {
    case Act::Walk:
        Walking = 1.0f;
        Raise[0] = 0.1f + 0.08f * std::sin(Now.Walk * 2.0f);
        Raise[1] = 0.1f + 0.08f * std::sin(Now.Walk * 2.0f + Pi);
        Move(Seconds);
        break;
    case Act::Wave:
        Raise[1] = 1.0f;
        Open[1]  = 0.45f + 0.45f * std::sin(Spent * 7.0f);
        Now.Wave += Seconds * 8.0f * Pace;
        Ease(Now.LookX, 0.0f, 5.0f, Seconds);
        Ease(Now.LookY, -0.3f, 5.0f, Seconds);
        break;
    case Act::Dig:
        Bob = 2.4f;
        Stalk = 0.45f;
        Walking = 0.55f;
        Now.Walk += Seconds * 11.0f * Pace;
        for (int Side = 0; Side < 2; Side++) {
            Raise[Side] = 0.45f * std::max(0.0f, std::sin(Spent * 7.0f * Pace + Side * Pi));
            Open[Side]  = 0.8f;
        }
        Ease(Now.LookY, 0.5f, 5.0f, Seconds);
        break;
    case Act::Blow:
        Mouth = true;
        Ease(Now.LookX, 0.0f, 5.0f, Seconds);
        Ease(Now.LookY, -0.5f, 5.0f, Seconds);
        break;
    case Act::Snap: {
        float Clap = std::sin(Spent * 19.0f * Pace) > 0.2f ? 1.0f : 0.0f;
        Raise[0] = Raise[1] = 0.55f;
        Open[0]  = Clap;
        Open[1]  = 1.0f - Clap;
        Ease(Now.LookX, 0.0f, 5.0f, Seconds);
        break;
    }
    default:
        Open[0] = Open[1] = 0;
        Ease(Now.LookX, 0.8f * std::sin(Spent * 0.9f + Where.X), 2.5f, Seconds);
        Ease(Now.LookY, 0.2f * std::sin(Spent * 0.6f), 2.5f, Seconds);
        break;
    }
    for (int Side = 0; Side < 2; Side++) {
        Ease(Now.Raise[Side], Raise[Side], 10.0f, Seconds);
        Ease(Now.Open[Side], Open[Side], 22.0f, Seconds);
    }
    Ease(Now.Bob, Bob + 0.35f * std::sin(Spent * 2.4f + Left), 8.0f, Seconds);
    Ease(Now.Stalk, Stalk, 7.0f, Seconds);
    Ease(Now.Walking, Walking, 9.0f, Seconds);
    Now.Mouth = Mouth;
}

void Crab::Blinks(float Seconds)
{
    constexpr float Length = 0.2f;
    ToBlink -= Seconds;
    if (Blinking < 0 && ToBlink <= 0) {
        Blinking = 0;
        ToBlink  = Between(2.0f, 6.0f);
    }
    Now.Blink = 0;
    if (Blinking >= 0) {
        Now.Blink = std::sin(Blinking / Length * Pi);
        Blinking += Seconds;
        if (Blinking >= Length) Blinking = -1;
    }
}

/* Sand while it digs, bubbles while it blows. */
void Crab::Throw(float Seconds)
{
    ToThrow -= Seconds;
    if (ToThrow > 0) return;
    if (Doing == Act::Dig) {
        ToThrow = 0.05f / Pace;
        float Side = Unit() < 0.5f ? -1.0f : 1.0f;
        Specks.push_back({{MiddleX + Side * Between(15.0f, 19.0f), GroundY - 1.0f},
                          {Side * Between(5.0f, 13.0f), -Between(18.0f, 34.0f)}, 0, 1.4f, Between(0.0f, 1.0f), false});
    } else if (Doing == Act::Blow && Left > 0.4f) {
        ToThrow = 0.5f;
        Specks.push_back({{MiddleX, 36.0f + Now.Bob}, {Between(-2.0f, 2.0f), -Between(5.0f, 8.0f)}, 0,
                          Between(3.0f, 5.0f), Between(1.4f, 3.2f), true});
    } else {
        ToThrow = 0.2f;
    }
}

void Crab::Fly(float Seconds)
{
    for (Speck &One : Specks) {
        One.Age += Seconds;
        if (One.Bubble) {
            One.At.X += One.Move.X * Seconds + std::sin(One.Age * 3.0f + One.Size) * 4.0f * Seconds;
            One.At.Y += One.Move.Y * Seconds;
        } else {
            One.Move.Y += Gravity * Seconds;
            One.At.X += One.Move.X * Seconds;
            One.At.Y += One.Move.Y * Seconds;
            if (One.At.Y >= GroundY) One.Age = One.Life;
        }
    }
    std::erase_if(Specks, [](const Speck &One) { return One.Age >= One.Life || One.At.Y < 2.0f; });
}

bool Crab::Step(float Seconds)
{
    Left  -= Seconds;
    Spent += Seconds;
    if (Left <= 0) Next();
    Behave(Seconds);
    Blinks(Seconds);
    Throw(Seconds);
    Fly(Seconds);
    return Doing != Act::Rest || !Specks.empty() || Now.Blink > 0 || Blinking >= 0;
}

/* The crab's colours: the shell, and legs and stalks a shade darker. */
struct Palette {
    Ramp Shell, Leg, Eye;
};

Ramp LightEye()
{
    return Ramp{{PainterColor::Rgb(0xC4, 0xBA, 0xA8), PainterColor::Rgb(0xE6, 0xDC, 0xC8), Sclera, Sclera, Glint},
                PainterColor::Rgb(0x3A, 0x12, 0x18)};
}

Palette PaletteOf(PainterColor Shell)
{
    return Palette{MakeRamp(Shell), MakeRamp(PainterColor::Mix(Shell, PainterColor::Rgb(0x30, 0x0C, 0x14), 70)),
                   LightEye()};
}

void DrawLegs(Pixmap &Art, const Palette &Ink, const Pose &Now)
{
    for (int Side = -1; Side <= 1; Side += 2) {
        for (int Index = 0; Index < 3; Index++) {
            float Phase = Now.Walk + Index * 2.09f + (Side > 0 ? Pi : 0.0f);
            float Lift  = std::max(0.0f, std::sin(Phase)) * 2.6f * Now.Walking;
            float Sway  = std::cos(Phase) * 1.8f * Now.Walking;
            float RootX = MiddleX + Side * (7.5f + Index * 2.8f);
            Spot  Root{RootX, 37.0f};
            Spot  Knee{MiddleX + Side * (15.5f + Index * 1.6f) + Sway * 0.5f, 33.0f - Lift * 0.8f};
            Spot  Foot{MiddleX + Side * (18.0f + Index * 2.6f) + Sway, GroundY - Lift};
            Art.Round(Root, Knee, 1.7f, 1.4f, Ink.Leg);
            Art.Round(Knee, Foot, 1.4f, 0.9f, Ink.Leg);
        }
    }
}

Spot WristOf(const Pose &Now, int Side)
{
    int  Index = Side > 0 ? 1 : 0;
    Spot Wrist = Lerp({MiddleX + Side * 21.0f, 26.5f}, {MiddleX + Side * 22.0f, 15.5f}, Now.Raise[Index]);
    Wrist.X += Side * std::sin(Now.Wave) * 2.2f * (Index == 1 ? Now.Raise[1] : 0.0f);
    return Wrist;
}

void DrawArms(Pixmap &Art, const Palette &Ink, const Pose &Now)
{
    for (int Side = -1; Side <= 1; Side += 2) {
        float Up = Now.Raise[Side > 0 ? 1 : 0];
        Spot Shoulder{MiddleX + Side * 12.0f, 32.0f + Now.Bob};
        Spot Elbow = Lerp({MiddleX + Side * 18.0f, 34.0f}, {MiddleX + Side * 19.0f, 26.0f}, Up);
        Spot Wrist = WristOf(Now, Side);
        Art.Round(Shoulder, Elbow, 2.0f, 1.8f, Ink.Shell);
        Art.Round(Elbow, Wrist, 1.8f, 2.2f, Ink.Shell);
    }
}

/* A claw: a palm, and two fingers that part and meet at their tips. */
void DrawClaws(Pixmap &Art, const Palette &Ink, const Pose &Now)
{
    for (int Side = -1; Side <= 1; Side += 2) {
        int  Index = Side > 0 ? 1 : 0;
        Spot Wrist = WristOf(Now, Side);
        Art.Oval(Wrist.X, Wrist.Y, 4.8f, 4.0f, Ink.Shell);
        float Dx = Side * 0.3f, Dy = -0.954f;
        Spot  Base{Wrist.X + Dx * 2.6f, Wrist.Y + Dy * 2.6f};
        for (int Jaw = -1; Jaw <= 1; Jaw += 2) {
            float Turn = Jaw * (0.17f + 0.5f * Now.Open[Index]);
            float Cs = std::cos(Turn), Sn = std::sin(Turn);
            Spot  Tip{Base.X + (Dx * Cs - Dy * Sn) * 8.0f, Base.Y + (Dx * Sn + Dy * Cs) * 8.0f};
            Art.Round(Base, Tip, 2.5f, 0.8f, Ink.Shell);
        }
    }
}

void DrawEye(Pixmap &Art, const Palette &Ink, const Pose &Now, float Side)
{
    float BaseY = 27.0f + Now.Bob, TopY = BaseY - 4.0f - 6.0f * Now.Stalk;
    float TopX  = MiddleX + Side * 6.0f;
    Art.Round({MiddleX + Side * 5.2f, BaseY}, {TopX, TopY}, 1.1f, 1.1f, Ink.Leg);
    Art.Oval(TopX, TopY, 3.4f, 3.4f, Ink.Eye);
    float PupilX = TopX + Now.LookX * 1.3f, PupilY = TopY + Now.LookY * 1.3f;
    Art.Oval(PupilX, PupilY, 1.7f, 1.7f, Flat(Pupil));
    Art.Dot(PupilX - 0.7f, PupilY - 0.9f, Glint);
    if (Now.Blink > 0.05f) {
        for (float Down = 0; Down < Now.Blink * 6.8f; Down += 0.25f) {
            float Y = TopY - 3.4f + Down, Half = std::sqrt(std::max(0.0f, 3.4f * 3.4f - (Y - TopY) * (Y - TopY)));
            for (float Across = -Half; Across <= Half; Across += 0.25f) Art.Dot(TopX + Across, Y, Ink.Shell.Tone[2]);
        }
    }
}

void DrawBody(Pixmap &Art, const Palette &Ink, const Pose &Now)
{
    float Top = 33.0f + Now.Bob;
    Art.Oval(MiddleX, Top, 13.5f, 8.4f, Ink.Shell);
    Art.Oval(MiddleX - 5.5f, Top - 4.4f, 3.2f, 1.1f, Flat(Ink.Shell.Tone[4]));
    for (Spot Spots : {Spot{-2.0f, -6.0f}, Spot{3.5f, -6.4f}, Spot{7.5f, -4.0f}, Spot{-8.5f, -1.5f}, Spot{9.5f, 0.5f}})
        Art.Oval(MiddleX + Spots.X, Top + Spots.Y, 0.9f, 0.9f, Flat(Ink.Shell.Tone[3]));
    if (Now.Mouth) {
        Art.Oval(MiddleX, Top + 3.4f, 1.3f, 1.1f, Flat(Ink.Shell.Outline));
        return;
    }
    for (float Across = -3.0f; Across <= 3.0f; Across += 0.5f)
        Art.Dot(MiddleX + Across, Top + 3.0f + 1.0f * (1.0f - Across * Across / 9.0f), Ink.Shell.Outline);
}

void DrawSpecks(Pixmap &Art, const std::vector<Speck> &Specks)
{
    for (const Speck &One : Specks) {
        if (!One.Bubble) {
            Art.Dot(One.At.X, One.At.Y, One.Size < 0.5f ? SandLight : SandDark);
            continue;
        }
        float Radius = One.Size * (0.5f + 0.5f * std::min(1.0f, One.Age * 1.5f));
        if (One.Age > One.Life - 0.3f) Radius *= 1.0f + (One.Age - One.Life + 0.3f);
        Art.Ring(One.At.X, One.At.Y, Radius, BubbleRim);
        Art.Dot(One.At.X - Radius * 0.4f, One.At.Y - Radius * 0.4f, Glint);
    }
}

void Draw(Pixmap &Art, const Palette &Ink, const Crab &Creature, int Shift, bool Shadowed)
{
    Art.Clear(Shift);
    const Pose &Now = Creature.Now;
    if (Shadowed) Art.Oval(MiddleX, GroundY + 0.4f, 17.0f, 2.2f, Flat(Shadow));
    DrawLegs(Art, Ink, Now);
    DrawArms(Art, Ink, Now);
    DrawEye(Art, Ink, Now, -1.0f);
    DrawEye(Art, Ink, Now, 1.0f);
    DrawBody(Art, Ink, Now);
    DrawClaws(Art, Ink, Now);
    DrawSpecks(Art, Creature.Specks);
}

/* Replace the retained footprint: transparent cells reveal the console, so
 * the crab leaves no earlier frame behind. */
box::status Paint(const Pixmap &Art, painter::tui::Surface &Screen, painter::tui::Frame &Frame, painter::core::Point At)
{
    if (auto Cleared = Screen.Clear(); !Cleared) return Cleared;
    Frame.ClearTransparent();
    int Rows = Frame.Size().Height;
    for (int Row = 0; Row < Rows; Row++) {
        for (int X = 0; X < Art.Width; X++) {
            PainterColor Top = Art.At(X, Row * 2), Low = Art.At(X, Row * 2 + 1);
            if (Top == Empty && Low == Empty) continue;
            Frame.Put({X, Row}, painter::tui::Cell{painter::tui::Glyphs::UpperHalf, Top, Low});
        }
    }
    return Screen.Composite(At, Frame);
}

float ZoomFor(int Cols, const Look &Set)
{
    return std::clamp(static_cast<float>(Cols) / (SpriteWide * 3.0f), 0.7f, 1.6f) * Set.Size;
}

}

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open(30, 10, painter::tui::SurfaceRole::Notice);
    if (!Opened) {
        Say(Opened.error().Describe("crab"));
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    painter::core::Size Dimensions = Screen.Size();

    Look      Set = ReadLook();
    Palette   Ink = PaletteOf(Set.Shell);
    Pixmap    Art;
    Art.Size(ZoomFor(Dimensions.Width, Set));
    std::expected<painter::tui::Frame, box::error> Created =
        painter::tui::Frame::Create({Art.Width, (Art.Height + 1) / 2});
    if (!Created) return 1;
    painter::tui::Frame Overlay = std::move(*Created);

    Crab Creature;
    Creature.Tune(Set);
    Creature.Begin(Dimensions.Width, Dimensions.Height, Art.Width, Overlay.Size().Height);
    box::settings::watch Watch;

    // Painted until it can be no more — the display gone, or the crab
    // ended: nothing but its end stops it.
    std::uint64_t Last = clock_uptime_us();
    for (;;) {
        if (Watch.changed()) {
            Set = ReadLook();
            Ink = PaletteOf(Set.Shell);
            Creature.Tune(Set);
            Art.Size(ZoomFor(Dimensions.Width, Set));
            std::expected<painter::tui::Frame, box::error> Resized =
                painter::tui::Frame::Create({Art.Width, (Art.Height + 1) / 2});
            if (!Resized) break;
            Overlay = std::move(*Resized);
            Creature.Fit(Dimensions.Width, Dimensions.Height, Art.Width, Overlay.Size().Height);
        }
        std::uint64_t Now    = clock_uptime_us();
        bool          Moving = Creature.Step(std::min(0.5f, static_cast<float>(Now - Last) / 1e6f));
        Last = Now;
        int Top = static_cast<int>(std::lround(Creature.Where.Y * 2.0f));
        Draw(Art, Ink, Creature, Top & 1, Set.Shadowed);
        if (!Paint(Art, Screen, Overlay, {static_cast<int>(std::lround(Creature.Where.X)), Top >> 1})) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(Moving ? ActiveMs : RestMs));
    }
    (void)Screen.Close();
    return 1;
}
