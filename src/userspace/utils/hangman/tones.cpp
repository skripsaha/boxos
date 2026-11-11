/* tones.cpp — the scene's colours (tones.h). */

#include "tones.h"

namespace hangman {

namespace {

Tones ByDay(const Look &Shown)
{
    return Tones{.SkyTop   = Shown.Sky,
                 .SkyLow   = Lighter(Shown.Sky, 0.6f),
                 .Far      = Mixed(Lighter(Shown.Sky, 0.45f), Rgb(96, 112, 150), 0.55f),
                 .Hill     = Shown.Grass,
                 .HillDeep = Darker(Shown.Grass, 0.4f),
                 .Sun      = Shown.Sun,
                 .Cloud    = Rgb(250, 252, 255),
                 .Leaf     = Mixed(Shown.Grass, Rgb(24, 96, 44), 0.5f),
                 .Trunk    = Darker(Shown.Wood, 0.2f),
                 .Timber   = Shown.Wood,
                 .Figure   = Shown.Figure};
}

Tones ByDusk(const Look &Shown)
{
    const Color Night = Rgb(38, 30, 78);
    return Tones{.SkyTop   = Mixed(Darker(Shown.Sky, 0.7f), Night, 0.6f),
                 .SkyLow   = Rgb(238, 128, 94),
                 .Far      = Rgb(84, 56, 104),
                 .Hill     = Mixed(Darker(Shown.Grass, 0.55f), Night, 0.3f),
                 .HillDeep = Mixed(Darker(Shown.Grass, 0.8f), Night, 0.4f),
                 .Sun      = Rgb(255, 118, 58),
                 .Cloud    = Rgb(150, 100, 126),
                 .Leaf     = Mixed(Rgb(24, 56, 44), Night, 0.3f),
                 .Trunk    = Mixed(Darker(Shown.Wood, 0.5f), Night, 0.3f),
                 .Timber   = Mixed(Darker(Shown.Wood, 0.4f), Night, 0.25f),
                 .Figure   = Mixed(Shown.Figure, Rgb(0, 0, 0), 0.3f)};
}

Tones ByJoy(const Look &Shown)
{
    return Tones{.SkyTop   = Lighter(Shown.Sky, 0.15f),
                 .SkyLow   = Rgb(255, 236, 176),
                 .Far      = Rgb(150, 172, 206),
                 .Hill     = Lighter(Shown.Grass, 0.14f),
                 .HillDeep = Shown.Grass,
                 .Sun      = Lighter(Shown.Sun, 0.3f),
                 .Cloud    = Rgb(255, 255, 255),
                 .Leaf     = Mixed(Lighter(Shown.Grass, 0.1f), Rgb(40, 120, 52), 0.4f),
                 .Trunk    = Shown.Wood,
                 .Timber   = Lighter(Shown.Wood, 0.12f),
                 .Figure   = Shown.Figure};
}

Tones Between(const Tones &From, const Tones &To, float Weight)
{
    return Tones{.SkyTop   = Mixed(From.SkyTop, To.SkyTop, Weight),
                 .SkyLow   = Mixed(From.SkyLow, To.SkyLow, Weight),
                 .Far      = Mixed(From.Far, To.Far, Weight),
                 .Hill     = Mixed(From.Hill, To.Hill, Weight),
                 .HillDeep = Mixed(From.HillDeep, To.HillDeep, Weight),
                 .Sun      = Mixed(From.Sun, To.Sun, Weight),
                 .Cloud    = Mixed(From.Cloud, To.Cloud, Weight),
                 .Leaf     = Mixed(From.Leaf, To.Leaf, Weight),
                 .Trunk    = Mixed(From.Trunk, To.Trunk, Weight),
                 .Timber   = Mixed(From.Timber, To.Timber, Weight),
                 .Figure   = Mixed(From.Figure, To.Figure, Weight)};
}

}  // namespace

Tones TonesFor(const Look &Shown, float Dusk, float Joy)
{
    return Between(Between(ByDay(Shown), ByDusk(Shown), Dusk), ByJoy(Shown), Joy);
}

}  // namespace hangman
