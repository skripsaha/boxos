/* slime — a lazy green slime on the glass, living on its own.
 *
 *   slime                   it wakes where it is and begins to hop about
 *   finish slime            it is gone
 *
 * It lives on by itself, as eyes and the clock do (box/life.h stays): whoever
 * starts it gets the prompt back at once, and it hops over the screen as a
 * notice, covering only its own cells and nothing beneath it elsewhere. It
 * takes no keys.
 *
 * A hop is a crouch, a launch that stretches it, an arc in the air and a
 * landing that squashes it flat and sets it wobbling, the body keeping its
 * volume throughout. Between hops it rests, breathes, blinks and looks about,
 * and now and then sleeps. Where it lands it leaves a wet slick that dries
 * to nothing. The body is jelly in pixel art: lit from the upper left, a
 * gloss on the crown, a darker heart and bubbles inside it, a glow where the
 * light comes through the bottom.
 *
 * It is painted over the console as half-block pixels, only in the box that
 * holds the slime and its slicks, twenty times a second while anything
 * moves, eight at rest. */

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
#include "box/cxx/timing.h"

#include "painter/core/color.h"
#include "painter/core/coverage.h"
#include "painter/tui/glyph.h"
#include "painter/tui/surface.h"

BOX_FORM("");
BOX_LIFE("stays");
BOX_SETTINGS("speed number from 20 to 300 percent default 100 \"how fast the slime hops and rests\"\n"
             "size number from 40 to 250 percent default 100 \"how large the slime is\"\n"
             "color color default #9FD724 \"the slime's body\"\n"
             "trail on-or-off default on \"a wet slick where the slime lands\"\n"
             "shadow on-or-off default on \"a dark shadow on the ground under the slime\"\n"
             "smooth on-or-off default off \"the slime's edge fades into what lies beneath; off keeps every edge pixel whole or none\"\n"
             "fps number from 5 to 60 frames default 20 \"how many frames a second the slime is drawn; more is smoother, the moving is as fast\"\n");

namespace {

using PainterColor = painter::core::Color;

constexpr std::chrono::microseconds RestInterval{125000};
constexpr float         Pi       = 3.14159265f;
constexpr int           FirstRow = 3;      /* the clock and its load are above */
constexpr float         SlickLife = 6.0f;  /* seconds a slick takes to dry */

const PainterColor Empty   = PainterColor::Through();
const PainterColor Sclera  = PainterColor::Rgb(0xF6, 0xF8, 0xD8);
const PainterColor Pupil   = PainterColor::Rgb(0x13, 0x20, 0x0A);
const PainterColor Gloss   = PainterColor::Rgb(0xF4, 0xFF, 0xE0);
const PainterColor Shadow  = PainterColor::Rgb(0x16, 0x1E, 0x1A);

/* The shadow on the ground: this much of 255 opaque, and fading out over this
 * many pixels at its edge. */
constexpr std::uint8_t ShadowOpacity = 150;
constexpr float        ShadowSoft    = 1.5f;

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

/* What the person set, read again whenever the file is saved. */
struct Look {
    float        Speed = 1.0f;
    float        Size  = 1.0f;
    PainterColor Body  = PainterColor::Rgb(0x9F, 0xD7, 0x24);
    bool         Trail = true;
    bool         Shadowed = true;
    bool         Smooth   = false;
    unsigned     Fps      = 20;
};

Look ReadLook()
{
    using box::settings;
    return Look{.Speed = static_cast<float>(settings::number("speed")) / 100.0f,
                .Size  = static_cast<float>(settings::number("size")) / 100.0f,
                .Body  = PainterColor::FromRaw(settings::color("color")),
                .Trail = settings::on("trail"),
                .Shadowed = settings::on("shadow"),
                .Smooth   = settings::on("smooth"),
                .Fps      = static_cast<unsigned>(settings::number("fps"))};
}

/* Five tones from shadow to highlight, and the line around a shape. */
struct Ramp {
    PainterColor Tone[5];
    PainterColor Outline;
};

Ramp MakeRamp(PainterColor Base)
{
    const PainterColor Deep = PainterColor::Rgb(0x06, 0x24, 0x18);
    const PainterColor Pale = PainterColor::Rgb(0xFF, 0xFF, 0xE8);
    return Ramp{{PainterColor::Mix(Base, Deep, 150), PainterColor::Mix(Base, Deep, 70), Base,
                 PainterColor::Mix(Base, Pale, 80), PainterColor::Mix(Base, Pale, 160)},
                PainterColor::Mix(Base, Deep, 205)};
}

Ramp LightEye()
{
    return Ramp{{PainterColor::Rgb(0xC8, 0xD0, 0xB4), Sclera, Sclera, Sclera, Sclera}, PainterColor::Rgb(0x10, 0x30, 0x14)};
}

Ramp Flat(PainterColor Ink)
{
    return Ramp{{Ink, Ink, Ink, Ink, Ink}, Ink};
}

int Band(float Nx, float Ny)
{
    float Flat = std::min(1.0f, Nx * Nx + Ny * Ny);
    float Lit  = std::clamp(-0.5f * Nx - 0.6f * Ny + 0.62f * std::sqrt(1.0f - Flat), 0.0f, 1.0f);
    return Lit < 0.25f ? 0 : Lit < 0.5f ? 1 : Lit < 0.75f ? 2 : Lit < 0.9f ? 3 : 4;
}

/* The pixels of the box that holds the slime, placed on the glass by its
 * upper left; every shape is given in pixels of the glass, which are square:
 * a column across and half a row down. A pixel on a shape's edge is as opaque
 * as the share of it the shape covers, so the edge is soft, and what is drawn
 * later goes over what is drawn before; unless it is set crisp, when each such
 * pixel is whole or none. */
class Pixmap {
public:
    void Place(int Left, int Top, int Wide, int High)
    {
        Across = Left;
        Down   = Top;
        Width  = Wide;
        Height = High;
        Dots.assign(static_cast<std::size_t>(Width) * Height, Empty);
    }

