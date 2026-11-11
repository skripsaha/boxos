// effects.cpp — the effect over the whole glass: what is laid, how long it
// lasts, and how it changes the picture (box/display.h).

#include "effects.h"

#include <algorithm>

#include "box/clock.h"
#include "box/color.h"

namespace {

constexpr std::uint64_t LongestRampMs = 800;    // how long an effect swells in, and ebbs out
constexpr std::uint32_t Whole = 256;            // a level, or a mix, at its fullest

constexpr std::uint32_t MovingKinds = DISPLAY_EFFECT_SWAY | DISPLAY_EFFECT_SHIMMER;
constexpr std::uint32_t SwellingKinds = DISPLAY_EFFECT_KINDS & ~DISPLAY_EFFECT_MIRROR;

struct Laid {
    bool On = false;
    std::uint32_t Pid = 0;
    std::uint32_t Generation = 0;
    DisplayEffectRequest Request{};
    std::uint64_t StartMs = 0;
    std::uint64_t EndMs = 0;
    std::uint64_t RampMs = 0;
    std::uint64_t NextMs = 0;       // when the next frame, or the end of a still stretch, is due
    std::uint64_t PhaseMs = 0;      // how long it has lain, as of the last frame
    std::uint32_t Frame = 0;
    std::uint32_t Level = 0;        // 0..Whole: how far it has swelled in, or ebbed out
};

Laid g_laid;
std::uint64_t g_frame_ms = 66;

// A frame at `Now`: the phase, the level, and when the next is due.
void Refresh(std::uint64_t Now) noexcept
{
    const std::uint64_t Elapsed = Now - g_laid.StartMs;
    const std::uint64_t Left = g_laid.EndMs - Now;
    const std::uint64_t Nearest = std::min(Elapsed, Left);
    const bool Swelling = (g_laid.Request.kinds & SwellingKinds) != 0;
    const bool Moving = (g_laid.Request.kinds & MovingKinds) != 0;
    const bool InRamp = Swelling && (Elapsed < g_laid.RampMs || Left <= g_laid.RampMs);

    g_laid.PhaseMs = Elapsed;
    g_laid.Frame++;
    g_laid.Level = Nearest >= g_laid.RampMs ? Whole
                                            : static_cast<std::uint32_t>(Nearest * Whole / g_laid.RampMs);
    if (Moving || InRamp) g_laid.NextMs = std::min(Now + g_frame_ms, g_laid.EndMs);
    else if (Swelling) g_laid.NextMs = g_laid.EndMs - g_laid.RampMs;
    else g_laid.NextMs = g_laid.EndMs;
}

// A sine of a turn counted in 1024ths, by the parabola that fits it: -1024..1024.
std::int32_t Sine(std::uint32_t Turn) noexcept
{
    std::int32_t Place = static_cast<std::int32_t>(Turn & 1023u);
    const bool Below = Place >= 512;
    if (Below) Place -= 512;
    const std::int32_t Height = Place * (512 - Place) / 64;
    return Below ? -Height : Height;
}

std::uint32_t Hash(std::uint32_t Value) noexcept
{
    Value ^= Value >> 16;
    Value *= 0x7FEB352Du;
    Value ^= Value >> 15;
    Value *= 0x846CA68Bu;
    return Value ^ (Value >> 16);
}

std::int32_t Wrap(std::int32_t Place, std::int32_t Size) noexcept
{
    const std::int32_t Left = Place % Size;
    return Left < 0 ? Left + Size : Left;
}

// How far each row and each column is moved off its place, in cells.
struct Swing {
    std::vector<std::int32_t> Row;
    std::vector<std::int32_t> Column;
};

void Swinging(Swing &Out, std::uint32_t Cols, std::uint32_t Rows)
{
    Out.Row.assign(Rows, 0);
    Out.Column.assign(Cols, 0);
    if (!(g_laid.Request.kinds & DISPLAY_EFFECT_SWAY)) return;

    const std::uint32_t Strength = g_laid.Request.strength;
    const std::uint32_t RowTurn = static_cast<std::uint32_t>(g_laid.PhaseMs * 1024u / 1700u);
    const std::uint32_t ColumnTurn = static_cast<std::uint32_t>(g_laid.PhaseMs * 1024u / 2300u);
    const std::uint32_t WidestX = std::max<std::uint32_t>(2, Cols / 16);
    const std::uint32_t WidestY = std::max<std::uint32_t>(1, Rows / 12);
    const std::int32_t SwingX = static_cast<std::int32_t>((1 + (WidestX - 1) * Strength / 255) * g_laid.Level);
    const std::int32_t SwingY = static_cast<std::int32_t>((1 + (WidestY - 1) * Strength / 255) * g_laid.Level);
    constexpr std::int32_t Unit = 1024 * static_cast<std::int32_t>(Whole);

    if (g_laid.Request.axes & DISPLAY_EFFECT_AXIS_X)
        for (std::uint32_t Y = 0; Y < Rows; Y++)
            Out.Row[Y] = (Sine(RowTurn + Y * 56u) * SwingX + Unit / 2) / Unit;
    if (g_laid.Request.axes & DISPLAY_EFFECT_AXIS_Y)
        for (std::uint32_t X = 0; X < Cols; X++)
            Out.Column[X] = (Sine(ColumnTurn + X * 40u) * SwingY + Unit / 2) / Unit;
}

// The colours' changes: how far each goes, as a mix of 0..Whole.
struct Grade {
    bool Any = false;
    std::uint32_t Fade = 0;
    std::uint32_t Invert = 0;
    bool Tint = false;
    std::uint32_t TintMix = 0;
    std::uint32_t TintColor = 0;
};

Grade Graded() noexcept
{
    Grade Out;
    const std::uint32_t Kinds = g_laid.Request.kinds;
    const std::uint32_t Mix = g_laid.Level * g_laid.Request.strength / 255u;
    if (Kinds & DISPLAY_EFFECT_FADE) Out.Fade = Mix;
    if (Kinds & DISPLAY_EFFECT_INVERT) Out.Invert = Mix;
    if (Kinds & DISPLAY_EFFECT_TINT) {
        Out.Tint = true;
        Out.TintMix = Mix;
        Out.TintColor = g_laid.Request.color;
    }
    Out.Any = Out.Fade != 0 || Out.Invert != 0 || Out.Tint;
    return Out;
}

std::uint32_t Channel(std::uint32_t Colour, std::uint32_t Shift) noexcept
{
    return (Colour >> Shift) & 0xFFu;
}

std::uint32_t Pack(std::uint32_t Red, std::uint32_t Green, std::uint32_t Blue) noexcept
{
    return (Red << 16) | (Green << 8) | Blue;
}

// One channel `Mix` of the way (0..Whole) from `From` to `To`.
std::uint32_t Between(std::uint32_t From, std::uint32_t To, std::uint32_t Mix) noexcept
{
    const std::int32_t Step = (static_cast<std::int32_t>(To) - static_cast<std::int32_t>(From)) *
                              static_cast<std::int32_t>(Mix) / static_cast<std::int32_t>(Whole);
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(From) + Step);
}

