#ifndef BOXOS_DISPLAY_EFFECTS_INK_H
#define BOXOS_DISPLAY_EFFECTS_INK_H

#include <cstdint>
#include <span>
#include <vector>

#include "easel.h"
#include "effects_math.h"

// The effects that change what a cell looks like and not where it is taken
// from: the colours' grades, the light, the sparkles and the falling snow.
// Worked out once for a frame (InkPlan), then asked cell by cell.

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

// A cell with a scratch of the film's down it.
void InkScratch(GlassCell &Cell, std::uint32_t Own) noexcept;

// The cell's colours come apart: red from the cell Left, blue from the cell
// Right, green its own.
void InkSplit(GlassCell &Cell, const GlassCell &Left, const GlassCell &Right, std::uint32_t Ink,
              std::uint32_t Paper) noexcept;

// A lit cell: an empty one becomes a star, a written one flashes.
void InkSparkle(GlassCell &Cell, std::uint32_t Roll, std::uint32_t Star, std::uint32_t Ink) noexcept;

// Snow falling over the picture, in the half-block pixels of the cells.
void InkSnow(std::vector<GlassCell> &Glass, const Moment &Now, std::uint32_t Ink, std::uint32_t Paper);

}  // namespace effects

#endif