    PainterColor At(int X, int Y) const { return Dots[static_cast<std::size_t>(Y) * Width + X]; }

    void Put(int X, int Y, PainterColor Ink)
    {
        X -= Across;
        Y -= Down;
        if (X < 0 || X >= Width || Y < 0 || Y >= Height) return;
        PainterColor &Here = Dots[static_cast<std::size_t>(Y) * Width + X];
        Here               = Ink.IsTranslucent() ? PainterColor::Mix(Here, Ink, 255) : Ink;
    }

    /* The pixel whose middle lies Beyond a shape's edge, in pixels: Inside
     * where the shape covers it, Line where the line of a pixel's width round
     * the shape does, as opaque as the two cover it together. */
    void Edge(int X, int Y, float Beyond, PainterColor Line, PainterColor Inside)
    {
        const float Fill = painter::core::Snapped(painter::core::InsideShare(Beyond), Smooth);
        const float Rim  = painter::core::Snapped(painter::core::BandShare(Beyond, 0.0f, 1.0f), Smooth);
        if (Fill + Rim <= 0.0f) return;
        const PainterColor Shown = PainterColor::Mix(Line, Inside, static_cast<std::uint8_t>(Fill / (Fill + Rim) * 255.0f + 0.5f));
        Put(X, Y, Shown.WithOpacity(static_cast<std::uint8_t>(std::min(Fill + Rim, 1.0f) * 255.0f + 0.5f)));
    }

    void Oval(float Cx, float Cy, float Rx, float Ry, const Ramp &Ink);
    void Cast(float Cx, float Cy, float Rx, float Ry, PainterColor Ink, std::uint8_t Opacity);