std::uint32_t Brightened(std::uint32_t Colour, std::uint32_t Mix) noexcept
{
    return Pack(Between(Channel(Colour, 16), 255, Mix), Between(Channel(Colour, 8), 255, Mix),
                Between(Channel(Colour, 0), 255, Mix));
}

// A colour — a triple, or COLOR_DEFAULT drawn as Own — as the effect shows it.
std::uint32_t Coloured(std::uint32_t Colour, std::uint32_t Own, const Grade &Colours) noexcept
{
    if (Colour >> 24) Colour = Own;
    std::uint32_t Red = Channel(Colour, 16), Green = Channel(Colour, 8), Blue = Channel(Colour, 0);
    if (Colours.Fade != 0) {
        const std::uint32_t Grey = (Red * 77 + Green * 151 + Blue * 28) >> 8;
        Red = Between(Red, Grey, Colours.Fade);
        Green = Between(Green, Grey, Colours.Fade);
        Blue = Between(Blue, Grey, Colours.Fade);
    }
    if (Colours.Invert != 0) {
        Red = Between(Red, 255 - Red, Colours.Invert);
        Green = Between(Green, 255 - Green, Colours.Invert);
        Blue = Between(Blue, 255 - Blue, Colours.Invert);
    }
    if (Colours.Tint) {
        Red = Red * Between(255, Channel(Colours.TintColor, 16), Colours.TintMix) / 255;
        Green = Green * Between(255, Channel(Colours.TintColor, 8), Colours.TintMix) / 255;
        Blue = Blue * Between(255, Channel(Colours.TintColor, 0), Colours.TintMix) / 255;
    }
    return Pack(Red, Green, Blue);
}

