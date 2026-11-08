/* mandelbrot_paint.cpp — every pixel worked out and coloured (mandelbrot.h). */

#include <algorithm>
#include <cmath>

#include "box/clock.h"

#include "mandelbrot.h"

namespace {

using Color = painter::core::Color;

constexpr double Bailout   = 256.0 * 256.0;   /* far past 2: the smooth count settles */
constexpr double LogTwo    = 0.6931471805599453;
constexpr int    FirstStep = 8;

/* Whether c is in the main cardioid or the bulb left of it: most of what is
 * inside at the first view, known without a single step. */
bool PlainlyInside(double Cx, double Cy)
{
    double Qx = Cx - 0.25, Q = Qx * Qx + Cy * Cy;
    if (Q * (Q + Qx) <= 0.25 * Cy * Cy) return true;
    double Bx = Cx + 1.0;
    return Bx * Bx + Cy * Cy <= 1.0 / 16.0;
}

/* How far z0 under z = z*z + c got before it escaped, smoothly; -1 when it
 * never did within `Steps`, or fell into a cycle — Brent's way: z is kept at
 * every power of two and a return to it within `Near` is a cycle. */
float Escape(double Zx, double Zy, double Cx, double Cy, int Steps, double Near)
{
    double X = Zx, Y = Zy, X2 = X * X, Y2 = Y * Y;
    double KeptX = X, KeptY = Y;
    int    Since = 0, Span = 8;
    for (int N = 0; N < Steps; N++) {
        if (X2 + Y2 > Bailout) {
            double Nu = std::log(0.5 * std::log(X2 + Y2) / LogTwo) / LogTwo;
            return static_cast<float>(std::max(0.0, N + 1 - Nu));
        }
        Y  = 2.0 * X * Y + Cy;
        X  = X2 - Y2 + Cx;
        X2 = X * X;
        Y2 = Y * Y;
        if (std::fabs(X - KeptX) < Near && std::fabs(Y - KeptY) < Near) return -1.0f;
        if (++Since == Span) {
            Since = 0;
            Span *= 2;
            KeptX = X;
            KeptY = Y;
        }
    }
    return -1.0f;
}

/* A palette from its stops, each a place 0..1 round the loop and a colour. */
struct Stop {
    float Place;
    Color Ink;
};

FractalPalette Build(const char *Name, std::initializer_list<Stop> Stops)
{
    FractalPalette Out{Name, {}};
    const Stop    *First = Stops.begin();
    for (int At = 0; At < 256; At++) {
        float Place = static_cast<float>(At) / 256.0f;
        const Stop *Low = First, *High = First;
        for (const Stop *One = First; One != Stops.end(); One++) {
            if (One->Place <= Place) Low = One;
            if (One->Place > Place) {
                High = One;
                break;
            }
        }
        float Span = High->Place - Low->Place;
        float Part = Span > 0 ? (Place - Low->Place) / Span : 0.0f;
        Out.Ramp[At] = Color::Mix(Low->Ink, High->Ink, static_cast<std::uint8_t>(Part * 255.0f));
    }
    return Out;
}

const FractalPalette *Palettes()
{
    static const FractalPalette All[] = {
        Build("classic", {{0.0f, Color::Rgb(0x00, 0x07, 0x64)}, {0.16f, Color::Rgb(0x20, 0x6B, 0xCB)},
                          {0.42f, Color::Rgb(0xED, 0xFF, 0xFF)}, {0.6425f, Color::Rgb(0xFF, 0xAA, 0x00)},
                          {0.8575f, Color::Rgb(0x30, 0x10, 0x00)}, {1.0f, Color::Rgb(0x00, 0x07, 0x64)}}),
        Build("fire", {{0.0f, Color::Rgb(0x14, 0x00, 0x00)}, {0.2f, Color::Rgb(0x90, 0x00, 0x00)},
                       {0.4f, Color::Rgb(0xFF, 0x50, 0x00)}, {0.55f, Color::Rgb(0xFF, 0xD0, 0x40)},
                       {0.65f, Color::Rgb(0xFF, 0xFF, 0xE0)}, {0.8f, Color::Rgb(0xFF, 0x80, 0x00)},
                       {1.0f, Color::Rgb(0x14, 0x00, 0x00)}}),
        Build("ocean", {{0.0f, Color::Rgb(0x00, 0x08, 0x18)}, {0.2f, Color::Rgb(0x00, 0x30, 0x60)},
                        {0.4f, Color::Rgb(0x00, 0x80, 0xC0)}, {0.55f, Color::Rgb(0x60, 0xE0, 0xFF)},
                        {0.65f, Color::Rgb(0xFF, 0xFF, 0xFF)}, {0.8f, Color::Rgb(0x20, 0x90, 0xD0)},
                        {1.0f, Color::Rgb(0x00, 0x08, 0x18)}}),
        Build("candy", {{0.0f, Color::Rgb(0x12, 0x06, 0x1A)}, {0.2f, Color::Rgb(0x81, 0x26, 0x5C)},
                        {0.4f, Color::Rgb(0xE8, 0x5C, 0x93)}, {0.55f, Color::Rgb(0xFF, 0xE2, 0xF0)},
                        {0.7f, Color::Rgb(0xB8, 0x3E, 0x79)}, {0.85f, Color::Rgb(0x3A, 0x0F, 0x40)},
                        {1.0f, Color::Rgb(0x12, 0x06, 0x1A)}}),
        Build("aurora", {{0.0f, Color::Rgb(0x00, 0x10, 0x10)}, {0.2f, Color::Rgb(0x00, 0xA0, 0x60)},
                         {0.4f, Color::Rgb(0x60, 0xFF, 0x90)}, {0.5f, Color::Rgb(0xC0, 0xFF, 0xFF)},
                         {0.7f, Color::Rgb(0x60, 0x40, 0xE0)}, {0.85f, Color::Rgb(0x20, 0x00, 0x40)},
                         {1.0f, Color::Rgb(0x00, 0x10, 0x10)}}),
    };
    return All;
}

}

