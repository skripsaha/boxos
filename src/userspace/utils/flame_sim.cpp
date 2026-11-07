/* flame_sim.cpp — the heat.
 *
 * Every cell holds a heat 0..255. A cell that is not a source takes its heat
 * from the cell one step toward its source (the "pull"), blurred 1-6-1 across
 * the pull so a flame has a body rather than a column of noise, leaned by the
 * wind, and cooled by a random amount. Pulling from the source side is what
 * makes the same rule burn up from the bottom, down from the top, inward from
 * the sides and the frame: only the map of pulls changes.
 *
 * A source does not hold still. Its heat follows a slow field of noise along
 * the edge, and that is what makes tongues of different height: a flame fed
 * by a cooler source dies sooner.
 *
 * Only one body burns: the placement, or the word when `where` is word.
 *
 * Time is counted in fixed ticks (TickUs), never in frames: the caller steps
 * as many ticks as the clock owes, so the fire moves as fast on a slow
 * machine as on a quick one.
 */

#include <algorithm>
#include <cstdlib>

#include "flame.h"

namespace {

constexpr int   SourceDepth = 2;
constexpr int   SourceHot   = 255;
constexpr int   NormalCool  = 16;
constexpr int   ReachShare  = 6;
constexpr float TickSeconds = FlameFire::TickUs / 1000000.0f;

FlameStepDir Opposite(FlameStepDir Dir)
{
    return {static_cast<std::int8_t>(-Dir.X), static_cast<std::int8_t>(-Dir.Y)};
}

int Clamp(int Value, int Low, int High)
{
    return Value < Low ? Low : (Value > High ? High : Value);
}

}

FlameFire::FlameFire(int Width, int Height, std::uint64_t Seed)
    : Wide(Width), High(Height), Seed(Seed | 1u), Out(static_cast<std::size_t>(Width) * Height, 0)
{
    MakeTurbulence();
}

/* Cooling is not white noise: a flame has tongues, and a tongue is a stretch
 * of cooler air that rides up with the heat. This is that air as a 256x256
 * field of random bytes, blurred wide across and long along the flow, then
 * stretched back to the full range the blur took away. */
void FlameFire::MakeTurbulence()
{
    constexpr int Side = TurbulenceSide;
    std::vector<std::uint8_t> Scratch(Side * Side);
    Noise.resize(Side * Side);
    for (std::uint8_t &Byte : Noise) Byte = static_cast<std::uint8_t>(Roll() >> 24);

    auto Blur = [&](int Radius, bool Along) {
        for (int Row = 0; Row < Side; Row++) {
            for (int Col = 0; Col < Side; Col++) {
                int Total = 0;
                for (int Step = -Radius; Step <= Radius; Step++) {
                    int Y = Along ? (Row + Step) & (Side - 1) : Row;
                    int X = Along ? Col : (Col + Step) & (Side - 1);
                    Total += Noise[static_cast<std::size_t>(Y) * Side + X];
                }
                Scratch[static_cast<std::size_t>(Row) * Side + Col] = static_cast<std::uint8_t>(Total / (2 * Radius + 1));
            }
        }
        Noise.swap(Scratch);
    };
    for (int Pass = 0; Pass < 3; Pass++) Blur(2, false);
    for (int Pass = 0; Pass < 2; Pass++) Blur(5, true);

    auto [Lowest, Highest] = std::minmax_element(Noise.begin(), Noise.end());
    int Low  = *Lowest;
    int Span = std::max(*Highest - Low, 1);
    for (std::uint8_t &Byte : Noise) Byte = static_cast<std::uint8_t>((Byte - Low) * 255 / Span);
}

int FlameFire::Turbulence(FlameStepDir Pull, int X, int Y) const
{
    int Phase  = static_cast<int>(Clock * 3 / 4);
    int Along  = Pull.Y != 0 ? Y + Pull.Y * Phase : X + Pull.X * Phase;
    int Across = Pull.Y != 0 ? X : Y;
    return Noise[static_cast<std::size_t>(Along & (TurbulenceSide - 1)) * TurbulenceSide + (Across & (TurbulenceSide - 1))];
}

