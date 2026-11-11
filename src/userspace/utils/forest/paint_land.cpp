/* paint_land.cpp — the meadow and what grows or lies on it: grass that goes
 * gold in autumn and white under the snow, flowers in spring, mushrooms and
 * fallen leaves in autumn, bushes that are green, red with berries or bare
 * twigs under snow, rocks with moss, the fallen log, the tufts of grass. */

#include <algorithm>
#include <cmath>
#include <vector>

#include "paint.h"

namespace forest {

namespace {

const Color GoldGrass  = Color::Rgb(0xA8, 0x98, 0x48);
const Color DeadGrass  = Color::Rgb(0x8C, 0x7C, 0x50);
const Color SnowShade  = Color::Rgb(0xBC, 0xC8, 0xD8);
const Color Twig       = Color::Rgb(0x4A, 0x36, 0x24);
const Color Berry      = Color::Rgb(0xD0, 0x30, 0x38);
const Color StoneInk   = Color::Rgb(0x8E, 0x8E, 0x94);
const Color MossInk    = Color::Rgb(0x4E, 0x80, 0x3C);
const Color CapInk     = Color::Rgb(0xC8, 0x42, 0x30);
const Color StemInk    = Color::Rgb(0xEC, 0xE4, 0xD0);
const Color FlowerInks[5] = {Color::Rgb(0xF8, 0xF8, 0xEE), Color::Rgb(0xF8, 0xE0, 0x40), Color::Rgb(0xF0, 0x8C, 0xB0),
                             Color::Rgb(0x9A, 0x78, 0xE0), Color::Rgb(0xE8, 0x48, 0x48)};

int Weighted(float Value) { return static_cast<int>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 256.0f)); }

// The meadow's colour before the snow: grass, turning gold with the autumn.
Color MeadowInk(const World &Shown)
{
    const Land &Ground = Shown.Weather.Ground;
    return Mixed(Shown.Chosen.Grass, GoldGrass, Weighted(Ground.Autumn * (1.0f - Ground.Snow) * 0.6f));
}

// The colours of a row of the meadow, lit once for the row: the grass, the
// snow and its shade, and the fallen leaves.
struct RowInks {
    Color Grass, Snow, SnowShade;
    Color Leaves[4];
};

RowInks InksOfRow(const World &Shown, float Away)
{
    Color  Meadow = MeadowInk(Shown);
    float  Far    = (1.0f - Away) * 0.2f;
    RowInks Row;
    Row.Grass     = Lit(Shown, Mixed(Darker(Meadow, 36), Meadow, Weighted(0.55f + 0.45f * Away)), Far);
    Row.Snow      = Lit(Shown, Shown.Chosen.Snow, Far);
    Row.SnowShade = Lit(Shown, SnowShade, Far);
    for (int Which = 0; Which < 4; Which++) Row.Leaves[Which] = Lit(Shown, AutumnInk(Shown.Chosen, Which), Far);
    return Row;
}

}  // namespace

// The leaves of autumn, in the colours the trees turn.
Color AutumnInk(const Look &Chosen, int Which)
{
    switch (Which & 3) {
    case 0:  return Chosen.Autumn;
    case 1:  return Mixed(Chosen.Autumn, Color::Rgb(0xC0, 0x38, 0x28), 140);
    case 2:  return Mixed(Chosen.Autumn, Color::Rgb(0xF0, 0xD0, 0x40), 150);
    default: return Color::Rgb(0x8A, 0x5A, 0x2A);
    }
}

