/* paint_air.cpp — the weather over the picture: rain with its splashes, snow,
 * leaves and petals on the wind, the specks and notes the animals leave, fog
 * drifting low, light falling slantwise through the trees, and the flash and
 * bolt of lightning. */

#include <algorithm>
#include <cmath>

#include "paint.h"

namespace forest {

namespace {

constexpr float LeafLies = 8.0f;

const Color RainInk   = Color::Rgb(0xD0, 0xDE, 0xEE);
const Color NoteInk   = Color::Rgb(0xF4, 0xF0, 0xD8);
const Color RayInk    = Color::Rgb(0xFF, 0xF2, 0xC8);
const Color RayLow    = Color::Rgb(0xFF, 0xB0, 0x70);
const Color FlashInk  = Color::Rgb(0xE8, 0xEE, 0xFF);
const Color BoltInk   = Color::Rgb(0xFF, 0xFF, 0xF2);
const Color GreenLeaf = Color::Rgb(0x58, 0x92, 0x34);

int Weighted(float Value) { return static_cast<int>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 256.0f)); }

int Pixel(float Value) { return static_cast<int>(std::lround(Value)); }

void PaintRain(const World &Shown, Canvas &Picture)
{
    const Falling &Air   = Shown.Air;
    float          Slant = 6.0f + 30.0f * Shown.Weather.Aloft.Wind;
    Color          Streak = Mixed(Shown.Sky.Horizon, RainInk, Weighted(0.45f + 0.4f * Shown.Sky.Day));
    for (const Drop &Each : Air.Drops) {
        float Lean = Slant / Each.Speed;
        for (int Tail = 0; Tail < 3; Tail++)
            Picture.Blend(Pixel(Each.X - Lean * static_cast<float>(Tail)), Pixel(Each.Y) - Tail, Streak, Weighted(0.75f - 0.2f * static_cast<float>(Tail)));
    }
    for (const Splash &Each : Air.Splashes) {
        int X = Pixel(Each.X), Y = Pixel(Each.Y);
        int Spread = Each.Age < 0.12f ? 1 : 2;
        int Up     = Each.Age < 0.12f ? 0 : 1;
        Picture.Blend(X - Spread, Y - Up, Streak, Weighted(0.6f));
        Picture.Blend(X + Spread, Y - Up, Streak, Weighted(0.6f));
    }
}

void PaintSnow(const World &Shown, Canvas &Picture)
{
    Color Crystal = Lit(Shown, Shown.Chosen.Snow);
    for (const Flake &Each : Shown.Air.Flakes) {
        int X = Pixel(Each.X), Y = Pixel(Each.Y);
        Picture.Blend(X, Y, Crystal, Weighted(0.92f));
        if (Each.Big) Picture.Blend(X + 1, Y, Crystal, Weighted(0.55f));
    }
}

void PaintLeaves(const World &Shown, Canvas &Picture)
{
    const Land &Ground = Shown.Weather.Ground;
    for (const Leaf &Each : Shown.Air.Leaves) {
        Color Ink = Each.Petal ? Shown.Chosen.Blossom : Ground.Autumn < 0.3f ? GreenLeaf : AutumnInk(Shown.Chosen, Each.Tone);
        Ink       = Lit(Shown, Ink);
        int X = Pixel(Each.X), Y = Pixel(Each.Y);
        if (Each.Y >= Each.Land) {
            Picture.Blend(X, Pixel(Each.Land), Ink, Weighted(1.0f - Each.Lying / LeafLies));
            continue;
        }
        Picture.Put(X, Y, Ink);
        if (!Each.Petal && Wave(Shown.Weather.Clock * 0.7f + Each.Phase) > 0) Picture.Put(X + 1, Y, Darker(Ink, 30));
    }
}

void PaintSpecks(const World &Shown, Canvas &Picture)
{
    Color Powder = Lit(Shown, Shown.Chosen.Snow);
    for (const Speck &Each : Shown.Air.Specks) {
        float Left = 1.0f - Each.Age / Each.Lasts;
        int   X = Pixel(Each.X), Y = Pixel(Each.Y);
        if (!Each.Note) {
            Picture.Blend(X, Y, Powder, Weighted(Left * 0.9f));
            continue;
        }
        float Fade = std::min(1.0f, Left * 2.0f);
        Picture.Blend(X, Y, NoteInk, Weighted(Fade));
        Picture.Blend(X, Y - 1, NoteInk, Weighted(Fade));
        Picture.Blend(X, Y - 2, NoteInk, Weighted(Fade));
        Picture.Blend(X + 1, Y - 2, NoteInk, Weighted(Fade * 0.8f));
    }
}

}  // namespace

