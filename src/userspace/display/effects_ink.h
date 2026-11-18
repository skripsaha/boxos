#ifndef BOXOS_DISPLAY_EFFECTS_INK_H
#define BOXOS_DISPLAY_EFFECTS_INK_H

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "box/display.h"
#include "box/items.h"
#include "effects_math.h"

// The effects that change what a place looks like and not where it is taken
// from: the colours' grades, the light, the sparkles and the falling snow.
// Worked out once for a frame (InkPlan), then asked place by place — a cell
// of a text page, or a framebuffer's pixel.

namespace effects {

// The turn of the hues in one column of the rainbow: a rotation about grey
// (a circulant matrix, in 1024ths, mixed with the identity by how far the
// effect has swelled) and how much of each primary the glass over it lets by.
struct Hue {
    std::int32_t  Diagonal = 1024;
    std::int32_t  Plus = 0;
    std::int32_t  Minus = 0;
    std::uint32_t Red = Whole, Green = Whole, Blue = Whole;
};

struct Ink {
    std::uint32_t Fade = 0;             // mixes, 0..Whole
    std::uint32_t Invert = 0;
    std::uint32_t TintMix = 0;
    std::uint32_t TintColor = 0;
    std::uint32_t Sepia = 0;
    bool          Spot = false;
    std::uint32_t Dark = Whole;         // what is outside the spotlight
    std::int32_t  SpotX = 0, SpotY = 0;
    std::uint32_t SpotNear = 0;         // the squares of the radius that is all light, and of the one where the dark begins
    std::uint32_t SpotFar = 0;
    std::int32_t  SpotRadius = 0, SpotEdge = 1;
    bool          Film = false;
    std::uint32_t FilmScale = 0;        // a place's squared distance from the middle times this, over 65536, is its place in the vignette
    std::int32_t  Scratch[2] = {-1, -1};
    std::int32_t  ScratchWide = 1;      // places across a scratch
    std::uint32_t Sparkle = 0;          // thousandths of the cells lit in a frame
    std::uint32_t Star = 0;
    bool          Rainbow = false;
    bool          Global = false;       // a colour is changed the same wherever it stands
    bool          Any = false;          // whether a colour is changed at all
    std::vector<Hue> Hues;
    std::vector<std::uint16_t> Vignette;                // 0..Whole by the place in the vignette, with the flicker
    std::vector<std::uint32_t> Known, Learnt;           // colours and what Global makes of them, by their hash
};

void InkPlan(Ink &Out, const Moment &Now);

// The light a cell is seen in, 0..Whole: the spotlight's circle, the film's
// vignette and flicker.
std::uint32_t InkLight(const Ink &Plan, const Moment &Now, std::int32_t X, std::int32_t Y) noexcept;

// A colour — a triple, or COLOR_DEFAULT drawn as Own — in the column X, as the
// effect shows it in Light. The plan remembers what it made of a colour.
std::uint32_t InkColour(Ink &Plan, std::uint32_t Colour, std::uint32_t Own, std::int32_t X,
                        std::uint32_t Light) noexcept;

// A colour with a scratch of the film's down it.
std::uint32_t InkScratched(std::uint32_t Colour) noexcept;
// A colour come apart: red from Before, blue from After, green its own.
std::uint32_t InkApart(std::uint32_t Middle, std::uint32_t Before, std::uint32_t After) noexcept;

// A cell with a scratch of the film's down it.
void InkScratch(ItemCell &Cell, std::uint32_t Own) noexcept;

// The cell's colours come apart: red from the cell Left, blue from the cell
// Right, green its own.
void InkSplit(ItemCell &Cell, const ItemCell &Left, const ItemCell &Right, std::uint32_t Ink, std::uint32_t Paper) noexcept;

// A lit cell: an empty one becomes a star, a written one flashes.
void InkSparkle(ItemCell &Cell, std::uint32_t Roll, std::uint32_t Star, std::uint32_t Ink) noexcept;

// Snow falling over the picture, in the half-block pixels of the cells.
void InkSnow(std::vector<ItemCell> &Glass, const Moment &Now, std::uint32_t Ink, std::uint32_t Paper);

// The flakes of a frame's snow, each told to Each(X, Y, Colour, Weight of
// Whole) where it stands on the glass's places — a framebuffer's pixels.
template <typename Flaking>
void InkFlakes(const Moment &Now, Flaking &&Each)
{
    static constexpr std::uint32_t Speeds[2] = {13, 27};
    static constexpr std::uint32_t Weights[2] = {120, 256};
    const std::uint32_t High = Now.Rows;
    const std::uint32_t Strips = std::max<std::uint32_t>(1, Now.Cols / Now.Across);
    const std::uint32_t Density = 6u + 54u * Now.Mix() / Whole;
    const std::uint32_t Flake = Brightened(Now.Color, 200);
    for (std::uint32_t Layer = 0; Layer < 2; Layer++)
        for (std::uint32_t Strip = 0; Strip < Strips; Strip++) {
            const std::uint32_t Roll = Hash(Strip * 668265263u + Layer * 374761393u + 7u);
            if ((Roll & 255u) >= Density) continue;
            /* A half-cell a second on a text page: as many pixels here. */
            const std::uint32_t Fall = static_cast<std::uint32_t>(Now.PhaseMs * Speeds[Layer] * Now.Down / 2000u) + (Roll >> 8) % High;
            const std::int32_t  Sway = Sine(static_cast<std::uint32_t>(Now.PhaseMs * 1024u / 2600u) + (Roll >> 20) + Layer * 300u) *
                                      2 * static_cast<std::int32_t>(Now.Across) / 1024;
            const std::int32_t X = Wrap(static_cast<std::int32_t>(Strip * Now.Across + Now.Across / 2) + Sway,
                                        static_cast<std::int32_t>(Now.Cols));
            Each(X, static_cast<std::int32_t>(Fall % High), Flake, Weights[Layer]);
        }
}

}  // namespace effects

#endif