    int  Width = 0, Height = 0;
    bool Smooth = false;

private:
    std::vector<PainterColor> Dots;
    int                       Across = 0, Down = 0;
};

int Whole(float Value)
{
    return static_cast<int>(std::floor(Value));
}

void Pixmap::Oval(float Cx, float Cy, float Rx, float Ry, const Ramp &Ink)
{
    for (int Y = Whole(Cy - Ry - 1); Y <= Whole(Cy + Ry + 1); Y++) {
        for (int X = Whole(Cx - Rx - 1); X <= Whole(Cx + Rx + 1); X++) {
            float Ex = (X + 0.5f - Cx) / Rx, Ey = (Y + 0.5f - Cy) / Ry;
            float Beyond = (std::sqrt(Ex * Ex + Ey * Ey) - 1.0f) * std::min(Rx, Ry);
            if (Beyond > 1.5f) continue;
            Edge(X, Y, Beyond, Ink.Outline, Ink.Tone[Band(Ex, Ey)]);
        }
    }
}

/* An ellipse of one colour at Opacity that fades out over ShadowSoft pixels at its edge. */
void Pixmap::Cast(float Cx, float Cy, float Rx, float Ry, PainterColor Ink, std::uint8_t Opacity)
{
    for (int Y = Whole(Cy - Ry - 1); Y <= Whole(Cy + Ry + 1); Y++) {
        for (int X = Whole(Cx - Rx - 1); X <= Whole(Cx + Rx + 1); X++) {
            float Ex = (X + 0.5f - Cx) / Rx, Ey = (Y + 0.5f - Cy) / Ry;
            float Beyond = (std::sqrt(Ex * Ex + Ey * Ey) - 1.0f) * std::min(Rx, Ry);
            float Share  = painter::core::InsideShare(Beyond / ShadowSoft);
            if (Share > 0.0f) Put(X, Y, Ink.WithOpacity(static_cast<std::uint8_t>(Share * Opacity + 0.5f)));
        }
    }
}

/* The ground it stands on, a hop at a time. */
struct Spot {
    float X = 0, Y = 0;
};

struct Slick {
    Spot  At;
    float Wide = 0, Age = 0;
};

enum class Act { Rest, Crouch, Fly, Sleep };

class Slime {
public:
    void Begin(int Cols, int Rows, float Zoom);
    void Fit(float Zoom);
    void Tune(const Look &Set) { Pace = Set.Speed; Trail = Set.Trail; }
    /* Time passing; true while anything moves. */
    bool Step(float Seconds);

    float Wide() const { return Round * (1.0f + Squash); }
    float High() const { return Tall / (1.0f + Squash); }

    Spot               Foot;     /* where it stands, on the ground, in pixels */
    float              Air = 0;  /* how high the body is lifted */
    float              Squash = 0, Ripple = 0, Phase = 0;
    float              LookX = 0, LookY = 0, Blink = 0;
    float              Round = 0, Tall = 0;
    bool               Surprised = false;
    std::vector<Slick> Slicks;

private:
    void  Next();
    void  Aim();
    void  Land();
    void  Fly(float Seconds);
    void  Spring(float Seconds, float Goal);
    void  Blinks(float Seconds);
    Spot  Clamp(Spot Place) const;

