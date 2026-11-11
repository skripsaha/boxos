// effects_warp.cpp — the effects that move cells (box/display.h): sway,
// mirror, ripple, quake, mosaic, melt, glitch and pulse.

#include "effects_warp.h"

#include <algorithm>

#include "box/display.h"

namespace effects {

namespace {

constexpr std::int32_t Unit = 1024 * static_cast<std::int32_t>(Whole);

void PlanSway(Warp &Out, const Moment &Now)
{
    const std::uint32_t Cols = Now.Cols, Rows = Now.Rows;
    const std::uint32_t RowTurn = static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 1700u);
    const std::uint32_t ColumnTurn = static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 2300u);
    const std::uint32_t WidestX = std::max<std::uint32_t>(2, Cols / 16);
    const std::uint32_t WidestY = std::max<std::uint32_t>(1, Rows / 12);
    const std::int32_t SwingX = static_cast<std::int32_t>((1 + (WidestX - 1) * Now.Strength / 255) * Now.Level);
    const std::int32_t SwingY = static_cast<std::int32_t>((1 + (WidestY - 1) * Now.Strength / 255) * Now.Level);

    if (Now.Axes & DISPLAY_EFFECT_AXIS_X)
        for (std::uint32_t Y = 0; Y < Rows; Y++) Out.RowShift[Y] += (Sine(RowTurn + Y * 56u) * SwingX + Unit / 2) / Unit;
    if (Now.Axes & DISPLAY_EFFECT_AXIS_Y)
        for (std::uint32_t X = 0; X < Cols; X++) Out.ColumnShift[X] += (Sine(ColumnTurn + X * 40u) * SwingY + Unit / 2) / Unit;
}

// Bands of rows torn sideways, new ones every other frame, and how far the
// colours of each come apart.
void PlanGlitch(Warp &Out, const Moment &Now)
{
    const std::uint32_t Step = Now.Number / 2;
    const std::uint32_t Mix = Now.Mix();
    const std::uint32_t Bands = Hash(Step * 7919u) % (2u + 6u * Mix / Whole);
    const std::uint32_t Tallest = std::max<std::uint32_t>(1, Now.Rows / 7);
    const std::uint32_t Farthest = std::max<std::uint32_t>(2, Now.Cols / 5);
    for (std::uint32_t Band = 0; Band < Bands; Band++) {
        const std::uint32_t Roll = Hash(Step * 31u + Band * 97u + 5u);
        const std::uint32_t From = Roll % Now.Rows;
        const std::uint32_t Height = 1 + (Roll >> 8) % Tallest;
        const std::int32_t Away = static_cast<std::int32_t>(1 + (Roll >> 16) % Farthest) * (Roll & 0x80000000u ? -1 : 1);
        const std::int32_t Apart = static_cast<std::int32_t>(1 + (Roll >> 24) % 3u);
        for (std::uint32_t Y = From; Y < std::min(Now.Rows, From + Height); Y++) {
            Out.RowShift[Y] += Away * static_cast<std::int32_t>(Mix) / static_cast<std::int32_t>(Whole);
            Out.Split[Y] = Apart;
        }
    }
}

void PlanMelt(Warp &Out, const Moment &Now)
{
    const std::uint32_t Turn = static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 2100u);
    const std::uint32_t Mix = Now.Mix();
    for (std::uint32_t X = 0; X < Now.Cols; X++) {
        const std::uint32_t Speed = 70u + Hash(X * 2246822519u + 3u) % 186u;
        const std::int32_t Surge = 256 + Sine(Turn + X * 91u) / 8;
        const std::uint32_t Reach = Now.Rows * Mix / Whole * Speed / Whole * static_cast<std::uint32_t>(Surge) / Whole / 3;
        Out.Drip[X] = static_cast<std::int32_t>(std::min(Reach, Now.Rows - 1));
    }
}

