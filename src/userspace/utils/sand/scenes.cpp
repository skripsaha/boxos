/* scenes.cpp — empty, a beach, a volcano and a garden. */

#include "scenes.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

namespace sand {

namespace {

// Builds in heights above the floor, which is the last row when things fall down.
class Builder {
public:
    explicit Builder(World &Into) : Field(Into) {}

    int Wide() const { return Field.Width; }
    int High() const { return Field.Height; }

    void Put(int X, int Up, Kind What) { Field.Place(X, Field.Fall > 0 ? Field.Height - 1 - Up : Up, What); }

    void Column(int X, int From, int To, Kind What)
    {
        for (int Up = From; Up < To; Up++) Put(X, Up, What);
    }

    void Tree(int X, int Base)
    {
        Column(X, Base, Base + 8, Kind::Wood);
        for (int Dy = -3; Dy <= 3; Dy++)
            for (int Dx = -3; Dx <= 3; Dx++)
                if (Dx * Dx + Dy * Dy <= 9 && !(Dx == 0 && Dy < 0)) Put(X + Dx, Base + 8 + Dy, Kind::Plant);
    }

private:
    World &Field;
};

void Empty(World &) {}

void Beach(World &Into)
{
    Builder Make(Into);
    const int Wide = Make.Wide(), High = Make.High();
    const int WaterLevel = High * 26 / 100;
    const auto Shore = [&](int X) { return High * 12 / 100 + (High * 30 / 100) * X / Wide; };
    for (int X = 0; X < Wide; X++) {
        Make.Column(X, 0, Shore(X), Kind::Sand);
        Make.Column(X, Shore(X), WaterLevel, Kind::Water);
    }
    for (int X = Wide * 55 / 100; X < Wide * 55 / 100 + 14; X++) Make.Column(X, Shore(X), Shore(X) + 2, Kind::Wood);
    const int Boulder = Wide * 82 / 100;
    for (int X = Boulder; X < Boulder + 7; X++) Make.Column(X, Shore(X), Shore(X) + 5 - std::abs(X - Boulder - 3), Kind::Stone);
}

void Volcano(World &Into)
{
    Builder Make(Into);
    const int Wide = Make.Wide(), High = Make.High();
    const int Middle = Wide / 2, Peak = High * 48 / 100, Mouth = std::max(4, Wide / 20);
    for (int X = 0; X < Wide; X++) {
        const int Away = std::abs(X - Middle);
        const int Top  = std::max(3, Peak - std::max(0, Away - Mouth) * 11 / 10);
        if (Away <= Mouth - 2) {
            Make.Column(X, 0, Peak - 16, Kind::Stone);
            Make.Column(X, Peak - 16, Peak - 3, Kind::Lava);
        } else {
            Make.Column(X, 0, Top, Kind::Stone);
        }
        if (X < Wide / 8) Make.Column(X, Top, Top + 6, Kind::Water);
    }
    Make.Tree(Wide * 85 / 100, 3);
    Make.Tree(Wide * 93 / 100, 3);
}

void Garden(World &Into)
{
    Builder Make(Into);
    const int Wide = Make.Wide(), High = Make.High();
    const int Soil = High * 18 / 100, Middle = Wide / 2, Reach = Wide / 6, Depth = High * 10 / 100;
    for (int X = 0; X < Wide; X++) {
        const int Away = std::abs(X - Middle);
        const int Pond = Away < Reach ? (Reach * Reach - Away * Away) * Depth / (Reach * Reach) : 0;
        Make.Column(X, 0, Soil - Pond, Kind::Sand);
        Make.Column(X, Soil - Pond, Soil, Kind::Water);
        if (Away >= Reach && Away < Reach + 36 && X % 4 == 0) Make.Put(X, Soil, Kind::Seed);
    }
    for (const int X : {Wide / 10, Wide * 9 / 10}) Make.Column(X, Soil, Soil + 8, Kind::Wood);
}

constexpr Scene Scenes[] = {{"empty", Empty}, {"beach", Beach}, {"volcano", Volcano}, {"garden", Garden}};

}  // namespace

const Scene *FindScene(std::string_view Name)
{
    for (const Scene &One : Scenes)
        if (Name == One.Name) return &One;
    return nullptr;
}

std::string SceneNames()
{
    std::string Names;
    for (std::size_t At = 0; At < std::size(Scenes); At++) {
        if (At > 0) Names += At + 1 == std::size(Scenes) ? " or " : ", ";
        Names += Scenes[At].Name;
    }
    return Names;
}

}  // namespace sand
