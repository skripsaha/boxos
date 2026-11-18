// effects.cpp — the effect over the whole glass: what is laid, how long it
// lasts, and how it changes the picture (box/display.h).

#include "effects.h"

#include <algorithm>

#include "box/clock.h"
#include "box/color.h"
#include "boxos_pixel.h"
#include "effects_ink.h"
#include "effects_warp.h"

namespace {

using effects::Whole;

constexpr std::uint64_t LongestRampMs = 800;    // how long an effect swells in, and ebbs out

// The kinds that are never still: a frame is drawn every so often while they lie.
constexpr std::uint32_t MovingKinds = DISPLAY_EFFECT_SWAY | DISPLAY_EFFECT_SHIMMER | DISPLAY_EFFECT_RIPPLE |
                                      DISPLAY_EFFECT_QUAKE | DISPLAY_EFFECT_RAINBOW | DISPLAY_EFFECT_MELT |
                                      DISPLAY_EFFECT_GLITCH | DISPLAY_EFFECT_SPOTLIGHT | DISPLAY_EFFECT_SEPIA |
                                      DISPLAY_EFFECT_SNOW | DISPLAY_EFFECT_PULSE;
// Of those, the ones that are drawn at the screen's full rate: the rest drift
// slowly enough to be drawn at two thirds of it, or half, which costs a text page nothing and
// a framebuffer much less.
constexpr std::uint32_t QuickKinds = DISPLAY_EFFECT_SWAY | DISPLAY_EFFECT_SHIMMER | DISPLAY_EFFECT_QUAKE |
                                     DISPLAY_EFFECT_GLITCH;
// The kinds that move whole cells about: they need an axis to say which way.
constexpr std::uint32_t AxisKinds = DISPLAY_EFFECT_SWAY | DISPLAY_EFFECT_MIRROR | DISPLAY_EFFECT_QUAKE;
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
std::uint64_t g_calm_ms = 66;     // between two frames of a drift

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
    const std::uint64_t Between = (g_laid.Request.kinds & QuickKinds) ? g_frame_ms : g_calm_ms;
    if (Moving || InRamp) g_laid.NextMs = std::min(Now + Between, g_laid.EndMs);
    else if (Swelling) g_laid.NextMs = g_laid.EndMs - g_laid.RampMs;
    else g_laid.NextMs = g_laid.EndMs;
}

}  // namespace

void EffectsFrameRate(std::uint32_t PerSecond) noexcept
{
    g_frame_ms = std::max<std::uint32_t>(1, 1000u / std::max<std::uint32_t>(1, PerSecond));
    g_calm_ms = std::max<std::uint64_t>(g_frame_ms, 66);
}

