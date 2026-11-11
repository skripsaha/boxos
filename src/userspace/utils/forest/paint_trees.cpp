/* paint_trees.cpp — the trees: oaks and birches drawn bough by bough with their
 * crowns over them, so what the wind and the season take from the crown leaves
 * a believable bare tree; spruces drawn tier by tier. The crown fills as
 * Land.Leaves does, turns as Land.Autumn does, flowers in spring on the trees
 * that flower, and the snow lies on what faces up. */

#include <algorithm>
#include <cmath>

#include "paint.h"
#include "sway.h"

namespace forest {

namespace {

const Color SpringInk  = Color::Rgb(0xA4, 0xD0, 0x5C);
const Color YellowLeaf = Color::Rgb(0x78, 0xA0, 0x30);
const Color BirchBark  = Color::Rgb(0xE6, 0xE2, 0xD8);
const Color SpruceInk  = Color::Rgb(0x2A, 0x58, 0x3A);
const Color BirchMark  = Color::Rgb(0x30, 0x2C, 0x28);
const Color Shadow     = Color::Rgb(0x08, 0x10, 0x08);

int Weighted(float Value) { return static_cast<int>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 256.0f)); }

// How far from the eye the layer is, for the haze.
float Distance(int Layer) { return Layer == 0 ? 0.62f : Layer == 1 ? 0.2f : 0.0f; }

// A pen over one tree: where its parts fall on the canvas, and in what light.
struct Pen {
    const World &Shown;
    Canvas      &Picture;
    const Tree  &Plant;
    float        Haze;
    std::uint32_t Salt;
    Color        Bark;          // the colours the tree is lit in, worked out once
    Color        Mark;
    Color        Snow;
    Color        Blossom;

    Pen(const World &Seen, Canvas &On, const Tree &Of)
        : Shown(Seen), Picture(On), Plant(Of), Haze(Distance(Of.Layer)), Salt(static_cast<std::uint32_t>(Of.Phase * 997.0f)),
          Bark(Dye(Of.Kind == TreeKind::Birch ? BirchBark : Seen.Chosen.Bark)), Mark(Dye(BirchMark)), Snow(Dye(Seen.Chosen.Snow)),
          Blossom(Dye(Seen.Chosen.Blossom))
    {
    }

    Color Dye(Color Ink) const { return Lit(Shown, Ink, Haze); }

    int AcrossAt(float Across, float Up) const
    {
        return static_cast<int>(std::lround(Plant.X + Across + Sway(Shown.Weather, Plant, Up)));
    }
    int RowAt(float Up) const { return static_cast<int>(std::lround(Plant.Foot - Up)); }

    void Put(int X, int Y, Color Ink) { Picture.Put(X, Y, Ink); }
    float SnowCover() const { return std::min(1.0f, Shown.Weather.Ground.Snow + Shown.Weather.Aloft.Flakes * 0.15f); }
};

void Shade(Pen &Hand)
{
    const Tree &Plant = Hand.Plant;
    if (Plant.Layer == 0) return;
    float Reach = Plant.Height * (Plant.Kind == TreeKind::Spruce ? 0.22f : 0.3f);
    float Cast  = (0.5f - Hand.Shown.Sky.SunX) * Plant.Height * 0.3f * Hand.Shown.Sky.Day;
    float Dark  = (0.18f + 0.22f * Hand.Shown.Sky.Day) * (1.0f - 0.5f * Hand.Shown.Weather.Ground.Snow);
    for (int Dy = -1; Dy <= 1; Dy++) {
        float Wide = Reach * (Dy == 0 ? 1.0f : 0.72f);
        for (int Dx = -static_cast<int>(Wide); Dx <= static_cast<int>(Wide); Dx++) {
            float Fade = 1.0f - std::fabs(static_cast<float>(Dx)) / (Wide + 1.0f);
            Hand.Picture.Blend(static_cast<int>(Plant.X + Cast) + Dx, static_cast<int>(Plant.Foot) + Dy, Shadow,
                               Weighted(Dark * Fade));
        }
    }
}

// ---- boughs -------------------------------------------------------------------

