/* air.cpp — rain, snow, leaves, lightning and the specks the animals throw up. */

#include "air.h"

#include <algorithm>
#include <cmath>

namespace forest {

namespace {

constexpr std::size_t MostLeaves = 160;
constexpr float       LeafLies   = 8.0f;

template <typename Thing>
void Remove(std::vector<Thing> &Things, std::size_t At)
{
    Things[At] = Things.back();
    Things.pop_back();
}

float Area(const Layout &Size)
{
    return static_cast<float>(Size.Width) * static_cast<float>(Size.Height);
}

float LandRow(const Layout &Size, Dice &Chance)
{
    return Chance.Between(static_cast<float>(Size.Ground + 2), static_cast<float>(Size.Front));
}

// A leaf, or a petal, let go from the crown of a tree that has some.
bool Release(const Scenery &Place, Dice &Chance, bool Petal, Leaf &Out)
{
    for (int Try = 0; Try < 8; Try++) {
        const Tree &From = Place.Trees[static_cast<std::size_t>(Chance.Count(0, static_cast<int>(Place.Trees.size()) - 1))];
        if (From.Layer == 0 || From.Kind == TreeKind::Spruce || From.Crown.empty()) continue;
        if (Petal && !From.Blossoms) continue;
        const Blob &Tuft = From.Crown[static_cast<std::size_t>(Chance.Count(0, static_cast<int>(From.Crown.size()) - 1))];
        Out.X     = From.X + Tuft.X + Chance.Between(-Tuft.Radius, Tuft.Radius) * 0.8f;
        Out.Y     = From.Foot - Tuft.Y + Chance.Between(-0.3f, 0.5f) * Tuft.Radius;
        Out.Land  = std::max(LandRow(Place.Size, Chance), Out.Y + 3.0f);
        Out.Phase = Chance.Unit();
        Out.Lying = 0;
        Out.Tone  = Chance.Count(0, 3);
        Out.Petal = Petal;
        return true;
    }
    return false;
}

}  // namespace

void Falling::Rain(const Nature &Weather, const Layout &Size, Dice &Chance, float Seconds)
{
    float Slant = 6.0f + 30.0f * Weather.Aloft.Wind;
    auto  Want  = static_cast<std::size_t>(Weather.Aloft.Rain * Area(Size) / 70.0f);
    for (std::size_t At = 0; At < Drops.size();) {
        Drop &Each = Drops[At];
        Each.Y += Each.Speed * Seconds;
        Each.X += Slant * Seconds;
        if (Each.Y < Each.Land) {
            At++;
            continue;
        }
        Splashes.push_back({Each.X, Each.Land, 0.0f});
        if (Drops.size() > Want) {
            Remove(Drops, At);
            continue;
        }
        Each = {Chance.Between(-20.0f, static_cast<float>(Size.Width)), -Chance.Between(0.0f, 12.0f), Chance.Between(70.0f, 110.0f),
                LandRow(Size, Chance)};
        At++;
    }
    for (int Added = 0; Drops.size() < Want && Added < 12; Added++)
        Drops.push_back({Chance.Between(-20.0f, static_cast<float>(Size.Width)), -Chance.Between(0.0f, 12.0f),
                         Chance.Between(70.0f, 110.0f), LandRow(Size, Chance)});
    for (std::size_t At = 0; At < Splashes.size();) {
        Splashes[At].Age += Seconds;
        if (Splashes[At].Age > 0.25f) Remove(Splashes, At);
        else At++;
    }
}

void Falling::Snow(const Nature &Weather, const Layout &Size, Dice &Chance, float Seconds)
{
    auto Want = static_cast<std::size_t>(Weather.Aloft.Flakes * Area(Size) / 110.0f);
    for (std::size_t At = 0; At < Flakes.size();) {
        Flake &Each = Flakes[At];
        Each.Y += Each.Speed * Seconds;
        Each.X += (Wave(Weather.Clock * 0.25f + Each.Phase) * 8.0f + Weather.Aloft.Wind * 30.0f) * Seconds;
        if (Each.X > static_cast<float>(Size.Width)) Each.X -= static_cast<float>(Size.Width);
        if (Each.X < 0) Each.X += static_cast<float>(Size.Width);
        if (Each.Y < static_cast<float>(Size.Height)) {
            At++;
            continue;
        }
        if (Flakes.size() > Want) {
            Remove(Flakes, At);
            continue;
        }
        Each.Y = -Chance.Between(0.0f, 6.0f);
        At++;
    }
    for (int Added = 0; Flakes.size() < Want && Added < 8; Added++)
        Flakes.push_back({Chance.Between(0.0f, static_cast<float>(Size.Width)), -Chance.Between(0.0f, 6.0f),
                          Chance.Between(7.0f, 16.0f), Chance.Unit(), Chance.Chance(0.2f)});
}

void Falling::Foliage(const Nature &Weather, const Scenery &Place, Dice &Chance, float Seconds)
{
    const Land &Ground = Weather.Ground;
    float       Wind   = Weather.Aloft.Wind;
    float       Per    = static_cast<float>(Place.Size.Width) / 128.0f;
    float       Shed   = std::max(0.0f, Ground.Leaves - 0.05f) * Ground.Autumn;
    LeafOwed += (Shed * (0.8f + 4.0f * Wind) + Ground.Leaves * Wind * 0.6f) * Per * Seconds;
    PetalOwed += Ground.Blossom * (0.9f + 4.0f * Wind) * Per * Seconds;

    for (; LeafOwed >= 1.0f; LeafOwed -= 1.0f) {
        Leaf Fresh;
        if (Leaves.size() < MostLeaves && Release(Place, Chance, false, Fresh)) Leaves.push_back(Fresh);
    }
    for (; PetalOwed >= 1.0f; PetalOwed -= 1.0f) {
        Leaf Fresh;
        if (Leaves.size() < MostLeaves && Release(Place, Chance, true, Fresh)) Leaves.push_back(Fresh);
    }

    for (std::size_t At = 0; At < Leaves.size();) {
        Leaf &Each = Leaves[At];
        if (Each.Y < Each.Land) {
            Each.Y += (Each.Petal ? 5.0f : 7.0f) * Seconds * (0.8f + 0.4f * Wave(Weather.Clock * 0.3f + Each.Phase));
            Each.X += (Wave(Weather.Clock * 0.35f + Each.Phase) * 7.0f + Wind * 28.0f) * Seconds;
        } else {
            Each.Lying += Seconds;
        }
        if (Each.Lying > LeafLies) Remove(Leaves, At);
        else At++;
    }
}

void Falling::Strike(const Nature &Weather, const Layout &Size, Dice &Chance, float Seconds)
{
    Flash = std::max(0.0f, Flash - 3.2f * Seconds);
    Lightning.Age += Seconds;
    if (EchoLeft > 0) {
        EchoLeft -= Seconds;
        if (EchoLeft <= 0) Flash = 0.7f;
    }
    if (Weather.Aloft.Storm < 0.3f || !Chance.Chance(0.14f * Weather.Aloft.Storm * Seconds)) return;

    Flash    = 1.0f;
    EchoLeft = 0.14f;
    Strikes += 1;
    Lightning.Age = 0;
    Lightning.Points.clear();
    float X    = Chance.Between(0.1f, 0.9f) * static_cast<float>(Size.Width);
    float Y    = static_cast<float>(Size.Ground) * 0.25f;
    float Down = Chance.Between(static_cast<float>(Size.Ground), static_cast<float>(Size.Front) * 0.85f);
    while (Y < Down) {
        Lightning.Points.push_back({X, Y});
        X += Chance.Between(-4.0f, 4.0f);
        Y += Chance.Between(3.0f, 6.0f);
    }
    Lightning.Points.push_back({X, Down});
}

void Falling::Step(const Nature &Weather, const Scenery &Place, Dice &Chance, float Seconds)
{
    Passed += Seconds * (0.4f + 3.0f * Weather.Aloft.Wind);
    Rain(Weather, Place.Size, Chance, Seconds);
    Snow(Weather, Place.Size, Chance, Seconds);
    Foliage(Weather, Place, Chance, Seconds);
    Strike(Weather, Place.Size, Chance, Seconds);

    for (std::size_t At = 0; At < Specks.size();) {
        Speck &Each = Specks[At];
        Each.Age += Seconds;
        Each.X += Each.Vx * Seconds;
        Each.Y += Each.Vy * Seconds;
        if (!Each.Note) Each.Vy += 40.0f * Seconds;
        if (Each.Age > Each.Lasts) Remove(Specks, At);
        else At++;
    }
}

void Falling::Powder(float X, float Y, int Count, Dice &Chance)
{
    for (int At = 0; At < Count; At++)
        Specks.push_back({X, Y, Chance.Between(-16.0f, 16.0f), -Chance.Between(10.0f, 26.0f), 0.0f, Chance.Between(0.5f, 0.9f), false});
}

void Falling::Note(float X, float Y)
{
    Specks.push_back({X, Y, 0.0f, -8.0f, 0.0f, 1.4f, true});
}

}  // namespace forest