std::uint64_t FlameFire::Roll()
{
    Seed ^= Seed << 13;
    Seed ^= Seed >> 7;
    Seed ^= Seed << 17;
    return Seed * 0x2545F4914F6CDD1Dull;
}

FlameLayer FlameFire::EmptyLayer(int Extent)
{
    std::size_t Cells = static_cast<std::size_t>(Wide) * High;
    FlameLayer  Layer;
    Layer.Heat.assign(Cells, 0);
    Layer.Next.assign(Cells, 0);
    Layer.Source.assign(Cells, 0);
    Layer.Cool.assign(Cells, NormalCool);
    Layer.Pull.assign(Cells, FlameStepDir{});
    Layer.Extent = std::max(Extent, 1);
    return Layer;
}

FlameLayer FlameFire::PlaceLayer(FlamePlace Place)
{
    const int W = Wide;
    const int H = High;

    int Extent = H;
    if (Place == FlamePlace::Sides) Extent = W / 3;
    if (Place == FlamePlace::Frame) Extent = std::min(W, H) / 3;
    if (Place == FlamePlace::Pit)   Extent = H * 3 / 2;
    FlameLayer Layer = EmptyLayer(Extent);

    auto Fire = [&](int X, int Y, int Value, FlameStepDir Away, bool Spark) {
        Layer.Source[static_cast<std::size_t>(Y) * W + X] = static_cast<std::uint8_t>(Value);
        if (Spark) Layer.Spawns.push_back({X, Y, Away});
    };

    for (int Y = 0; Y < H; Y++) {
        for (int X = 0; X < W; X++) {
            std::size_t Cell = static_cast<std::size_t>(Y) * W + X;
            switch (Place) {
            case FlamePlace::Bottom:
            case FlamePlace::Pit: Layer.Pull[Cell] = {0, 1}; break;
            case FlamePlace::Top: Layer.Pull[Cell] = {0, -1}; break;
            case FlamePlace::Sides:
                Layer.Pull[Cell] = X < W / 2 ? FlameStepDir{-1, 0} : FlameStepDir{1, 0};
                break;
            case FlamePlace::Frame: {
                int Left = X, Right = W - 1 - X, Above = Y, Below = H - 1 - Y;
                int Near = std::min(std::min(Left, Right), std::min(Above, Below));
                if (Near == Above)      Layer.Pull[Cell] = {0, -1};
                else if (Near == Below) Layer.Pull[Cell] = {0, 1};
                else if (Near == Left)  Layer.Pull[Cell] = {-1, 0};
                else                    Layer.Pull[Cell] = {1, 0};
                if (Near < SourceDepth) Fire(X, Y, SourceHot, Opposite(Layer.Pull[Cell]), Near == SourceDepth - 1);
                break;
            }
            case FlamePlace::Word: break;
            }
        }
    }

    for (int Depth = 0; Depth < SourceDepth; Depth++) {
        bool Spark = Depth == SourceDepth - 1;
        for (int X = 0; X < W && (Place == FlamePlace::Bottom || Place == FlamePlace::Top); X++)
            Fire(X, Place == FlamePlace::Bottom ? H - 1 - Depth : Depth, SourceHot,
                 {0, static_cast<std::int8_t>(Place == FlamePlace::Bottom ? -1 : 1)}, Spark);
        for (int Y = 0; Y < H && Place == FlamePlace::Sides; Y++) {
            Fire(Depth, Y, SourceHot, {1, 0}, Spark);
            Fire(W - 1 - Depth, Y, SourceHot, {-1, 0}, Spark);
        }
    }

    if (Place == FlamePlace::Pit) {
        const int Centre = W / 2;
        const int Half   = std::max(5, W / 5);
        for (int X = 0; X < W; X++) {
            int Across = std::abs(X - Centre);
            int Weight = NormalCool + 32 * std::min(Across, Half) / Half;
            if (Across > Half) Weight += (Across - Half) * 24;
            for (int Y = 0; Y < H; Y++) Layer.Cool[static_cast<std::size_t>(Y) * W + X] = static_cast<std::uint8_t>(std::min(Weight, 255));
            if (Across > Half) continue;
            int Value = SourceHot - 150 * Across * Across / (Half * Half);
            for (int Depth = 0; Depth < 3; Depth++) Fire(X, H - 1 - Depth, Value, {0, -1}, Depth == 2);
        }
        Layer.SparkChance = 28;
    }
    return Layer;
}