void PlanShake(Warp &Out, const Moment &Now)
{
    const std::int32_t Mix = static_cast<std::int32_t>(Now.Mix());
    const std::int32_t WidestX = (1 + static_cast<std::int32_t>(Now.Cols) / 40) * Mix / static_cast<std::int32_t>(Whole);
    const std::int32_t WidestY = (1 + static_cast<std::int32_t>(Now.Rows) / 25) * Mix / static_cast<std::int32_t>(Whole);
    const std::uint32_t Roll = Hash(Now.Number * 2654435761u + 17u);
    if (Now.Axes & DISPLAY_EFFECT_AXIS_X)
        Out.ShakeX = static_cast<std::int32_t>(Roll % static_cast<std::uint32_t>(2 * WidestX + 1)) - WidestX;
    if (Now.Axes & DISPLAY_EFFECT_AXIS_Y)
        Out.ShakeY = static_cast<std::int32_t>((Roll >> 12) % static_cast<std::uint32_t>(2 * WidestY + 1)) - WidestY;
}

// The rings: a table, by the distance from the middle of the glass in
// sixteenths of a pixel, of how far the wave moves a cell there, and one of
// the direction's reciprocal.
void PlanRipple(Warp &Out, const Moment &Now)
{
    const std::int32_t Cols = static_cast<std::int32_t>(Now.Cols), Rows = static_cast<std::int32_t>(Now.Rows);
    const auto Farthest = static_cast<std::size_t>(Root(static_cast<std::uint32_t>(Cols * Cols / 4 + Rows * Rows)) * 16 + 32);
    const std::int32_t Mix = static_cast<std::int32_t>(Now.Mix());
    const std::int32_t Tall = (1 + Cols / 60) * 16 * Mix / static_cast<std::int32_t>(Whole);
    const std::uint32_t Phase = static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 1100u);
    Out.Rippling = true;
    Out.RingHeight.resize(Farthest);
    Out.RingAim.resize(Farthest);
    for (std::size_t Far16 = 0; Far16 < Farthest; Far16++) {
        const auto Away = static_cast<std::uint32_t>(Far16);
        const std::int32_t Fading = 256 - static_cast<std::int32_t>(std::min<std::uint32_t>(255, Away * 256u / (12u * Now.Cols)));
        Out.RingHeight[Far16] = Sine(Away * 32u / 9u - Phase) * Tall / 1024 * Fading / 256;
        Out.RingAim[Far16] = Far16 == 0 ? 0 : (1 << 22) / static_cast<std::int32_t>(Far16);
    }
}

// The picture seen through the rings: a cell comes from where the wave
// moved it, outward and inward from the middle of the glass.
void Ripple(const Warp &Plan, const Moment &Now, std::int32_t X, std::int32_t Y, std::int32_t &FromX,
            std::int32_t &FromY) noexcept
{
    const std::int32_t DeltaX = X - static_cast<std::int32_t>(Now.Cols) / 2;
    const std::int32_t DeltaY = (Y - static_cast<std::int32_t>(Now.Rows) / 2) * 2;
    const std::uint32_t Far16 = Root(static_cast<std::uint32_t>(DeltaX * DeltaX + DeltaY * DeltaY) * 256u);
    const std::int32_t Height = Plan.RingHeight[Far16];
    const std::int32_t Aim = Plan.RingAim[Far16];
    const std::int32_t AlongX = DeltaX * Aim >> 6;
    const std::int32_t AlongY = DeltaY * Aim >> 6;
    FromX -= (Height * AlongX + (Height * AlongX >= 0 ? 1 : -1) * 2048 * 16) / (4096 * 16);
    FromY -= (Height * AlongY + (Height * AlongY >= 0 ? 1 : -1) * 2048 * 32) / (4096 * 32);
}

}  // namespace