    int   Cols = 0, Rows = 0;
    float Zoom = 1.0f, Pace = 1.0f, Peak = 0, Held = 0, Velocity = 0, Spent = 0, Left = 1.0f, Taken = 1.0f;
    float ToBlink = 2.0f, Blinking = -1;
    bool  Trail = true;
    Spot  From, To, Goal;
    Act   Doing = Act::Rest;
};

void Slime::Begin(int ColsNow, int RowsNow, float ZoomNow)
{
    Seed = cpu_rdtsc() | 1;
    Cols = ColsNow;
    Rows = RowsNow;
    Fit(ZoomNow);
    Foot = Clamp({Between(0, static_cast<float>(Cols)), Between(0, static_cast<float>(Rows) * 2.0f)});
    Goal = Foot;
    Next();
}

void Slime::Fit(float ZoomNow)
{
    Zoom  = ZoomNow;
    Round = 26.0f * Zoom;
    Tall  = 18.0f * Zoom;
    Foot  = Clamp(Foot);
}

Spot Slime::Clamp(Spot Place) const
{
    float Side = Round * 0.9f;
    return {std::clamp(Place.X, Side, std::max(Side, Cols - Side)),
            std::clamp(Place.Y, FirstRow * 2.0f + Tall * 1.7f, std::max(FirstRow * 2.0f + Tall * 1.7f, Rows * 2.0f - 3.0f))};
}

/* A rest ends in a hop, or now and then a sleep. */
void Slime::Next()
{
    Spent = 0;
    if (Doing == Act::Rest && Unit() < 0.1f) {
        Doing = Act::Sleep;
        Left  = Between(3.0f, 6.0f) / Pace;
        return;
    }
    Doing = Act::Crouch;
    Left  = 0.3f / Pace;
    Aim();
}

/* Where the next hop lands: a way toward the place it is bound for. */
void Slime::Aim()
{
    float Far = std::hypot(Goal.X - Foot.X, Goal.Y - Foot.Y);
    if (Far < Round * 0.5f) {
        Goal = Clamp({Between(0, static_cast<float>(Cols)), Between(0, static_cast<float>(Rows) * 2.0f)});
        Far  = std::hypot(Goal.X - Foot.X, Goal.Y - Foot.Y);
    }
    float Hop = std::min(Far, Between(0.7f, 1.0f) * 22.0f * Zoom);
    From = Foot;
    To   = Clamp({Foot.X + (Goal.X - Foot.X) / Far * Hop, Foot.Y + (Goal.Y - Foot.Y) / Far * Hop});
    Hop   = std::hypot(To.X - From.X, To.Y - From.Y);
    Taken = (0.5f + Hop * 0.008f / Zoom) / Pace;
    Peak  = 6.0f * Zoom + Hop * 0.22f;
    Held  = 0;
    LookX = To.X >= From.X ? 1.0f : -1.0f;
}

void Slime::Land()
{
    Doing = Act::Rest;
    Spent = 0;
    Left  = Between(0.6f, 2.2f) / Pace;
    Air   = 0;
    Foot  = To;
    Velocity += 7.0f;
    Blinking = 0;
    if (Trail) Slicks.push_back({To, Round * Between(0.8f, 1.1f), 0});
}

void Slime::Fly(float Seconds)
{
    Held += Seconds;
    float Part = std::min(1.0f, Held / Taken);
    Foot.X = From.X + (To.X - From.X) * Part;
    Foot.Y = From.Y + (To.Y - From.Y) * Part;
    Air    = 4.0f * Peak * Part * (1.0f - Part);
    if (Part >= 1.0f) Land();
}

/* The squash is a spring: pushed, it overshoots and settles. */
void Slime::Spring(float Seconds, float Goal)
{
    for (float Left = Seconds; Left > 0; Left -= 0.02f) {
        float Dt = std::min(0.02f, Left);
        Velocity += (-150.0f * (Squash - Goal) - 9.0f * Velocity) * Dt;
        Squash   += Velocity * Dt;
    }
    Ripple = std::clamp(std::fabs(Velocity) * 0.025f, 0.0f, 0.14f);
    Phase += Seconds * 16.0f;
}

void Slime::Blinks(float Seconds)
{
    constexpr float Length = 0.2f;
    ToBlink -= Seconds;
    if (Blinking < 0 && ToBlink <= 0) {
        Blinking = 0;
        ToBlink  = Between(2.0f, 6.0f);
    }
    float Shut = 0;
    if (Blinking >= 0) {
        Shut = std::sin(Blinking / Length * Pi);
        Blinking += Seconds;
        if (Blinking >= Length) Blinking = -1;
    }
    Ease(Blink, Doing == Act::Sleep ? 1.0f : Doing == Act::Crouch ? std::max(Shut, 0.4f) : Shut, 30.0f, Seconds);
}

bool Slime::Step(float Seconds)
{
    Left  -= Seconds;
    Spent += Seconds;
    Surprised = Doing == Act::Fly;
    switch (Doing) {
    case Act::Fly: {
        float Part = std::min(1.0f, Held / Taken);
        Spring(Seconds, -0.2f * std::pow(std::fabs(1.0f - 2.0f * Part), 0.7f) + 0.04f);
        Fly(Seconds);
        Ease(LookY, -0.3f, 8.0f, Seconds);
        break;
    }
    case Act::Crouch:
        Spring(Seconds, 0.38f);
        if (Left <= 0) {
            Doing = Act::Fly;
            Velocity -= 6.0f;
        }
        break;
    case Act::Sleep:
        Spring(Seconds, 0.1f + 0.05f * std::sin(Spent * 1.6f));
        Ease(LookY, 0.5f, 3.0f, Seconds);
        if (Left <= 0) Next();
        break;
    default:
        Spring(Seconds, 0.03f * std::sin(Spent * 2.2f));
        Ease(LookX, 0.9f * std::sin(Spent * 0.8f + Foot.X), 3.0f, Seconds);
        Ease(LookY, 0.25f * std::sin(Spent * 0.5f), 3.0f, Seconds);
        if (Left <= 0) Next();
        break;
    }
    Blinks(Seconds);
    for (Slick &One : Slicks) One.Age += Seconds * Pace;
    std::erase_if(Slicks, [](const Slick &One) { return One.Age >= SlickLife; });
    return Doing == Act::Fly || Doing == Act::Crouch || std::fabs(Velocity) > 0.15f || std::fabs(Squash) > 0.06f
           || Blinking >= 0 || !Slicks.empty();
}

/* The body: jelly lit from the upper left, with its glow, heart and bubbles. */
struct Figure {
    float Cx, Bottom, Rx, Ry, Ripple, Phase;
};

float Away(float Across, float Down)
{
    return Across * Across + Down * Down;
}

PainterColor Jelly(const Ramp &Ink, float Ex, float Ey, float Far)
{
    int Tone = Band(Ex, Ey);
    if (Ey > 0.3f) Tone = Far < 0.86f ? 3 : 1;
    float Heart = Away((Ex - 0.12f) / 0.38f, (Ey - 0.18f) / 0.32f);
    if (Heart < 1.0f) Tone = 1;
    else if (Heart < 1.64f && Ey > 0.0f) Tone = 3;
    if (Away((Ex + 0.42f) / 0.2f, (Ey + 0.55f) / 0.1f) < 1.0f) return Gloss;
    if (Away(Ex + 0.16f, Ey + 0.72f) < 0.003f) return Gloss;
    if (Away(Ex - 0.5f, Ey - 0.4f) < 0.005f || Away(Ex + 0.52f, Ey - 0.45f) < 0.0025f) return Ink.Tone[4];
    return Ink.Tone[Tone];
}

/* Column by column: the crown ripples along its width, a wave turned a step
 * at a time instead of asked of sin for every pixel. */
void DrawBody(Pixmap &Art, const Ramp &Ink, const Figure &Shape)
{
    float Cy = Shape.Bottom - Shape.Ry, Edge = std::min(Shape.Rx, Shape.Ry);
    float Step = 3.2f / Shape.Rx, Cos = std::cos(Step), Sin = std::sin(Step);
    int   Left = Whole(Shape.Cx - Shape.Rx - 2);
    float Wave = std::sin((Left + 0.5f - Shape.Cx) / Shape.Rx * 3.2f + Shape.Phase);
    float Turn = std::cos((Left + 0.5f - Shape.Cx) / Shape.Rx * 3.2f + Shape.Phase);
    for (int X = Left; X <= Whole(Shape.Cx + Shape.Rx + 2); X++) {
        float Ex = (X + 0.5f - Shape.Cx) / Shape.Rx, Crown = 1.0f + Shape.Ripple * Wave;
        for (int Y = Whole(Cy - Shape.Ry * 1.2f - 2); Y <= Whole(Shape.Bottom + 2); Y++) {
            float Ey = (Y + 0.5f - Cy) / Shape.Ry, Far;
            if (Ey < 0.0f) {
                Ey /= Crown;
                Far = std::sqrt(Away(Ex, Ey));
            } else {
                float Cube = std::fabs(Ex * Ex * Ex) + Ey * Ey * Ey;
                Far = (Cube + 2.0f) / 3.0f;   /* the cube root, near enough to the edge */
            }
            float Beyond = (Far - 1.0f) * Edge;
            if (Beyond > 1.5f) continue;
            Art.Edge(X, Y, Beyond, Ink.Outline, Beyond < 0.5f ? Jelly(Ink, Ex, Ey, Far) : Ink.Outline);
        }
        float Next = Wave * Cos + Turn * Sin;
        Turn = Turn * Cos - Wave * Sin;
        Wave = Next;
    }
}

struct Palette {
    Ramp Body, Eye;
};

void DrawEye(Pixmap &Art, const Palette &Ink, const Slime &Creature, float Cx, float Cy, float Radius)
{
    Art.Oval(Cx, Cy, Radius, Radius, Ink.Eye);
    float Seeing = std::max(0.9f, Radius * 0.52f);
    float PupilX = Cx + Creature.LookX * Radius * 0.35f, PupilY = Cy + Creature.LookY * Radius * 0.3f;
    Art.Oval(PupilX, PupilY, Creature.Surprised ? Seeing * 0.8f : Seeing, Creature.Surprised ? Seeing * 0.8f : Seeing, Flat(Pupil));
    Art.Put(Whole(PupilX - Seeing * 0.4f), Whole(PupilY - Seeing * 0.5f), Gloss);
    for (float Down = 0; Down < Creature.Blink * Radius * 2.0f; Down += 0.5f) {
        float Y = Cy - Radius + Down, Half = std::sqrt(std::max(0.0f, Radius * Radius - (Y - Cy) * (Y - Cy)));
        for (float Across = -Half; Across <= Half; Across += 0.5f)
            Art.Put(Whole(Cx + Across), Whole(Y), Ink.Body.Tone[2]);
    }
}

void DrawFace(Pixmap &Art, const Palette &Ink, const Slime &Creature, const Figure &Shape, float Zoom)
{
    float Cy = Shape.Bottom - Shape.Ry, Eyes = Cy - Shape.Ry * 0.22f;
    float Radius = std::max(1.8f, 3.8f * Zoom * (Creature.Surprised ? 1.12f : 1.0f));
    for (float Side : {-1.0f, 1.0f}) {
        DrawEye(Art, Ink, Creature, Shape.Cx + Side * Shape.Rx * 0.4f, Eyes, Radius);
    }
    float MouthY = Cy + Shape.Ry * 0.5f;
    if (Creature.Surprised) {
        Art.Oval(Shape.Cx, MouthY, std::max(1.0f, 1.2f * Zoom), std::max(1.0f, 1.3f * Zoom), Flat(Ink.Body.Outline));
        return;
    }
    float Reach = std::max(1.5f, 2.2f * Zoom);
    for (float Across = -Reach; Across <= Reach; Across += 0.5f)
        Art.Put(Whole(Shape.Cx + Across), Whole(MouthY + 1.2f * Zoom * (1.0f - Across * Across / (Reach * Reach))), Ink.Body.Outline);
}

void Draw(Pixmap &Art, const Palette &Ink, const Slime &Creature, float Zoom, bool Shadowed)
{
    for (const Slick &One : Creature.Slicks) {
        float Left = 1.0f - One.Age / SlickLife, Rx = std::max(1.0f, One.Wide * 0.5f * std::sqrt(Left));
        if (Left > 0.02f) Art.Oval(One.At.X, One.At.Y, Rx, std::max(0.9f, Rx * 0.16f), Ink.Body);
    }
    float Height = Creature.Air;
    if (Shadowed) {
        float Shade = Creature.Round * 0.46f * (1.0f - std::min(0.5f, Height / (Creature.Round * 1.2f)));
        if (Art.Smooth) Art.Cast(Creature.Foot.X, Creature.Foot.Y + 0.5f, Shade, std::max(1.0f, Shade * 0.16f), Shadow, ShadowOpacity);
        else Art.Oval(Creature.Foot.X, Creature.Foot.Y + 0.5f, Shade, std::max(1.0f, Shade * 0.16f), Flat(Shadow));
    }
    Figure Shape{Creature.Foot.X, Creature.Foot.Y - Height, Creature.Wide() * 0.5f, Creature.High() * 0.5f, Creature.Ripple, Creature.Phase};
    DrawBody(Art, Ink.Body, Shape);
    DrawFace(Art, Ink, Creature, Shape, Zoom);
}

/* The box on the glass that holds the slime and its slicks, in pixels. */
struct Reach {
    int Left, Top, Right, Bottom;
};

Reach Holding(const Slime &Creature, int Cols, int Rows)
{
    float Side = Creature.Round * 1.0f;
    float Left = Creature.Foot.X - Side, Right = Creature.Foot.X + Side;
    float Top  = Creature.Foot.Y - Creature.Air - Creature.Tall * 1.8f, Bottom = Creature.Foot.Y + 3.0f;
    for (const Slick &One : Creature.Slicks) {
        Left   = std::min(Left, One.At.X - One.Wide * 0.5f - 1.0f);
        Right  = std::max(Right, One.At.X + One.Wide * 0.5f + 1.0f);
        Top    = std::min(Top, One.At.Y - 3.0f);
        Bottom = std::max(Bottom, One.At.Y + 3.0f);
    }
    return {std::max(0, Whole(Left)), std::max(FirstRow * 2, Whole(Top) & ~1), std::min(Cols, Whole(Right) + 1),
            std::min(Rows * 2, (Whole(Bottom) + 2) & ~1)};
}

/* Replace the retained footprint: transparent cells reveal the console, so
 * the slime leaves no earlier frame behind. */
box::status Paint(const Pixmap &Art, painter::tui::Surface &Screen, painter::tui::Frame &Glass, const Reach &Box)
{
    if (auto Cleared = Screen.Clear(); !Cleared) return Cleared;
    painter::core::Rect Cells{Box.Left, Box.Top / 2, Art.Width, Art.Height / 2};
    Glass.Fill(Cells, painter::tui::Cell::TransparentCell());
    for (int Row = 0; Row < Cells.Height; Row++) {
        for (int X = 0; X < Art.Width; X++) {
            PainterColor Top = Art.At(X, Row * 2), Low = Art.At(X, Row * 2 + 1);
            if (Top == Empty && Low == Empty) continue;
            Glass.Put({Cells.X + X, Cells.Y + Row}, painter::tui::Cell{painter::tui::Glyphs::UpperHalf, Top, Low});
        }
    }
    return Screen.Composite({Cells.X, Cells.Y}, Glass, Cells);
}

float ZoomFor(int Cols, const Look &Set)
{
    return std::clamp(static_cast<float>(Cols) / 180.0f, 0.6f, 1.5f) * Set.Size;
}

}