FlameLayer FlameFire::WordLayer(const std::string &Word)
{
    const int W = Wide;
    const int H = High;

    FlameLayer                Layer  = EmptyLayer(H / 4);
    Layer.Surge                      = 45;
    std::vector<std::uint8_t> Pixels = FlameWordPixels(Word, W, H);
    std::fill(Layer.Pull.begin(), Layer.Pull.end(), FlameStepDir{0, 1});
    for (int Y = 0; Y < H; Y++) {
        for (int X = 0; X < W; X++) {
            std::size_t Cell = static_cast<std::size_t>(Y) * W + X;
            if (!Pixels[Cell]) continue;
            Layer.Source[Cell] = SourceHot;
            if (Y == 0 || !Pixels[Cell - W]) Layer.Spawns.push_back({X, Y, {0, -1}});
        }
    }
    return Layer;
}

void FlameFire::Rebuild(const FlameSettings &Settings)
{
    Sparks.clear();
    std::fill(Out.begin(), Out.end(), 0);
    Body = Settings.Place == FlamePlace::Word ? WordLayer(Settings.Word) : PlaceLayer(Settings.Place);
}

void FlameFire::StepLayer(FlameLayer &Layer, const FlameSettings &Settings)
{
    const int W = Wide;
    const int H = High;

    int Reach     = std::max(4, Layer.Extent * Settings.Height / ReachShare);
    int CoolAvg   = 255 * 16 / Reach;
    int WindPush  = std::abs(Settings.Wind) * 35;
    int WindSign  = Settings.Wind > 0 ? -1 : 1;

    auto Sample = [&](int X, int Y) -> int {
        return Layer.Heat[static_cast<std::size_t>(Clamp(Y, 0, H - 1)) * W + Clamp(X, 0, W - 1)];
    };

    for (int Y = 0; Y < H; Y++) {
        for (int X = 0; X < W; X++) {
            std::size_t   Cell = static_cast<std::size_t>(Y) * W + X;
            std::uint64_t Dice = Roll();

            if (Layer.Source[Cell]) {
                int Surge = Noise[static_cast<std::size_t>((Clock >> 2) & (TurbulenceSide - 1)) * TurbulenceSide +
                                  ((X + Y) & (TurbulenceSide - 1))];
                int Hot   = Layer.Source[Cell] - (Surge * Layer.Surge >> 8) - static_cast<int>((Dice >> 16) & 7);
                Layer.Next[Cell] = static_cast<std::uint8_t>(std::max(Hot, 0));
                continue;
            }

            FlameStepDir Pull = Layer.Pull[Cell];
            if (Pull.X == 0 && Pull.Y == 0) {
                Layer.Next[Cell] = 0;
                continue;
            }

            int AcrossX = Pull.Y != 0 ? 1 : 0;
            int AcrossY = Pull.Y != 0 ? 0 : 1;
            int Lean    = (WindPush + static_cast<int>(((Dice & 0x7F) * 100) >> 7)) / 100;
            int Flicker = static_cast<int>((Dice >> 7) & 7);
            int Slip    = WindSign * Lean + (Flicker == 0 ? -1 : (Flicker == 1 ? 1 : 0));
            int FromX   = X + Pull.X + AcrossX * Slip;
            int FromY   = Y + Pull.Y + AcrossY * Slip;

            int Blend = Sample(FromX - AcrossX, FromY - AcrossY) + 6 * Sample(FromX, FromY) +
                        Sample(FromX + AcrossX, FromY + AcrossY);
            int Heat  = Blend >> 3;

            int Cool   = CoolAvg * Layer.Cool[Cell] >> 4;
            int Scaled = Cool * (Turbulence(Pull, X, Y) + 24) / 152;
            int Draw   = static_cast<int>((((Dice >> 20) & 0xFFFF) * static_cast<std::uint64_t>(Scaled / 2 + 1)) >> 16);
            int Cut    = (Scaled * 3 / 4 + Draw + static_cast<int>((Dice >> 36) & 15)) >> 4;
            Layer.Next[Cell] = static_cast<std::uint8_t>(Heat > Cut ? Heat - Cut : 0);
        }
    }
    Layer.Heat.swap(Layer.Next);
}