error_t EffectsLay(const AskFrom &From, const DisplayEffectRequest &Request) noexcept
{
    const std::uint32_t Kinds = Request.kinds;
    if (Kinds == 0 || (Kinds & ~static_cast<std::uint32_t>(DISPLAY_EFFECT_KINDS)) != 0) return ERR_INVALID_ARGUMENT;
    if (Request.strength == 0 || (Request.axes & ~(DISPLAY_EFFECT_AXIS_X | DISPLAY_EFFECT_AXIS_Y)) != 0)
        return ERR_INVALID_ARGUMENT;
    if ((Kinds & AxisKinds) && Request.axes == 0) return ERR_INVALID_ARGUMENT;
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

void EffectsApply(std::span<const ItemCell> Composed, std::vector<ItemCell> &Glass, Caret &At, std::uint32_t Cols,
                  std::uint32_t Rows, std::uint32_t Ink, std::uint32_t Paper)
{
    static effects::Warp Plan;
    static effects::Ink Colours;
    const effects::Moment Now{g_laid.Request.kinds, g_laid.Request.strength, g_laid.Request.color, g_laid.Request.axes,
                              g_laid.Level, g_laid.PhaseMs, g_laid.Frame, Cols, Rows};
    effects::WarpPlan(Plan, Now);
    effects::InkPlan(Colours, Now);
    const std::size_t Seen = At.Visible && At.X < Cols && At.Y < Rows
                                 ? static_cast<std::size_t>(At.Y) * Cols + At.X
                                 : Composed.size();
    bool Carried = false;

    Glass.resize(Composed.size());
    for (std::uint32_t Y = 0; Y < Rows; Y++) {
        for (std::uint32_t X = 0; X < Cols; X++) {
            const effects::Source Taken = effects::WarpSource(Plan, Now, static_cast<std::int32_t>(X), static_cast<std::int32_t>(Y));
            const std::size_t From = static_cast<std::size_t>(Taken.Y) * Cols + static_cast<std::size_t>(Taken.X);
            const std::size_t To = static_cast<std::size_t>(Y) * Cols + X;

            ItemCell Cell = Composed[From];
            if (From == Seen && !Carried) {
                At.X = X;
                At.Y = Y;
                Carried = true;
            }
            if (const std::int32_t Apart = Plan.Split[Y]; Apart != 0) {
                const std::size_t Row = static_cast<std::size_t>(Taken.Y) * Cols;
                const auto Beside = [&](std::int32_t By) {
                    return Row + static_cast<std::size_t>(effects::Wrap(Taken.X + By, static_cast<std::int32_t>(Cols)));
                };
                effects::InkSplit(Cell, Composed[Beside(-Apart)], Composed[Beside(Apart)], Ink, Paper);
            }
            if (Colours.Any) {
                const std::uint32_t Light = effects::InkLight(Colours, Now, static_cast<std::int32_t>(X), static_cast<std::int32_t>(Y));
                Cell.Ink   = effects::InkColour(Colours, Cell.Ink, Ink, static_cast<std::int32_t>(X), Light);
                Cell.Paper = effects::InkColour(Colours, Cell.Paper, Paper, static_cast<std::int32_t>(X), Light);
                if (Cell.Mark != ITEM_MARK_NONE)
                    Cell.MarkInk = effects::InkColour(Colours, Cell.MarkInk, Ink, static_cast<std::int32_t>(X), Light);
                if (static_cast<std::int32_t>(X) == Colours.Scratch[0] || static_cast<std::int32_t>(X) == Colours.Scratch[1])
                    effects::InkScratch(Cell, Ink);
            }
            if (Colours.Sparkle != 0) {
                const std::uint32_t Roll = effects::Hash(static_cast<std::uint32_t>(To) * 2654435761u + (g_laid.Frame >> 1));
                if (Roll % 1000u < Colours.Sparkle) effects::InkSparkle(Cell, Roll, Colours.Star, Ink);
            }
            Glass[To] = Cell;
        }
    }
    if (Now.Kinds & DISPLAY_EFFECT_SNOW) effects::InkSnow(Glass, Now, Ink, Paper);
    At.Visible = At.Visible && Carried;
}

void EffectsPaint(const std::uint32_t *Picture, std::size_t Stride, std::uint32_t Width, std::uint32_t Height,
                  std::uint32_t *Glass, std::size_t GlassStride, std::uint32_t Format, std::uint32_t Ink, std::uint32_t Across,
                  std::uint32_t Down)
{
    static effects::Warp       Plan;
    static effects::Ink        Colours;
    static std::vector<std::uint32_t> Row, Lit;
    if (Width == 0 || Height == 0) return;
    const effects::Moment Now{g_laid.Request.kinds, g_laid.Request.strength, g_laid.Request.color, g_laid.Request.axes,
                              g_laid.Level,         g_laid.PhaseMs,          g_laid.Frame,         Width,
                              Height,               1,                       std::max<std::uint32_t>(1, Across),
                              std::max<std::uint32_t>(1, Down)};
    effects::WarpPlan(Plan, Now);
    effects::InkPlan(Colours, Now);
    const bool Moves = (Now.Kinds & (DISPLAY_EFFECT_SWAY | DISPLAY_EFFECT_MIRROR | DISPLAY_EFFECT_RIPPLE | DISPLAY_EFFECT_QUAKE |
                                     DISPLAY_EFFECT_MOSAIC | DISPLAY_EFFECT_MELT | DISPLAY_EFFECT_GLITCH | DISPLAY_EFFECT_PULSE)) != 0;
    const auto Cols = static_cast<std::int32_t>(Width);
    const auto At   = [&](std::int32_t X, std::int32_t Y) { return Picture[static_cast<std::size_t>(Y) * Stride + static_cast<std::size_t>(X)]; };
    const auto Put  = [Format](std::uint32_t Colour) { return BoxPixelEncode(Colour, Format); };

    /* A pixel as the effect shows it, before the stars and the snow: taken
     * from where the picture moved, its colours come apart where a band tore,
     * graded, lit, scratched. */
    const auto Shown = [&](std::int32_t X, std::int32_t Y) {
        std::int32_t FromX = X, FromY = Y;
        if (Moves) {
            const effects::Source Taken = effects::WarpSource(Plan, Now, X, Y);
            FromX = Taken.X, FromY = Taken.Y;
        }
        std::uint32_t Colour = At(FromX, FromY);
        if (const std::int32_t Apart = Plan.Split[static_cast<std::size_t>(Y)]; Apart != 0)
            Colour = effects::InkApart(Colour, At(effects::Wrap(FromX - Apart, Cols), FromY), At(effects::Wrap(FromX + Apart, Cols), FromY));
        if (Colours.Any)
            Colour = effects::InkColour(Colours, Colour, Ink, X, effects::InkLight(Colours, Now, X, Y));
        for (const std::int32_t Scratch : Colours.Scratch)
            if (Scratch >= 0 && X >= Scratch && X < Scratch + Colours.ScratchWide) Colour = effects::InkScratched(Colour);
        return Colour & 0x00FFFFFFu;
    };

    /* The stars of a shimmer: in a cell's square of pixels now and then, a
     * small cross of light at its middle. */
    const std::uint32_t Squares = (Width + Now.Across - 1) / Now.Across;
    const std::int32_t  Arm     = static_cast<std::int32_t>(std::max<std::uint32_t>(1, Now.Across / 4));
    try {
        Row.resize(Width);
        Lit.resize(Squares);
    } catch (...) {
        return;
    }
    std::uint32_t LitRow = UINT32_MAX;
    for (std::uint32_t Y = 0; Y < Height; Y++) {
        if (Colours.Sparkle != 0 && Y / Now.Down != LitRow) {
            LitRow = Y / Now.Down;
            for (std::uint32_t Square = 0; Square < Squares; Square++) {
                const std::uint32_t Roll = effects::Hash((LitRow * Squares + Square) * 2654435761u + (g_laid.Frame >> 1));
                Lit[Square] = Roll % 1000u < Colours.Sparkle ? 1u : 0u;
            }
        }
        for (std::uint32_t X = 0; X < Width; X++) {
            std::uint32_t Colour = Shown(static_cast<std::int32_t>(X), static_cast<std::int32_t>(Y));
            if (Colours.Sparkle != 0 && Lit[X / Now.Across]) {
                const std::int32_t DX = static_cast<std::int32_t>(X % Now.Across) - static_cast<std::int32_t>(Now.Across / 2);
                const std::int32_t DY = static_cast<std::int32_t>(Y % Now.Down) - static_cast<std::int32_t>(Now.Down / 2);
                if ((DY == 0 && DX >= -Arm && DX <= Arm) || (DX == 0 && DY >= -Arm && DY <= Arm)) Colour = Colours.Star;
            }
            Row[X] = Put(Colour);
        }
        std::copy(Row.begin(), Row.end(), Glass + static_cast<std::size_t>(Y) * GlassStride);
    }

    /* The snow over all of it: each flake a small round of light, laid over
     * the pixels it falls on as the effect shows them. */
    if (Now.Kinds & DISPLAY_EFFECT_SNOW) {
        const std::int32_t Reach = static_cast<std::int32_t>(std::max<std::uint32_t>(1, Now.Across / 5));
        effects::InkFlakes(Now, [&](std::int32_t X, std::int32_t Y, std::uint32_t Flake, std::uint32_t Weight) {
            for (std::int32_t DY = -Reach; DY <= Reach; DY++)
                for (std::int32_t DX = -Reach; DX <= Reach; DX++) {
                    const std::int32_t Far = DX * DX + DY * DY;
                    if (Far > Reach * Reach) continue;
                    const std::int32_t PX = X + DX, PY = Y + DY;
                    if (PX < 0 || PY < 0 || PX >= Cols || PY >= static_cast<std::int32_t>(Height)) continue;
                    const std::uint32_t Soft = Weight * static_cast<std::uint32_t>(Reach * Reach + 1 - Far) /
                                               static_cast<std::uint32_t>(Reach * Reach + 1);
                    Glass[static_cast<std::size_t>(PY) * GlassStride + static_cast<std::size_t>(PX)] =
                        Put(BoxColorOver(Shown(PX, PY), Flake, effects::Share(Soft)));
                }
        });
    }
}