// How many of the cells are lit in a frame, in thousandths.
std::uint32_t Sparkling() noexcept
{
    if (!(g_laid.Request.kinds & DISPLAY_EFFECT_SHIMMER)) return 0;
    return 70u * g_laid.Level * g_laid.Request.strength / (255u * Whole);
}

// A lit cell: an empty one becomes a star, a written one flashes.
void Sparkle(GlassCell &Cell, std::uint32_t Roll, std::uint32_t Star, std::uint32_t Ink) noexcept
{
    static constexpr char Stars[4] = {'*', '+', '.', static_cast<char>(0xFA)};
    if (Cell.Cell.ch == ' ' || Cell.Cell.ch == 0) {
        Cell.Cell.ch = Stars[(Roll >> 12) & 3u];
        Cell.Cell.fg = Star;
    } else {
        Cell.Cell.fg = Brightened((Cell.Cell.fg >> 24) ? Ink : Cell.Cell.fg, 190);
    }
}

}  // namespace

void EffectsFrameRate(std::uint32_t PerSecond) noexcept
{
    g_frame_ms = std::max<std::uint32_t>(1, 1000u / std::max<std::uint32_t>(1, PerSecond));
}

error_t EffectsLay(const AskFrom &From, const DisplayEffectRequest &Request) noexcept
{
    const std::uint32_t Kinds = Request.kinds;
    if (Kinds == 0 || (Kinds & ~static_cast<std::uint32_t>(DISPLAY_EFFECT_KINDS)) != 0) return ERR_INVALID_ARGUMENT;
    if (Request.strength == 0 || (Request.axes & ~(DISPLAY_EFFECT_AXIS_X | DISPLAY_EFFECT_AXIS_Y)) != 0)
        return ERR_INVALID_ARGUMENT;
    if ((Kinds & (DISPLAY_EFFECT_SWAY | DISPLAY_EFFECT_MIRROR)) && Request.axes == 0) return ERR_INVALID_ARGUMENT;
    if ((Request.color >> 24) != 0) return ERR_INVALID_ARGUMENT;
    if (Request.duration_ms == 0 || Request.duration_ms > DISPLAY_EFFECT_LONGEST_MS) return ERR_INVALID_ARGUMENT;

    const std::uint64_t Now = clock_uptime_ms();
    g_laid.On = true;
    g_laid.Pid = From.program_pid;
    g_laid.Generation = From.program_generation;
    g_laid.Request = Request;
    g_laid.StartMs = Now;
    g_laid.EndMs = Now + Request.duration_ms;
    g_laid.RampMs = std::max<std::uint64_t>(1, std::min<std::uint64_t>(LongestRampMs, Request.duration_ms / 4));
    g_laid.Frame = 0;
    Refresh(Now);
    return OK;
}