void PaintAir(const World &Shown, Canvas &Picture)
{
    PaintRain(Shown, Picture);
    PaintSnow(Shown, Picture);
    PaintLeaves(Shown, Picture);
    PaintSpecks(Shown, Picture);
}

void PaintFog(const World &Shown, Canvas &Picture)
{
    float Fog = Shown.Weather.Aloft.Fog;
    if (Fog < 0.02f) return;
    const Layout &Size  = Shown.Place.Size;
    Color         Ink   = Lighter(Shown.Sky.Horizon, 24);
    float         Clock = Shown.Weather.Clock;
    for (int Y = 0; Y < Picture.Height; Y++) {
        float Low  = 0.3f + 0.7f * Smooth(static_cast<float>(Size.Ground) * 0.3f, static_cast<float>(Size.Ground), static_cast<float>(Y));
        float Prof = Fog * Low * (1.0f - 0.3f * Smooth(static_cast<float>(Size.Front), static_cast<float>(Size.Height), static_cast<float>(Y)));
        for (int X = 0; X < Picture.Width; X++) {
            float Fx = static_cast<float>(X), Fy = static_cast<float>(Y);
            float N  = 0.5f + 0.25f * Wave(Fx / 45.0f + Clock * 0.012f + Fy / 80.0f) + 0.25f * Wave(Fx / 19.0f - Clock * 0.02f + Fy / 37.0f);
            Picture.Blend(X, Y, Ink, Weighted(Prof * (0.1f + 0.3f * N)));
        }
    }
}

void PaintRays(const World &Shown, Canvas &Picture)
{
    const SkyNow &Sky   = Shown.Sky;
    const Air    &Aloft = Shown.Weather.Aloft;
    float Strength = (0.1f + Sky.Glow * 0.5f + Aloft.Fog * 0.25f) * (1.0f - Aloft.Gloom) * Smooth(0.2f, 0.5f, Sky.Day);
    if (!Sky.SunUp || Strength < 0.02f) return;
    const Layout &Size  = Shown.Place.Size;
    Color         Ink   = Mixed(RayInk, RayLow, Weighted(Sky.Glow));
    float         Clock = Shown.Weather.Clock;
    int           Reach = Size.Ground * 21 / 20;
    for (int Y = 0; Y < Reach; Y++) {
        float Fade = 1.0f - static_cast<float>(Y) / static_cast<float>(Reach);
        for (int X = 0; X < Picture.Width; X++) {
            float Along = static_cast<float>(X) + static_cast<float>(Y) * 0.55f;
            float Beam  = 0.5f * Wave(Along / 38.0f + Clock * 0.008f) + 0.5f * Wave(Along / 15.0f - Clock * 0.013f) - 0.15f;
            if (Beam > 0) Picture.Blend(X, Y, Ink, Weighted(Beam * Strength * Fade * 0.9f));
        }
    }
}

void PaintFlash(const World &Shown, Canvas &Picture)
{
    const Falling &Air = Shown.Air;
    if (Air.Flash > 0.02f)
        for (int Y = 0; Y < Picture.Height; Y++)
            for (int X = 0; X < Picture.Width; X++) Picture.Blend(X, Y, FlashInk, Weighted(Air.Flash * 0.5f));
    if (Air.Lightning.Age >= BoltLasts || Air.Lightning.Points.size() < 2) return;
    for (std::size_t At = 1; At < Air.Lightning.Points.size(); At++) {
        const Spot &From = Air.Lightning.Points[At - 1], &To = Air.Lightning.Points[At];
        Picture.Line(Pixel(From.X) - 1, Pixel(From.Y), Pixel(To.X) - 1, Pixel(To.Y), Mixed(Shown.Sky.Horizon, FlashInk, 150));
        Picture.Line(Pixel(From.X), Pixel(From.Y), Pixel(To.X), Pixel(To.Y), BoltInk);
    }
}

}  // namespace forest