int FractalSteps(const FractalView &View, double Detail)
{
    /* Deep views are full of slow escapes: the need grows faster than the depth. */
    double Depth = std::max(0.0, std::log2(3.4 / View.Width));
    return std::clamp(static_cast<int>((150.0 + 50.0 * Depth + 2.0 * Depth * Depth) * Detail), 32, 50000);
}

void FractalPicture::Resize(int Wide, int High)
{
    Width  = Wide;
    Height = High;
    Counts.assign(static_cast<std::size_t>(Wide) * High, -1.0f);
    Step = 0;
}

void FractalPicture::Restart(const FractalView &Look, int Most)
{
    View  = Look;
    Steps = Most;
    Step  = FirstStep;
    Row   = 0;
}

void FractalPicture::Fill(int X, int Y, float Count)
{
    for (int Dy = 0; Dy < Step && Y + Dy < Height; Dy++)
        for (int Dx = 0; Dx < Step && X + Dx < Width; Dx++)
            Counts[static_cast<std::size_t>(Y + Dy) * Width + X + Dx] = Count;
}

/* A pass works the rows that are multiples of its block, and of them the
 * pixels the coarser passes did not: the picture is never worked twice. */
bool FractalPicture::Work(std::uint64_t Budget)
{
    if (Step == 0) return false;
    std::uint64_t Began = clock_uptime_us();
    double        Pixel = View.Width / Width;
    double        Left  = View.CentreX - View.Width / 2;
    double        Top   = View.CentreY + Pixel * Height / 2;
    double        Near  = Pixel * 1e-3;
    while (Step != 0) {
        for (; Row < Height; Row += Step) {
            double Cy   = Top - (Row + 0.5) * Pixel;
            bool   Done = Step != FirstStep && Row % (Step * 2) == 0;
            for (int X = 0; X < Width; X += Step) {
                if (Done && X % (Step * 2) == 0) continue;
                double Cx = Left + (X + 0.5) * Pixel;
                float  Count;
                if (View.Julia) Count = Escape(Cx, Cy, View.JuliaX, View.JuliaY, Steps, Near);
                else Count = PlainlyInside(Cx, Cy) ? -1.0f : Escape(0, 0, Cx, Cy, Steps, Near);
                Fill(X, Row, Count);
            }
            if (clock_uptime_us() - Began >= Budget) {
                Row += Step;
                return true;
            }
        }
        Step /= 2;
        Row = 0;
    }
    return false;
}

const FractalPalette &FractalPalettes(int Which)
{
    return Palettes()[Which % FractalPaletteCount()];
}

int FractalPaletteCount()
{
    return 5;
}

void FractalColour(const FractalPicture &Picture, const FractalPalette &Palette, float Spread, float Turn,
                   std::vector<PainterColor> &Pixels)
{
    Pixels.resize(Picture.Counts.size());
    float Scale = 256.0f / Spread;
    for (std::size_t At = 0; At < Picture.Counts.size(); At++) {
        float Count = Picture.Counts[At];
        if (Count < 0) {
            Pixels[At] = painter::core::Colors::Black;
            continue;
        }
        float Place = std::fmod(Count * Scale + Turn, 256.0f);
        int   Index = static_cast<int>(Place);
        PainterColor Here = Palette.Ramp[Index & 255], Next = Palette.Ramp[(Index + 1) & 255];
        Pixels[At]  = PainterColor::Mix(Here, Next, static_cast<std::uint8_t>((Place - Index) * 255.0f));
    }
}
