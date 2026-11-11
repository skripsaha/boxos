/* ship_art.cpp — the ships' pictures (ship_art.h). Every shape is a distance
 * to an edge, so a hull is as smooth as the pixels allow at any size of cell. */

#include "ship_art.h"

#include <algorithm>
#include <cmath>
#include <span>

#include "rules.h"

namespace seabattle {

namespace {

enum class PartKind { Turret, Box, Funnel, Plane };

// A thing standing on the deck. At is its place along the deck from the stern, 0..1; Side its
// place across, in cells from the middle; Big and Small its size in cells: a radius, or a
// length and a width.
struct Part {
    PartKind Kind;
    float    At;
    float    Side;
    float    Big;
    float    Small;
};

constexpr Part Patrol[] = {{PartKind::Box, 0.42f, 0, 0.34f, 0.30f}, {PartKind::Turret, 0.80f, 0, 0.13f, 0}};
constexpr Part Destroyer[] = {{PartKind::Funnel, 0.22f, 0, 0.13f, 0}, {PartKind::Box, 0.45f, 0, 0.36f, 0.30f},
                              {PartKind::Turret, 0.76f, 0, 0.22f, 0}};
constexpr Part Cruiser[] = {{PartKind::Turret, 0.15f, 0, 0.2f, 0}, {PartKind::Funnel, 0.34f, 0, 0.12f, 0},
                            {PartKind::Box, 0.50f, 0, 0.40f, 0.32f}, {PartKind::Turret, 0.80f, 0, 0.23f, 0}};
constexpr Part Battleship[] = {{PartKind::Turret, 0.09f, 0, 0.2f, 0}, {PartKind::Funnel, 0.25f, 0, 0.11f, 0},
                               {PartKind::Funnel, 0.35f, 0, 0.11f, 0}, {PartKind::Box, 0.52f, 0, 0.42f, 0.34f},
                               {PartKind::Turret, 0.70f, 0, 0.23f, 0}, {PartKind::Turret, 0.86f, 0, 0.23f, 0}};
constexpr Part Carrier[] = {{PartKind::Plane, 0.16f, -0.13f, 0.16f, 0}, {PartKind::Plane, 0.32f, 0.12f, 0.16f, 0},
                            {PartKind::Box, 0.62f, 0.28f, 0.42f, 0.18f}};

std::span<const Part> PartsOf(int Length)
{
    switch (Length) {
    case 1:  return Patrol;
    case 2:  return Destroyer;
    case 3:  return Cruiser;
    case 4:  return Battleship;
    default: return Carrier;
    }
}

// The hull in the ship's own measures: u runs from stern to bow, v across.
struct Hull {
    float Cell;
    float Start;     // where the hull begins along u
    float Span;      // its length
    float Beam;      // half its width
    float Stern;     // the rounded stern's length
    float Bow;       // the pointed bow's length
};

Hull HullOf(const ShipDraw &Ship)
{
    const auto Cell = static_cast<float>(Ship.Cell);
    const float Span = Cell * static_cast<float>(Ship.Length) - 0.24f * Cell;
    const float Slim = Clamped(0.011f * (Cell - 8.0f), 0.0f, 0.11f);
    const float Beam = std::max(0.8f, (Ship.Length == LongestShip ? 0.47f - Slim : 0.43f - Slim) * Cell);
    return Hull{Cell, 0.12f * Cell, Span, Beam, std::min(0.6f * Cell, Span * 0.2f), std::min(1.7f * Cell, Span * 0.5f)};
}

// A point's place on the hull: how far outside its edge, how far along, and the hull's half width there.
struct Spot {
    float Outside;
    float Along;
    float Half;
    float Across;      // from the middle line
};

Spot Locate(const Hull &Shape, float U, float V)
{
    const float Along = U - Shape.Start;
    float       Half = Shape.Beam;
    if (Along < Shape.Stern) {
        const float Near = 1.0f - std::max(0.0f, Along) / Shape.Stern;
        Half = Shape.Beam * std::sqrt(std::max(0.0f, 1.0f - Near * Near));
    } else if (Along > Shape.Span - Shape.Bow) {
        Half = Shape.Beam * std::sqrt(std::max(0.0f, (Shape.Span - Along) / Shape.Bow));
    }
    const float Across = V - Shape.Cell * 0.5f;
    return Spot{std::max(std::fabs(Across) - Half, std::max(-Along, Along - Shape.Span)), Along, Half, Across};
}

Color DeckOf(Color Steel) { return Mixed(Lighter(Steel, 0.18f), Rgb(0xB4, 0xA4, 0x80), 0.3f); }

// The steel and the deck at a point of the hull: a dark outline, the plating of the side, then the deck.
Color HullColor(const ShipDraw &Ship, const Spot &At, float U, float V)
{
    const float Depth = -At.Outside;
    const float Lit = 0.5f - 0.5f * At.Across / std::max(0.5f, At.Half);
    Color       Ink = Darker(Ship.Steel, 0.74f);
    if (Ship.Plain) return Depth >= 0.75f ? Ship.Steel : Ink;
    if (Depth >= 0.75f) {
        Ink = Toned(Ship.Steel, 0.62f + 0.22f * Lit);
        if (Depth >= 1.7f) {
            Ink = Toned(DeckOf(Ship.Steel), 0.8f + 0.3f * Lit);
            if (static_cast<int>(At.Along) % 3 == 0 && Ship.Cell >= 6) Ink = Toned(Ink, 0.93f);
        }
    }
    if (!Ship.Wreck) return Ink;
    Ink = Mixed(Ink, Rgb(0x23, 0x1D, 0x19), 0.6f);
    if (Grain(static_cast<int>(U), static_cast<int>(V), 41) > 0.72f) Ink = Darker(Ink, 0.45f);
    return Mixed(Ink, Ship.Water, 0.28f);
}

float Solid(float Distance) { return Distance <= 0.0f ? 1.0f : 0.0f; }

float DiscCover(float Du, float Dv, float Radius) { return Solid(std::sqrt(Du * Du + Dv * Dv) - Radius); }

// What a part adds at a point: its colour over Ink, by how much of the pixel it covers.
Color PartColor(const Part &On, const Hull &Shape, const ShipDraw &Ship, float U, float V, Color Ink)
{
    const float Du = U - (Shape.Start + On.At * Shape.Span), Dv = V - (Shape.Cell * 0.5f + On.Side * Shape.Cell);
    const float Big = On.Big * Shape.Cell, Small = On.Small * Shape.Cell;
    switch (On.Kind) {
    case PartKind::Turret: {
        const float Facing = On.At < 0.3f ? -1.0f : 1.0f;
        const float Along = Du * Facing;
        const float Reach = Big * (Ship.Cell >= 10 ? 2.9f : 2.5f);
        const float Pair = Ship.Cell >= 10 ? Big * 0.45f : 0.0f;
        const float Barrel = (Along > 0.0f && Along < Reach) ? std::max(Solid(std::fabs(Dv - Pair) - 0.55f), Solid(std::fabs(Dv + Pair) - 0.55f)) : 0.0f;
        Ink = Mixed(Ink, Darker(Ship.Steel, 0.7f), Barrel);
        const float Far = std::sqrt(Du * Du + Dv * Dv);
        const float Light = 1.15f - 0.55f * (Du + Dv) / (Big * 2.0f);
        Color Cap = Toned(Darker(Ship.Steel, 0.3f), Light);
        if (Far > Big - 1.0f && Big >= 2.0f) Cap = Darker(Ship.Steel, 0.62f);
        return Mixed(Ink, Cap, DiscCover(Du, Dv, Big));
    }
    case PartKind::Box: {
        const float Edge = std::max(std::fabs(Du) - Big * 0.5f, std::fabs(Dv) - Small * 0.5f);
        Color Roof = Lighter(Ship.Steel, 0.28f);
        if (Dv > Small * 0.18f) Roof = Darker(Roof, 0.22f);
        if (Dv < -Small * 0.12f && Dv > -Small * 0.34f && std::fabs(Du) < Big * 0.34f) Roof = Rgb(0x1E, 0x2A, 0x38);
        return Mixed(Ink, Roof, Solid(Edge));
    }
    case PartKind::Funnel: {
        const float Far = std::sqrt(Du * Du + Dv * Dv);
        const Color Body = Far < Big * 0.5f ? Rgb(0x20, 0x20, 0x24) : Rgb(0xC0, 0x40, 0x30);
        return Mixed(Ink, Body, Solid(Far - Big));
    }
    case PartKind::Plane: {
        const float Body = std::fabs(Dv) < 0.5f && std::fabs(Du) < Big ? 1.0f : 0.0f;
        const float Wing = std::fabs(Du + Big * 0.15f) < 0.6f && std::fabs(Dv) < Big * 1.1f ? 1.0f : 0.0f;
        return Mixed(Ink, Rgb(0xE8, 0xE8, 0xE0), std::max(Body, Wing));
    }
    }
    return Ink;
}

// The ship at a point: its colour, and how much of the pixel it covers. A ship going down is
// two halves drawn apart along its length, torn at the break.
struct Sample {
    Color Ink;
    float Cover = 0.0f;
};

Sample SampleShip(const ShipDraw &Ship, const Hull &Shape, float U, float V)
{
    const float Sunk = Ship.Sink;
    if (Sunk > 0.0f) {
        const float Gap = Sunk * 0.45f * Shape.Cell;
        const float Mid = Shape.Start + Shape.Span * 0.5f + (Grain(static_cast<int>(V * 2.0f), 17) - 0.5f) * 0.5f * Shape.Cell * Sunk;
        if (U < Mid - Gap) U += Gap;
        else if (U > Mid + Gap) U -= Gap;
        else return {};
    }
    const Spot At = Locate(Shape, U, V);
    Sample     Got;
    Got.Cover = Solid(At.Outside);
    if (Got.Cover <= 0.0f) return Got;
    Got.Ink = HullColor(Ship, At, U, V);
    if (!Ship.Wreck && !Ship.Plain && Ship.Cell >= 5 && At.Outside < -0.9f)
        for (const Part &On : PartsOf(Ship.Length)) Got.Ink = PartColor(On, Shape, Ship, U, V, Got.Ink);
    if (Ship.Wreck && Grain(static_cast<int>(U * 1.5f), static_cast<int>(V * 1.5f), 77) > 0.9f) Got.Cover = 0.0f;
    if (Sunk > 0.0f) Got.Ink = Mixed(Got.Ink, Ship.Water, Sunk * 0.6f);
    return Got;
}

}  // namespace

void DrawShip(Canvas &Out, const ShipDraw &Ship)
{
    const Hull  Shape = HullOf(Ship);
    const int   Long = Ship.Cell * Ship.Length;
    const int   Wide = Ship.Vertical ? Ship.Cell : Long, High = Ship.Vertical ? Long : Ship.Cell;
    const float Swell = Wave(static_cast<float>(Ship.Ms % 2600) / 2600.0f + static_cast<float>(Hash(Ship.X, Ship.Y) & 15) / 16.0f) * 0.28f;
    const float Faded = Ship.Alpha * (1.0f - Ship.Sink * Ship.Sink);
    for (int Y = Ship.Y - 2; Y < Ship.Y + High + 2; Y++) {
        for (int X = Ship.X - 2; X < Ship.X + Wide + 2; X++) {
            const float Across = static_cast<float>(X - Ship.X) + 0.5f, Down = static_cast<float>(Y - Ship.Y) + 0.5f;
            const float U = Ship.Vertical ? Down : Across, V = (Ship.Vertical ? Across : Down) - Swell;
            const float ShadowU = Ship.Vertical ? 1.0f : 0.8f, ShadowV = Ship.Vertical ? 0.8f : 1.0f;
            if (Ship.Alpha >= 1.0f && !Ship.Wreck) Out.Cover(X, Y, Rgb(0, 0, 0), SampleShip(Ship, Shape, U - ShadowU, V - ShadowV).Cover * 0.34f);
            const Sample Got = SampleShip(Ship, Shape, U, V);
            if (Got.Cover <= 0.0f) {
                const Spot Near = Locate(Shape, U, V);
                if (Ship.Sink == 0.0f && Near.Outside < 1.5f && Near.Outside > 0.0f)
                    Out.Cover(X, Y, Rgb(0xFF, 0xFF, 0xFF), (1.0f - Near.Outside / 1.5f) * 0.22f * Faded);
                continue;
            }
            Out.Cover(X, Y, Mixed(Got.Ink, Ship.Tint, Ship.TintShare), Got.Cover * Faded);
        }
    }
}

}  // namespace seabattle