void FlameFire::Spawn(const FlameLayer &Layer, int Chance)
{
    if (Layer.Spawns.empty() || Sparks.size() >= static_cast<std::size_t>(std::max(8, Wide))) return;
    std::uint64_t Dice = Roll();
    if (static_cast<int>(Dice % 100) >= Chance) return;

    const FlameSpawn &From  = Layer.Spawns[(Dice >> 16) % Layer.Spawns.size()];
    float Speed = 18.0f + static_cast<float>((Dice >> 32) & 63) * 0.7f;
    float Side  = (static_cast<float>((Dice >> 40) & 63) - 32.0f) * 0.5f;
    float Span  = 0.7f + static_cast<float>((Dice >> 48) & 63) / 64.0f * 1.3f;

    Sparks.push_back({static_cast<float>(From.X) + 0.5f, static_cast<float>(From.Y) + 0.5f,
                      From.Away.X * Speed + From.Away.Y * Side,
                      From.Away.Y * Speed + From.Away.X * Side, Span, Span});
}

void FlameFire::StepSparks(const FlameSettings &Settings)
{
    float Push = static_cast<float>(Settings.Wind) * 8.0f * TickSeconds;
    std::size_t Kept = 0;
    for (FlameSpark Spark : Sparks) {
        float Flutter = (static_cast<float>(Roll() & 31) - 15.5f) * 3.0f * TickSeconds;
        Spark.X    += Spark.Vx * TickSeconds;
        Spark.Y    += Spark.Vy * TickSeconds;
        Spark.Vx   += Push + Flutter;
        Spark.Vx   -= Spark.Vx * 0.5f * TickSeconds;
        Spark.Vy   -= Spark.Vy * 0.5f * TickSeconds;
        Spark.Life -= TickSeconds;
        bool Inside = Spark.X >= 0 && Spark.Y >= 0 && Spark.X < static_cast<float>(Wide) && Spark.Y < static_cast<float>(High);
        if (Spark.Life > 0 && Inside) Sparks[Kept++] = Spark;
    }
    Sparks.resize(Kept);
}

void FlameFire::Step(const FlameSettings &Settings)
{
    StepLayer(Body, Settings);
    Clock++;

    if (Settings.Sparks) Spawn(Body, Body.SparkChance);
    StepSparks(Settings);
    if (!Settings.Sparks) Sparks.clear();

    Out = Body.Heat;

    for (const FlameSpark &Spark : Sparks) {
        auto Cell = static_cast<std::size_t>(static_cast<int>(Spark.Y)) * Wide + static_cast<int>(Spark.X);
        auto Heat = static_cast<std::uint8_t>(255.0f * Spark.Life / Spark.Span);
        Out[Cell] = std::max(Out[Cell], Heat);
    }
}