error_t EffectsLift(const AskFrom &From, bool Keeper, bool &Lifted) noexcept
{
    Lifted = false;
    if (!g_laid.On) return OK;
    const bool Own = g_laid.Pid == From.program_pid && g_laid.Generation == From.program_generation;
    if (!Own && !Keeper) return ERR_ACCESS_DENIED;
    g_laid.On = false;
    Lifted = true;
    return OK;
}

bool EffectsDrop(std::uint32_t Pid, std::uint32_t Generation) noexcept
{
    if (!g_laid.On || g_laid.Pid != Pid || g_laid.Generation != Generation) return false;
    g_laid.On = false;
    return true;
}

bool EffectsOn() noexcept
{
    return g_laid.On;
}

bool EffectsStep() noexcept
{
    if (!g_laid.On) return false;
    const std::uint64_t Now = clock_uptime_ms();
    if (Now >= g_laid.EndMs) {
        g_laid.On = false;
        return true;
    }
    if (Now < g_laid.NextMs) return false;
    Refresh(Now);
    return true;
}

std::uint64_t EffectsDue() noexcept
{
    return g_laid.On ? g_laid.NextMs : 0;
}

void EffectsApply(std::span<const GlassCell> Composed, std::vector<GlassCell> &Glass, Caret &At,
                  std::uint32_t Cols, std::uint32_t Rows, std::uint32_t Ink, std::uint32_t Paper)
{
    static Swing Swings;
    Swinging(Swings, Cols, Rows);
    const Grade Colours = Graded();
    const std::uint32_t Chance = Sparkling();
    const std::uint32_t Star = Brightened(g_laid.Request.color, 150);
    const bool Mirror = (g_laid.Request.kinds & DISPLAY_EFFECT_MIRROR) != 0;
    const bool MirrorX = Mirror && (g_laid.Request.axes & DISPLAY_EFFECT_AXIS_X);
    const bool MirrorY = Mirror && (g_laid.Request.axes & DISPLAY_EFFECT_AXIS_Y);
    const std::size_t Seen = At.Visible && At.X < Cols && At.Y < Rows
                                 ? static_cast<std::size_t>(At.Y) * Cols + At.X
                                 : Composed.size();
    bool Carried = false;

    Glass.resize(Composed.size());
    for (std::uint32_t Y = 0; Y < Rows; Y++) {
        for (std::uint32_t X = 0; X < Cols; X++) {
            std::int32_t FromX = MirrorX ? static_cast<std::int32_t>(Cols - 1 - X) : static_cast<std::int32_t>(X);
            std::int32_t FromY = MirrorY ? static_cast<std::int32_t>(Rows - 1 - Y) : static_cast<std::int32_t>(Y);
            FromX = Wrap(FromX - Swings.Row[Y], static_cast<std::int32_t>(Cols));
            FromY = Wrap(FromY - Swings.Column[X], static_cast<std::int32_t>(Rows));
            const std::size_t From = static_cast<std::size_t>(FromY) * Cols + static_cast<std::size_t>(FromX);
            const std::size_t To = static_cast<std::size_t>(Y) * Cols + X;

            GlassCell Cell = Composed[From];
            if (From == Seen && !Carried) {
                At.X = X;
                At.Y = Y;
                Carried = true;
            }
            if (Colours.Any) {
                Cell.Cell.fg = Coloured(Cell.Cell.fg, Ink, Colours);
                Cell.Cell.bg = Coloured(Cell.Cell.bg, Paper, Colours);
                if (Cell.Mark != DISPLAY_CELL_MARK_NONE) Cell.MarkColor = Coloured(Cell.MarkColor, Ink, Colours);
            }
            if (Chance != 0) {
                const std::uint32_t Roll = Hash(static_cast<std::uint32_t>(To) * 2654435761u + (g_laid.Frame >> 1));
                if (Roll % 1000u < Chance) Sparkle(Cell, Roll, Star, Ink);
            }
            Glass[To] = Cell;
        }
    }
    At.Visible = At.Visible && Carried;
}