Color BarkAt(const Pen &Hand, int X, int Y, int Side, int Width)
{
    bool  Birch = Hand.Plant.Kind == TreeKind::Birch;
    Color Ink   = Hand.Bark;
    if (Side == 0 && Width > 1) Ink = Lighter(Ink, 28);
    if (Side == Width - 1 && Width > 1) Ink = Darker(Ink, 45);
    if (Birch && (Hash(X, Y, Hand.Salt) & 7u) == 0) Ink = Hand.Mark;
    else if (!Birch && (Hash(X, Y, Hand.Salt) & 7u) == 0) Ink = Darker(Ink, 25);
    return Ink;
}

void Bough(Pen &Hand, const Branch &Limb)
{
    float Along = std::max(std::fabs(Limb.X1 - Limb.X0), std::fabs(Limb.Y1 - Limb.Y0));
    int   Steps = std::max(1, static_cast<int>(std::ceil(Along)));
    int   Width = std::max(1, static_cast<int>(std::lround(Limb.Width)));
    bool  Flat  = std::fabs(Limb.X1 - Limb.X0) > std::fabs(Limb.Y1 - Limb.Y0);
    float Snow  = Hand.SnowCover();
    for (int At = 0; At <= Steps; At++) {
        float T      = static_cast<float>(At) / static_cast<float>(Steps);
        float Across = Limb.X0 + (Limb.X1 - Limb.X0) * T;
        float Up     = Limb.Y0 + (Limb.Y1 - Limb.Y0) * T;
        int   X      = Hand.AcrossAt(Across, Up) - (Flat ? 0 : Width / 2);
        int   Y      = Hand.RowAt(Up) - (Flat ? Width / 2 : 0);
        for (int Side = 0; Side < Width; Side++)
            Hand.Put(X + (Flat ? 0 : Side), Y + (Flat ? Side : 0), BarkAt(Hand, X + Side, Y, Side, Width));
        if (Hand.Plant.Layer > 0 && Flat && Grain(X, Y, Hand.Salt) < Snow * 0.8f)
            Hand.Put(X, Y - 1, Hand.Snow);
    }
}

// ---- crowns -------------------------------------------------------------------

// The leaf colour of the tree now, before the light on this pixel.
Color CrownInk(const Pen &Hand)
{
    const Land &Ground = Hand.Shown.Weather.Ground;
    const Look &Chosen = Hand.Shown.Chosen;
    bool        Birch  = Hand.Plant.Kind == TreeKind::Birch;
    Color       Green  = Mixed(Chosen.Leaves, YellowLeaf, 25 * Hand.Plant.Variant);
    if (Birch) Green = Lighter(Green, 30);
    Green = Mixed(Green, SpringInk, Weighted(Ground.Blossom * 0.55f));
    return Hand.Dye(Mixed(Green, AutumnInk(Chosen, Hand.Plant.Variant), Weighted(Ground.Autumn)));
}

void Crown(Pen &Hand, const Blob &Tuft, std::uint32_t Index)
{
    const Land &Ground = Hand.Shown.Weather.Ground;
    const Color Leaf   = CrownInk(Hand);
    int         Reach  = static_cast<int>(std::ceil(Tuft.Radius));
    int         Bx     = static_cast<int>(std::lround(Tuft.X));
    int         By     = static_cast<int>(std::lround(Tuft.Y));
    for (int Dy = -Reach; Dy <= Reach; Dy++) {
        for (int Dx = -Reach; Dx <= Reach; Dx++) {
            float Nx = static_cast<float>(Dx) / Tuft.Radius, Ny = static_cast<float>(Dy) / Tuft.Radius;
            if (Nx * Nx + Ny * Ny > 0.8f + 0.2f * Grain(Bx + Dx, By - Dy, 51)) continue;
            if (Grain(Bx + Dx, By - Dy, Hand.Salt + Index) >= Ground.Leaves) continue;

            float Clump = Grain((Bx + Dx) >> 1, (By - Dy) >> 1, Hand.Salt);
            Color Ink   = Leaf;
            float Lit   = -Ny * 0.6f - Nx * 0.4f;
            if (Lit > 0.15f) Ink = Lighter(Ink, static_cast<int>(Lit * 70.0f));
            else if (Lit < -0.1f) Ink = Darker(Ink, static_cast<int>(-Lit * 90.0f));
            if (Clump < 0.28f) Ink = Darker(Ink, 28);
            else if (Clump > 0.85f) Ink = Lighter(Ink, 26);
            if (Hand.Plant.Blossoms && Grain(Bx + Dx, By - Dy, 41) < Ground.Blossom * 0.6f)
                Ink = Mixed(Hand.Blossom, Color::Rgb(0xFF, 0xFF, 0xFF), static_cast<int>(Clump * 90.0f));
            float Up = Tuft.Y - static_cast<float>(Dy);
            Hand.Put(Hand.AcrossAt(Tuft.X + static_cast<float>(Dx), Up), Hand.RowAt(Up), Ink);
        }
    }
}