void PaintGround(const World &Shown, Canvas &Picture)
{
    const Scenery &Place  = Shown.Place;
    const Layout  &Size   = Place.Size;
    const Land    &Ground = Shown.Weather.Ground;
    const float    Depth  = static_cast<float>(std::max(1, Size.Front - Size.Ground));

    std::vector<RowInks> Rows(static_cast<std::size_t>(Picture.Height));
    for (int Y = 0; Y < Picture.Height; Y++)
        Rows[static_cast<std::size_t>(Y)] = InksOfRow(Shown, std::clamp(static_cast<float>(Y - Size.Ground) / Depth, 0.0f, 1.0f));

    for (int X = 0; X < Picture.Width; X++) {
        int Top = GroundAt(Place, X);
        for (int Y = Top; Y < Picture.Height; Y++) {
            const RowInks &Row   = Rows[static_cast<std::size_t>(Y)];
            float          Noise = Grain(X, Y, 3);
            Color          Ink   = Row.Grass;
            if (Noise < 0.2f) Ink = Darker(Ink, 26);
            else if (Noise > 0.88f) Ink = Lighter(Ink, 20);
            if (Y == Top) Ink = Lighter(Ink, 24);

            if (Ground.Litter > 0.05f && Grain(X / 3, Y / 2, 5) < Ground.Litter * 0.34f)
                Ink = Mixed(Row.Leaves[Hash(X, Y, 9) >> 5 & 3u], Ink, 60);
            if (Grain(X, Y, 11) < Ground.Snow * 0.985f)
                Ink = Mixed(Row.Snow, Row.SnowShade, Weighted(0.2f * Grain(X, Y, 13) + (Y - Size.Ground < Depth * 0.1f ? 0.25f : 0.0f)));
            Picture.At(X, Y) = Ink;
        }
    }

    const float Bloom = Ground.Flowers * (1.0f - Ground.Snow);
    for (const Flower &Each : Place.Flowers) {
        if (Each.Threshold >= Bloom) continue;
        Picture.Put(Each.X, Each.Y, Lit(Shown, FlowerInks[Each.Variant]));
        if (Each.Y < Size.Front) Picture.Put(Each.X, Each.Y + 1, Lit(Shown, Darker(Shown.Chosen.Grass, 60)));
    }
    if (Ground.Litter < 0.6f || Ground.Snow > 0.3f) return;
    for (const Flower &Each : Place.Flowers) {
        if (Each.Variant != 4 || Each.Threshold >= 0.35f) continue;
        for (int Dx = -1; Dx <= 1; Dx++) Picture.Put(Each.X + Dx, Each.Y - 1, Lit(Shown, CapInk));
        Picture.Put(Each.X, Each.Y - 1, Lit(Shown, Lighter(CapInk, 90)));
        Picture.Put(Each.X, Each.Y, Lit(Shown, StemInk));
    }
}

void PaintRock(const World &Shown, Canvas &Picture, const Rock &Stone)
{
    const Land &Ground = Shown.Weather.Ground;
    float       Rx     = static_cast<float>(Stone.Width) / 2.0f;
    float       Ry     = static_cast<float>(Stone.Height);
    for (int Dy = 0; Dy < Stone.Height; Dy++) {
        for (int Dx = 0; Dx < Stone.Width; Dx++) {
            float Nx = (static_cast<float>(Dx) + 0.5f - Rx) / Rx;
            float Ny = (static_cast<float>(Stone.Height - Dy) - 0.5f) / Ry;
            if (Nx * Nx + Ny * Ny > 1.0f) continue;
            int   X   = static_cast<int>(Stone.X) - Stone.Width / 2 + Dx;
            int   Y   = static_cast<int>(Stone.Foot) - Stone.Height + 1 + Dy;
            Color Ink = StoneInk;
            if (Ny > 0.55f && Nx < 0.3f) Ink = Lighter(Ink, 40);
            if (Nx > 0.4f) Ink = Darker(Ink, 70);
            if ((Hash(X, Y, Stone.Seed) & 7u) == 0) Ink = Darker(Ink, 40);
            if (Ny > 0.6f && (Hash(X, Y, Stone.Seed) & 3u) != 0 && Ground.Leaves > 0.2f) Ink = Mixed(Ink, MossInk, 150);
            if (Ny > 0.5f && Grain(X, Y, 17) < Ground.Snow) Ink = Mixed(Shown.Chosen.Snow, SnowShade, Nx > 0.3f ? 90 : 20);
            Picture.Put(X, Y, Lit(Shown, Ink));
        }
    }
}

