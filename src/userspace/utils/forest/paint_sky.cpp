/* paint_sky.cpp — the sky: its colours from the top down, the stars that come
 * out when the day is gone, the sun and the moon with the moon's real phase,
 * the clouds that drift with the wind and the grey sheet a storm pulls over
 * them, and the hills behind the trees. */

#include <algorithm>
#include <cmath>

#include "paint.h"

namespace forest {

namespace {

constexpr float Pi = 3.14159265358979323846f;

const Color SunHigh      = Color::Rgb(0xFF, 0xF4, 0xC8);
const Color SunLow       = Color::Rgb(0xFF, 0xA0, 0x50);
const Color MoonLit      = Color::Rgb(0xEC, 0xEA, 0xDC);
const Color MoonDark     = Color::Rgb(0x30, 0x32, 0x38);
const Color StarInk      = Color::Rgb(0xF4, 0xF4, 0xFF);
const Color CloudBright  = Color::Rgb(0xFA, 0xFA, 0xFC);
const Color CloudShade   = Color::Rgb(0xA0, 0xA6, 0xAC);
const Color StormInk     = Color::Rgb(0x50, 0x56, 0x60);
const Color HillFar      = Color::Rgb(0x5E, 0x7C, 0x74);
const Color HillNear     = Color::Rgb(0x3C, 0x6A, 0x46);
const Color HillAutumn   = Color::Rgb(0xA8, 0x7C, 0x40);

int Weighted(float Value) { return static_cast<int>(std::lround(std::clamp(Value, 0.0f, 1.0f) * 256.0f)); }

void PaintGradient(const World &Shown, Canvas &Picture)
{
    const Layout &Size = Shown.Place.Size;
    for (int Y = 0; Y < Picture.Height; Y++) {
        float Along = std::min(1.0f, static_cast<float>(Y) / static_cast<float>(Size.Ground));
        Color Row   = Mixed(Shown.Sky.Top, Shown.Sky.Horizon, Weighted(Along * (0.55f + 0.45f * Along)));
        for (int X = 0; X < Picture.Width; X++) Picture.At(X, Y) = Row;
    }
}

// How far up the sky a height of 0..1 is, in rows.
int SkyRow(const Layout &Size, float Height)
{
    return Size.Ground - static_cast<int>(std::lround(Height * static_cast<float>(Size.Ground) * 0.88f));
}

void PaintStars(const World &Shown, Canvas &Picture)
{
    const SkyNow &Sky   = Shown.Sky;
    const Air    &Aloft = Shown.Weather.Aloft;
    float Seen = (1.0f - std::min(1.0f, Sky.Day * 1.5f)) * (1.0f - Aloft.Cloud * 0.9f) * (1.0f - Aloft.Fog);
    if (Seen < 0.05f) return;
    for (const Star &Each : Shown.Place.Stars) {
        float Twinkle = 0.65f + 0.35f * Wave(Shown.Weather.Clock * 0.4f + Each.Phase);
        Picture.Blend(Each.X, Each.Y, StarInk, Weighted(Seen * Each.Bright * Twinkle * 1.4f));
    }
}

void PaintSun(const World &Shown, Canvas &Picture)
{
    const SkyNow &Sky = Shown.Sky;
    if (!Sky.SunUp || Sky.SunHeight < -0.02f) return;
    const Layout &Size  = Shown.Place.Size;
    float         Cloud = 1.0f - Shown.Weather.Aloft.Gloom * 0.85f;
    int           Cx    = static_cast<int>(Sky.SunX * static_cast<float>(Size.Width));
    int           Cy    = SkyRow(Size, Sky.SunHeight);
    int           Disc  = std::max(2, Size.Height / 30);
    Color         Ink   = Mixed(SunHigh, SunLow, Weighted(Sky.Glow * 1.2f));

    int Reach = Disc * 5;
    for (int Dy = -Reach; Dy <= Reach; Dy++) {
        for (int Dx = -Reach; Dx <= Reach; Dx++) {
            float Near = 1.0f - std::sqrt(static_cast<float>(Dx * Dx + Dy * Dy)) / static_cast<float>(Reach);
            if (Near <= 0) continue;
            Picture.Blend(Cx + Dx, Cy + Dy, Ink, Weighted(Near * Near * 0.5f * Cloud * (0.4f + 0.6f * Sky.Day)));
        }
    }
    if (Cloud < 0.3f) return;
    for (int Dy = -Disc; Dy <= Disc; Dy++)
        for (int Dx = -Disc; Dx <= Disc; Dx++)
            if (Dx * Dx + Dy * Dy <= Disc * Disc) Picture.Blend(Cx + Dx, Cy + Dy, Ink, Weighted(Cloud));
}

void PaintMoon(const World &Shown, Canvas &Picture)
{
    const SkyNow &Sky = Shown.Sky;
    if (!Sky.MoonUp) return;
    const Layout &Size = Shown.Place.Size;
    float Seen = (1.0f - Sky.Day * 0.85f) * (1.0f - Shown.Weather.Aloft.Gloom * 0.9f);
    if (Seen < 0.05f) return;
    int   Cx = static_cast<int>(Sky.MoonX * static_cast<float>(Size.Width));
    int   Cy = SkyRow(Size, Sky.MoonHeight);
    int   Disc = std::max(2, Size.Height / 34);
    float Turn = std::cos(2.0f * Pi * Sky.MoonRound);

    int Reach = Disc * 4;
    for (int Dy = -Reach; Dy <= Reach; Dy++) {
        for (int Dx = -Reach; Dx <= Reach; Dx++) {
            float Near = 1.0f - std::sqrt(static_cast<float>(Dx * Dx + Dy * Dy)) / static_cast<float>(Reach);
            if (Near > 0) Picture.Blend(Cx + Dx, Cy + Dy, MoonLit, Weighted(Near * Near * 0.22f * Seen * Sky.MoonLight));
        }
    }
    float Radius = static_cast<float>(Disc) + 0.4f;
    for (int Dy = -Disc; Dy <= Disc; Dy++) {
        for (int Dx = -Disc; Dx <= Disc; Dx++) {
            float Nx = static_cast<float>(Dx) / Radius, Ny = static_cast<float>(Dy) / Radius;
            if (Nx * Nx + Ny * Ny > 1.0f) continue;
            float Edge = Turn * std::sqrt(1.0f - Ny * Ny);
            bool  Lit  = Sky.MoonRound < 0.5f ? Nx > Edge : Nx < -Edge;
            Picture.Blend(Cx + Dx, Cy + Dy, Lit ? MoonLit : MoonDark, Weighted(Lit ? Seen : Seen * 0.3f));
        }
    }
}

void PaintPuff(const World &Shown, Canvas &Picture, float X, float Y, float Radius, float Alpha)
{
    float Gloom = Shown.Weather.Aloft.Gloom;
    for (int Dy = -static_cast<int>(Radius); Dy <= static_cast<int>(Radius); Dy++) {
        for (int Dx = -static_cast<int>(Radius); Dx <= static_cast<int>(Radius); Dx++) {
            if (static_cast<float>(Dx * Dx + Dy * Dy) > Radius * Radius) continue;
            float Down = (static_cast<float>(Dy) + Radius) / (2.0f * Radius);
            Color Ink  = Mixed(CloudBright, CloudShade, Weighted(Down * 0.7f + Gloom * 0.3f));
            Ink        = Tinted(Mixed(Ink, StormInk, Weighted(Gloom * 0.8f)), Shown.Sky.Light);
            Picture.Blend(static_cast<int>(X) + Dx, static_cast<int>(Y) + Dy, Ink, Weighted(Alpha * 0.92f));
        }
    }
}

void PaintClouds(const World &Shown, Canvas &Picture)
{
    const Layout &Size  = Shown.Place.Size;
    float         Cover = Shown.Weather.Aloft.Cloud;
    float         Wide  = static_cast<float>(Size.Width);
    for (const Cloud &Each : Shown.Place.Clouds) {
        if (Cover <= Each.Threshold) continue;
        float Alpha = std::min(1.0f, (Cover - Each.Threshold) * 4.0f);
        float Left  = std::fmod(Each.X + Wide * 0.2f + Each.Speed * Shown.Air.Passed, Wide * 1.4f) - Wide * 0.2f;
        for (const Blob &Puff : Each.Puffs) PaintPuff(Shown, Picture, Left + Puff.X, Each.Y + Puff.Y, Puff.Radius, Alpha);
    }

    float Gloom = Shown.Weather.Aloft.Gloom;
    if (Gloom < 0.35f) return;
    Color Sheet = Tinted(StormInk, Shown.Sky.Light);
    for (int X = 0; X < Picture.Width; X++) {
        float Edge = static_cast<float>(Size.Ground) * (0.08f + 0.34f * Gloom) +
                     3.0f * Wave(static_cast<float>(X) / 19.0f + Shown.Air.Passed * 0.02f) + 2.0f * Wave(static_cast<float>(X) / 7.0f);
        for (int Y = 0; static_cast<float>(Y) < Edge; Y++)
            Picture.Blend(X, Y, Sheet, Weighted((Gloom - 0.3f) * 1.1f * (1.0f - 0.45f * static_cast<float>(Y) / Edge)));
    }
}

}  // namespace

void PaintSky(const World &Shown, Canvas &Picture)
{
    PaintGradient(Shown, Picture);
    PaintStars(Shown, Picture);
    PaintMoon(Shown, Picture);
    PaintSun(Shown, Picture);
    PaintClouds(Shown, Picture);
}

void PaintHills(const World &Shown, Canvas &Picture)
{
    const Scenery &Place = Shown.Place;
    const Land    &Ground = Shown.Weather.Ground;
    auto Season = [&](Color Base) {
        Color Turned = Mixed(Base, HillAutumn, Weighted(Ground.Autumn * Ground.Leaves * 0.6f));
        return Mixed(Turned, Shown.Chosen.Snow, Weighted(Ground.Snow * 0.85f));
    };
    const Color Inks[2] = {Lit(Shown, Season(HillFar), 0.78f), Lit(Shown, Season(HillNear), 0.5f)};
    for (int X = 0; X < Picture.Width; X++) {
        for (int Row = 0; Row < 2; Row++) {
            int Rise = (Row == 0 ? Place.Far : Place.Near)[static_cast<std::size_t>(X)];
            int Top  = Place.Size.Ground - Rise;
            for (int Y = Top; Y <= Place.Size.Ground + 3; Y++) {
                float Down = static_cast<float>(Y - Top) / static_cast<float>(std::max(1, Rise));
                Picture.Put(X, Y, Mixed(Lighter(Inks[Row], 18), Inks[Row], Weighted(Down * 1.5f)));
            }
        }
    }
}

}  // namespace forest
