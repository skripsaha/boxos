#ifndef FLAME_H
#define FLAME_H

/* flame — fire on the console, burning from where it is told to.
 *
 * Six parts. flame_sim.cpp is the heat, flame_palette.cpp turns heat into
 * colour, flame_font.cpp draws a word as lit pixels; these three take nothing
 * of BoxOS but its colours, so they run anywhere and what they make can be
 * looked at. The glass is flame_paint.cpp, the menu and keys flame_menu.cpp,
 * and flame.cpp decides: the Form, the clock and the loop.
 *
 * A fire is a grid of heat, one pixel for every half of a cell: two pixels
 * stand in a cell's height. The simulation keeps raw RGB palette values; the
 * Painter adapter turns them into frame cells at the edge.
 */

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "painter/core/color.h"
#include "painter/tui/frame.h"
#include "painter/tui/stage.h"

enum class FlamePlace : std::uint8_t { Bottom, Top, Sides, Frame, Pit, Word };
enum class FlameInk : std::uint8_t { Fire, Gas, Toxic, Spirit, Custom };

constexpr int FlamePlaceCount = 6;
constexpr int FlameInkCount   = 5;
constexpr int FlameHeightMin  = 1;
constexpr int FlameHeightMax  = 10;
constexpr int FlameWindMax    = 3;
constexpr std::size_t FlameWordMax = 32;

struct FlameSettings {
    FlamePlace    Place     = FlamePlace::Bottom;
    FlameInk      Ink       = FlameInk::Fire;
    std::uint32_t Custom    = 0xFF6A00;
    bool          HasCustom = false;
    int           Height    = 5;
    int           Wind      = 0;
    bool          Sparks    = false;
    std::string   Word;
};

const char *FlamePlaceName(FlamePlace Place);
const char *FlameInkName(FlameInk Ink);

using FlamePalette = std::array<std::uint32_t, 256>;

FlamePalette FlameMakePalette(FlameInk Ink, std::uint32_t Custom);

/* The word as lit pixels on a Width x Height grid, centred, as big as fits. */
std::vector<std::uint8_t> FlameWordPixels(const std::string &Word, int Width, int Height);

struct FlameStepDir {
    std::int8_t X = 0;
    std::int8_t Y = 0;
};

/* A source cell sparks may leave from, and the way away from the source. */
struct FlameSpawn {
    int          X;
    int          Y;
    FlameStepDir Away;
};

/* The body of fire: its sources and the way heat is pulled. Either a placement
 * or a word burns, never both: a word is fire that rises from its letters. */
struct FlameLayer {
    std::vector<std::uint8_t>  Heat, Next, Source, Cool;
    std::vector<FlameStepDir>  Pull;
    std::vector<FlameSpawn>    Spawns;
    int                        Extent      = 1;
    int                        SparkChance = 10;
    int                        Surge       = 140;
};

struct FlameSpark {
    float X, Y, Vx, Vy, Life, Span;
};

class FlameFire {
public:
    static constexpr int TurbulenceSide = 256;
    static constexpr unsigned TickUs = 25000;

    FlameFire(int Width, int Height, std::uint64_t Seed);

    void Rebuild(const FlameSettings &Settings);
    void Step(const FlameSettings &Settings);

    int          Width() const { return Wide; }
    int          Height() const { return High; }
    std::uint8_t At(int X, int Y) const { return Out[static_cast<std::size_t>(Y) * Wide + X]; }

private:
    std::uint64_t Roll();
    FlameLayer    PlaceLayer(FlamePlace Place);
    FlameLayer    WordLayer(const std::string &Word);
    FlameLayer    EmptyLayer(int Extent);
    void          MakeTurbulence();
    int           Turbulence(FlameStepDir Pull, int X, int Y) const;
    void          StepLayer(FlameLayer &Layer, const FlameSettings &Settings);
    void          StepSparks(const FlameSettings &Settings);
    void          Spawn(const FlameLayer &Layer, int Chance);

    int                         Wide;
    int                         High;
    std::uint64_t               Seed;
    FlameLayer                  Body;
    std::vector<FlameSpark>     Sparks;
    std::vector<std::uint8_t>   Out;
    std::vector<std::uint8_t>   Noise;
    unsigned                    Clock = 0;
};

struct FlameOutcome {
    bool Leave     = false;
    bool Rebuild   = false;
    bool Repalette = false;
};

class FlameMenu {
public:
    bool Open = false;

    FlameOutcome OnKey(const painter::tui::KeyEvent &Key, FlameSettings &Settings);
    void         Draw(painter::tui::Frame &Frame, int Rows, int Cols, const FlameSettings &Settings) const;
    std::optional<painter::core::Point> Caret(int Rows, int Cols) const;

private:
    FlameOutcome Quick(const painter::tui::KeyEvent &Key, FlameSettings &Settings);
    FlameOutcome Choose(const painter::tui::KeyEvent &Key, FlameSettings &Settings);
    FlameOutcome Type(const painter::tui::KeyEvent &Key, FlameSettings &Settings);
    FlameOutcome Apply(int Row, int Delta, FlameSettings &Settings);
    FlameOutcome SetPlace(FlamePlace Place, FlameSettings &Settings);
    FlameOutcome Accept(FlameSettings &Settings);
    void         BeginEdit(const FlameSettings &Settings);

    bool        Editing = false;
    int         Item    = 0;
    const char *Note    = nullptr;
    std::string Text;
};

constexpr int FlameMinCols = 56;
constexpr int FlameMinRows = 14;

/* Two pixels to a cell: the upper half block, ink the pixel above, paper the
 * pixel below. */
void FlamePaintFire(const FlameFire &Fire, const FlamePalette &Palette, painter::tui::Frame &Frame, int Rows,
                    int Cols);

#endif