void PaintBroadleaf(Pen &Hand)
{
    for (const Branch &Limb : Hand.Plant.Branches) Bough(Hand, Limb);
    std::uint32_t Index = 0;
    for (const Blob &Tuft : Hand.Plant.Crown) Crown(Hand, Tuft, Index++);
}

// ---- spruces ------------------------------------------------------------------

float SpruceWidth(const Tree &Plant, float Up)
{
    float Start = Plant.Height * 0.12f;
    float T     = (Up - Start) / (Plant.Height - Start);
    if (T < 0 || T > 1) return -1.0f;
    float Tiers = std::max(3.0f, Plant.Height / 9.0f);
    float Step  = T * Tiers - std::floor(T * Tiers);
    return Plant.Height * (Plant.Layer == 2 ? 0.24f : 0.2f) * (1.0f - T) * (0.55f + 0.45f * (1.0f - Step));
}

void PaintSpruce(Pen &Hand)
{
    const Tree &Plant = Hand.Plant;
    Color       Green = Hand.Dye(Mixed(SpruceInk, Lighter(SpruceInk, 70), 14 * Plant.Variant));
    float       Snow  = Hand.SnowCover();
    int         Trunk = std::max(1, static_cast<int>(std::lround(Plant.Trunk)));

    for (int Up = 0; static_cast<float>(Up) < Plant.Height * 0.2f; Up++)
        for (int Side = 0; Side < Trunk; Side++)
            Hand.Put(Hand.AcrossAt(static_cast<float>(Side - Trunk / 2), static_cast<float>(Up)), Hand.RowAt(static_cast<float>(Up)),
                     BarkAt(Hand, Side, Up, Side, Trunk));

    for (int Up = static_cast<int>(Plant.Height * 0.12f); static_cast<float>(Up) <= Plant.Height; Up++) {
        float Half = SpruceWidth(Plant, static_cast<float>(Up));
        if (Half < 0) continue;
        float Above = std::max(0.0f, SpruceWidth(Plant, static_cast<float>(Up + 1)));
        int   Reach = static_cast<int>(Half + 0.5f);
        for (int Dx = -Reach; Dx <= Reach; Dx++) {
            float Across = static_cast<float>(Dx);
            Color Ink    = Green;
            float Clump  = Grain(Dx, Up, Hand.Salt);
            if (Dx < 0) Ink = Lighter(Ink, 22);
            else if (Dx > Reach / 2) Ink = Darker(Ink, 38);
            if (Clump < 0.25f) Ink = Darker(Ink, 26);
            else if (Clump > 0.88f) Ink = Lighter(Ink, 24);
            if (std::fabs(Across) > Above + 0.5f && Grain(Dx, Up, 43) < Snow * 0.95f) Ink = Hand.Snow;
            else if (Grain(Dx, Up, 47) < Snow * 0.12f) Ink = Lighter(Ink, 120);
            Hand.Put(Hand.AcrossAt(Across, static_cast<float>(Up)), Hand.RowAt(static_cast<float>(Up)), Ink);
        }
    }
}

}  // namespace

void PaintTree(const World &Shown, Canvas &Picture, const Tree &Plant)
{
    Pen Hand(Shown, Picture, Plant);
    Shade(Hand);
    if (Plant.Kind == TreeKind::Spruce) PaintSpruce(Hand);
    else PaintBroadleaf(Hand);
}

}  // namespace forest