int main()
{
    std::expected<painter::tui::Surface, painter::tui::StageError> Opened =
        painter::tui::Surface::Open(30, 10, painter::tui::SurfaceRole::Notice);
    if (!Opened) {
        Say(Opened.error().Describe("slime"));
        return 1;
    }
    painter::tui::Surface Screen = std::move(*Opened);
    painter::core::Size   Dimensions = Screen.Size();
    std::expected<painter::tui::Frame, box::error> Created = painter::tui::Frame::Create(Dimensions);
    if (!Created) return 1;
    painter::tui::Frame Glass = std::move(*Created);

    Look    Set = ReadLook();
    Palette Ink{MakeRamp(Set.Body), LightEye()};
    float   Zoom = ZoomFor(Dimensions.Width, Set);
    Pixmap  Art;
    Slime   Creature;
    Creature.Tune(Set);
    Creature.Begin(Dimensions.Width, Dimensions.Height, Zoom);
    box::settings::watch Watch;

    // Painted until it can be no more — the display gone, or the slime
    // ended: nothing but its end stops it.
    std::uint64_t Last = clock_uptime_us();
    box::pacer Pace;
    for (;;) {
        if (Watch.changed()) {
            Set  = ReadLook();
            Ink  = Palette{MakeRamp(Set.Body), LightEye()};
            Zoom = ZoomFor(Dimensions.Width, Set);
            Creature.Tune(Set);
            Creature.Fit(Zoom);
        }
        std::uint64_t Now    = clock_uptime_us();
        bool          Moving = Creature.Step(std::min(0.5f, static_cast<float>(Now - Last) / 1e6f));
        Last = Now;
        Reach Box = Holding(Creature, Dimensions.Width, Dimensions.Height);
        Art.Place(Box.Left, Box.Top, Box.Right - Box.Left, Box.Bottom - Box.Top);
        Art.Smooth = Set.Smooth;
        Draw(Art, Ink, Creature, Zoom, Set.Shadowed);
        if (!Paint(Art, Screen, Glass, Box)) break;
        const std::chrono::microseconds Interval = box::pacer::interval(Set.Fps);
        std::this_thread::sleep_for(Pace.wait(Moving ? Interval : std::max(Interval, RestInterval)));
    }
    (void)Screen.Close();
    return 1;
}