void PaintBush(const World &Shown, Canvas &Picture, const Bush &Shrub)
{
    const Land &Ground = Shown.Weather.Ground;
    float       Rx     = static_cast<float>(Shrub.Width) / 2.0f;
    Color       Leaf   = Mixed(Darker(Shown.Chosen.Leaves, 34), AutumnInk(Shown.Chosen, static_cast<int>(Shrub.Seed >> 7)),
                               Weighted(Ground.Autumn * 0.85f));
    for (int Dx = 0; Dx < Shrub.Width; Dx++) {
        float Across = (static_cast<float>(Dx) + 0.5f - Rx) / Rx;
        float Top    = static_cast<float>(Shrub.Height) * std::sqrt(std::max(0.0f, 1.0f - Across * Across)) *
                    (0.82f + 0.3f * Grain(Dx, 0, Shrub.Seed));
        int X = static_cast<int>(Shrub.X) - Shrub.Width / 2 + Dx;
        for (int Rise = 0; static_cast<float>(Rise) < Top; Rise++) {
            int   Y   = static_cast<int>(Shrub.Foot) - Rise;
            bool  Cap = static_cast<float>(Rise + 1) >= Top;
            float N   = Grain(X, Y, Shrub.Seed);
            Color Ink = Leaf;
            if (N < 0.25f) Ink = Darker(Ink, 40);
            else if (N > 0.8f) Ink = Lighter(Ink, 35);
            if (Ground.Litter > 0.5f && N > 0.93f) Ink = Berry;

            bool Leafy = Grain(X, Y, Shrub.Seed ^ 21u) < Ground.Leaves * 1.15f;
            if (!Leafy) {
                if (N > 0.45f) continue;
                Ink = Twig;
            }
            if (Cap && Grain(X, Y, 23) < Ground.Snow) Ink = Shown.Chosen.Snow;
            Picture.Put(X, Y, Lit(Shown, Ink));
        }
    }
}

void PaintLog(const World &Shown, Canvas &Picture, const Log &Trunk)
{
    const Land &Ground = Shown.Weather.Ground;
    constexpr int Thick = 4;
    int Left = static_cast<int>(Trunk.X) - Trunk.Length / 2;
    for (int Dx = 0; Dx < Trunk.Length; Dx++) {
        for (int Dy = 0; Dy < Thick; Dy++) {
            int   X   = Left + Dx;
            int   Y   = static_cast<int>(Trunk.Foot) - Thick + 1 + Dy;
            Color Ink = Shown.Chosen.Bark;
            if (Dy == 0) Ink = Lighter(Ink, 30);
            if (Dy >= Thick - 1) Ink = Darker(Ink, 55);
            if (Grain(X, Y, 29) < 0.15f) Ink = Darker(Ink, 30);
            if (Dx >= Trunk.Length - 2) Ink = Mixed(Lighter(Shown.Chosen.Bark, 120), Ink, Dy == 1 || Dy == 2 ? 40 : 160);
            if (Dy == 0 && Dx < Trunk.Length - 2 && Grain(X, Y, 31) < 0.4f * Ground.Leaves) Ink = MossInk;
            if (Dy == 0 && Grain(X, Y, 33) < Ground.Snow) Ink = Shown.Chosen.Snow;
            Picture.Put(X, Y, Lit(Shown, Ink));
        }
    }
    if (Ground.Snow > 0.3f)
        for (int Dx = 1; Dx < Trunk.Length - 2; Dx++)
            if (Grain(Left + Dx, 0, 35) < Ground.Snow) Picture.Put(Left + Dx, static_cast<int>(Trunk.Foot) - Thick, Lit(Shown, Shown.Chosen.Snow));
}

void PaintTuft(const World &Shown, Canvas &Picture, const Tuft &Grass)
{
    const Land &Ground = Shown.Weather.Ground;
    const Air  &Aloft  = Shown.Weather.Aloft;
    Color Blade = Mixed(Shown.Chosen.Grass, Grass.Variant == 0 ? Darker(Shown.Chosen.Grass, 70) : Lighter(Shown.Chosen.Grass, 50),
                        Grass.Variant == 1 ? 0 : 130);
    Blade       = Mixed(Blade, DeadGrass, Weighted(Ground.Autumn * 0.6f));
    Blade       = Mixed(Blade, Shown.Chosen.Snow, Weighted(Ground.Snow * 0.9f));
    float Lean  = Wave(Shown.Weather.Clock * 0.45f + Grass.X * 0.07f) * (0.35f + 1.3f * Aloft.Wind) * 1.4f;
    for (int Blades = -1; Blades <= 1; Blades++) {
        int Long = Blades == 0 ? Grass.Height : Grass.Height - 1;
        for (int Rise = 0; Rise < Long; Rise++) {
            float Up = static_cast<float>(Rise) / static_cast<float>(Grass.Height);
            int   X  = static_cast<int>(Grass.X) + Blades + static_cast<int>(std::lround(Lean * Up + static_cast<float>(Blades) * Up));
            Color Ink = Rise + 1 == Long && Ground.Snow > 0.5f ? Shown.Chosen.Snow : Mixed(Darker(Blade, 40), Blade, Weighted(0.4f + Up));
            Picture.Put(X, static_cast<int>(Grass.Foot) - Rise, Lit(Shown, Ink));
        }
    }
}

}  // namespace forest