void WarpPlan(Warp &Out, const Moment &Now)
{
    const std::uint32_t Kinds = Now.Kinds;
    const std::int32_t Cols = static_cast<std::int32_t>(Now.Cols), Rows = static_cast<std::int32_t>(Now.Rows);
    Out.RowShift.assign(Now.Rows, 0);
    Out.ColumnShift.assign(Now.Cols, 0);
    Out.Drip.assign(Now.Cols, 0);
    Out.Split.assign(Now.Rows, 0);
    Out.ShakeX = Out.ShakeY = 0;
    Out.Rippling = false;

    const std::int32_t Mix = static_cast<std::int32_t>(Now.Mix());
    const bool MirrorX = (Kinds & DISPLAY_EFFECT_MIRROR) && (Now.Axes & DISPLAY_EFFECT_AXIS_X);
    const bool MirrorY = (Kinds & DISPLAY_EFFECT_MIRROR) && (Now.Axes & DISPLAY_EFFECT_AXIS_Y);
    std::int32_t Zoom = 256;
    std::int32_t BlockWidth = 1;
    if (Kinds & DISPLAY_EFFECT_PULSE) {
        const std::int32_t Breath = 1024 + Sine(static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 1900u));
        Zoom = 256 + Breath * Mix / (static_cast<std::int32_t>(Whole) * 2048 / 64);
    }
    if (Kinds & DISPLAY_EFFECT_MOSAIC) {
        const std::int32_t Breath = 1024 + Sine(static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 2600u));
        BlockWidth = 1 + Cols / 32 * (Mix * Breath / 2048) / static_cast<std::int32_t>(Whole);
    }
    const std::int32_t BlockHeight = std::max(1, BlockWidth / 2);

    Out.ColumnBase.resize(Now.Cols);
    Out.RowBase.resize(Now.Rows);
    for (std::int32_t X = 0; X < Cols; X++) {
        std::int32_t From = MirrorX ? Cols - 1 - X : X;
        if (Zoom != 256) From = Clamp(Cols / 2 + (From - Cols / 2) * 256 / Zoom, Cols);
        if (BlockWidth > 1) From = Clamp(From - From % BlockWidth + BlockWidth / 2, Cols);
        Out.ColumnBase[static_cast<std::size_t>(X)] = From;
    }
    for (std::int32_t Y = 0; Y < Rows; Y++) {
        std::int32_t From = MirrorY ? Rows - 1 - Y : Y;
        if (Zoom != 256) From = Clamp(Rows / 2 + (From - Rows / 2) * 256 / Zoom, Rows);
        if (BlockHeight > 1) From = Clamp(From - From % BlockHeight + BlockHeight / 2, Rows);
        Out.RowBase[static_cast<std::size_t>(Y)] = From;
    }

    if (Kinds & DISPLAY_EFFECT_SWAY) PlanSway(Out, Now);
    if (Kinds & DISPLAY_EFFECT_GLITCH) PlanGlitch(Out, Now);
    if (Kinds & DISPLAY_EFFECT_MELT) PlanMelt(Out, Now);
    if (Kinds & DISPLAY_EFFECT_QUAKE) PlanShake(Out, Now);
    if (Kinds & DISPLAY_EFFECT_RIPPLE) PlanRipple(Out, Now);
}

Source WarpSource(const Warp &Plan, const Moment &Now, std::int32_t X, std::int32_t Y) noexcept
{
    const std::int32_t Cols = static_cast<std::int32_t>(Now.Cols), Rows = static_cast<std::int32_t>(Now.Rows);
    const auto Column = static_cast<std::size_t>(X);
    const auto Row = static_cast<std::size_t>(Y);
    std::int32_t FromX = Plan.ColumnBase[Column];
    std::int32_t FromY = Plan.RowBase[Row];

    if (Plan.Rippling) {
        Ripple(Plan, Now, X, Y, FromX, FromY);
        FromX = Clamp(FromX, Cols);
        FromY = Clamp(FromY, Rows);
    }
    FromX = Wrap(FromX - Plan.RowShift[Row] - Plan.ShakeX, Cols);
    FromY = Wrap(FromY - Plan.ColumnShift[Column] - Plan.ShakeY, Rows);
    FromY = std::max(0, FromY - Plan.Drip[Column]);
    return Source{FromX, FromY};
}

}  // namespace effects
