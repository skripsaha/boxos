/* faces.cpp — the shapes of the six symbols (faces.h). */

#include "faces.h"

#include <algorithm>
#include <cmath>

#include "canvas.h"
#include "trig.h"

namespace casino {

namespace {

// A color in 0..255 floats and how much of the point it covers.
struct Ink {
    float Red   = 0;
    float Green = 0;
    float Blue  = 0;
    float Cover = 0;
};

Ink Solid(float Red, float Green, float Blue, float Light = 1.0f)
{
    return Ink{std::min(Red * Light, 255.0f), std::min(Green * Light, 255.0f), std::min(Blue * Light, 255.0f), 1.0f};
}

// Distances, negative inside.
float DiscAt(float U, float V, float Across, float Down, float Radius)
{
    return std::sqrt((U - Across) * (U - Across) + (V - Down) * (V - Down)) - Radius;
}

float BoxAt(float U, float V, float Across, float Down, float HalfAcross, float HalfDown)
{
    return std::max(std::fabs(U - Across) - HalfAcross, std::fabs(V - Down) - HalfDown);
}

float SegmentAt(float U, float V, float FromU, float FromV, float ToU, float ToV, float Radius)
{
    const float AlongU = ToU - FromU, AlongV = ToV - FromV;
    const float Reach  = std::clamp(((U - FromU) * AlongU + (V - FromV) * AlongV) / (AlongU * AlongU + AlongV * AlongV), 0.0f, 1.0f);
    return std::sqrt((U - FromU - Reach * AlongU) * (U - FromU - Reach * AlongU) +
                     (V - FromV - Reach * AlongV) * (V - FromV - Reach * AlongV)) - Radius;
}

bool InPolygon(float U, float V, const float (*Points)[2], int Count)
{
    bool Inside = false;
    for (int At = 0, Back = Count - 1; At < Count; Back = At++) {
        if ((Points[At][1] > V) != (Points[Back][1] > V) &&
            U < (Points[Back][0] - Points[At][0]) * (V - Points[At][1]) / (Points[Back][1] - Points[At][1]) + Points[At][0])
            Inside = !Inside;
    }
    return Inside;
}

constexpr float GoldRed = 255, GoldGreen = 205, GoldBlue = 64;

Ink Seven(float U, float V)
{
    const float Edge = std::min(BoxAt(U, V, 0.0f, -0.6f, 0.68f, 0.24f), SegmentAt(U, V, 0.46f, -0.46f, -0.22f, 0.8f, 0.23f));
    if (Edge > 0) return {};
    if (Edge > -0.08f) return Solid(GoldRed, GoldGreen, GoldBlue);
    return Solid(226, 36, 48, 1.05f - 0.3f * (V + 1.0f) / 2.0f);
}

Ink Bar(float U, float V)
{
    const float Edge = BoxAt(U, V, 0.0f, 0.0f, 0.92f, 0.52f);
    if (Edge > 0) return {};
    if (Edge > -0.13f) return Solid(GoldRed, GoldGreen, GoldBlue, 0.8f + 0.3f * (1.0f - (V + 1.0f) / 2.0f));
    const float Col = (U + 0.68f) / 1.36f * 17.0f, Row = (V + 0.28f) / 0.56f * 7.0f;
    if (Col >= 0 && Col < 17 && Row >= 0 && Row < 7) {
        const int Which = static_cast<int>(Col) / 6, Within = static_cast<int>(Col) % 6;
        if (Within < 5 && Canvas::Inked("BAR"[Which], Within, static_cast<int>(Row))) return Solid(244, 244, 238);
    }
    return Solid(34, 34, 40, 1.0f + 0.4f * (1.0f - (V + 1.0f) / 2.0f));
}

Ink Cherry(float U, float V)
{
    if (DiscAt(U, V, -0.5f, 0.26f, 0.09f) < 0 || DiscAt(U, V, 0.3f, 0.12f, 0.09f) < 0) return Solid(255, 255, 255);
    const float Centers[2][2] = {{-0.4f, 0.4f}, {0.42f, 0.3f}};
    for (const auto &Center : Centers) {
        if (DiscAt(U, V, Center[0], Center[1], 0.4f) >= 0) continue;
        const float Round = std::clamp(1.0f - DiscAt(U + 0.14f, V + 0.14f, Center[0], Center[1], 0.0f) / 0.5f, 0.0f, 1.0f);
        return Solid(214, 28, 40, 0.6f + 0.6f * Round);
    }
    if (SegmentAt(U, V, 0.05f, -0.6f, 0.62f, -0.82f, 0.1f) < 0) return Solid(70, 160, 50);
    if (SegmentAt(U, V, -0.4f, 0.05f, 0.08f, -0.64f, 0.05f) < 0 || SegmentAt(U, V, 0.42f, -0.05f, 0.08f, -0.64f, 0.05f) < 0)
        return Solid(104, 134, 40);
    return {};
}

bool InsideBell(float U, float V)
{
    if (DiscAt(U, V, 0.0f, -0.82f, 0.13f) < 0) return true;
    if (V < -0.1f) return DiscAt(U, V, 0.0f, -0.1f, 0.6f) < 0;
    if (V < 0.5f) return std::fabs(U) < 0.6f + (V + 0.1f) / 0.6f * 0.28f;
    return V < 0.68f && std::fabs(U) < 0.94f;
}

Ink Bell(float U, float V)
{
    if (DiscAt(U, V, 0.0f, 0.78f, 0.17f) < 0) return Solid(150, 98, 12);
    if (!InsideBell(U, V)) return {};
    const bool Rim  = !InsideBell(U * 1.12f, V * 1.06f - 0.03f) || (V > 0.5f);
    const float Lit = 1.18f - 0.5f * (U + 0.6f) / 1.2f + (U > -0.38f && U < -0.22f && V > -0.3f && V < 0.4f ? 0.25f : 0.0f);
    if (Rim) return Solid(GoldRed, GoldGreen, GoldBlue, 0.62f);
    return Solid(GoldRed, GoldGreen, GoldBlue, Lit);
}

Ink Lemon(float U, float V)
{
    if (SegmentAt(U, V, 0.1f, -0.52f, 0.6f, -0.82f, 0.09f) < 0 && V < -0.42f) return Solid(70, 160, 50);
    const float Cos = 0.866f, Sin = 0.5f;
    const float Along = U * Cos + V * Sin, Across = -U * Sin + V * Cos;
    const float Part  = Along / 0.9f;
    if (std::fabs(Part) >= 1.0f) return {};
    const float Width = 0.56f * std::pow(1.0f - Part * Part, 0.7f);
    if (std::fabs(Across) >= Width) return {};
    const float Rim = std::fabs(Across) / Width;
    if (Rim > 0.9f) return Solid(190, 160, 20);
    const float Shine = DiscAt(U, V, -0.3f, -0.18f, 0.1f) < 0 ? 1.25f : 1.0f;
    return Solid(250, 226, 44, (1.15f - 0.3f * Rim) * Shine);
}

Ink Star(float U, float V)
{
    static float Points[10][2];
    static float Inner[10][2];
    static bool  Ready = false;
    if (!Ready) {
        for (int At = 0; At < 10; At++) {
            const float Angle  = At * Pi / 5.0f;
            const float Radius = At % 2 == 0 ? 0.97f : 0.4f;
            Points[At][0]      = Sine(Angle) * Radius;
            Points[At][1]      = -Cosine(Angle) * Radius;
            Inner[At][0]       = Points[At][0] * 0.8f;
            Inner[At][1]       = Points[At][1] * 0.8f;
        }
        Ready = true;
    }
    if (!InPolygon(U, V, Points, 10)) return {};
    if (!InPolygon(U, V, Inner, 10)) return Solid(190, 118, 26);
    float Turn = Bearing(U, V);
    if (Turn < 0) Turn += TwoPi;
    return Solid(GoldRed, GoldGreen, GoldBlue, static_cast<int>(Turn / (Pi / 5.0f)) % 2 == 0 ? 1.12f : 0.88f);
}

Ink Draw(Symbol Which, float U, float V)
{
    switch (Which) {
    case Symbol::Seven:  return Seven(U, V);
    case Symbol::Bar:    return Bar(U, V);
    case Symbol::Bell:   return Bell(U, V);
    case Symbol::Star:   return Star(U, V);
    case Symbol::Cherry: return Cherry(U, V);
    case Symbol::Lemon:  return Lemon(U, V);
    }
    return {};
}

constexpr int Grid = 3;

}  // namespace

Faces::Faces(int Edge) : Side(Edge), Images(static_cast<std::size_t>(SymbolKinds) * Edge * Edge)
{
    for (int Which = 0; Which < SymbolKinds; Which++) {
        for (int Down = 0; Down < Edge; Down++) {
            for (int Across = 0; Across < Edge; Across++) {
                float Red = 0, Green = 0, Blue = 0, Cover = 0;
                for (int Sub = 0; Sub < Grid * Grid; Sub++) {
                    const float U = (Across + (Sub % Grid + 0.5f) / Grid) / Edge * 2.0f - 1.0f;
                    const float V = (Down + (Sub / Grid + 0.5f) / Grid) / Edge * 2.0f - 1.0f;
                    const Ink   Seen = Draw(static_cast<Symbol>(Which), U, V);
                    Red += Seen.Red * Seen.Cover;
                    Green += Seen.Green * Seen.Cover;
                    Blue += Seen.Blue * Seen.Cover;
                    Cover += Seen.Cover;
                }
                Rgba &Pixel = Images[(static_cast<std::size_t>(Which) * Edge + Down) * Edge + Across];
                if (Cover <= 0) continue;
                Pixel = Rgba{static_cast<std::uint8_t>(Red / Cover), static_cast<std::uint8_t>(Green / Cover),
                             static_cast<std::uint8_t>(Blue / Cover), static_cast<std::uint8_t>(Cover / (Grid * Grid) * 255.0f)};
            }
        }
    }
}

Rgba Faces::At(Symbol Which, int Across, int Down) const
{
    if (Across < 0 || Across >= Side || Down < 0 || Down >= Side) return {};
    return Images[(static_cast<std::size_t>(Which) * Side + Down) * Side + Across];
}

}  // namespace casino
