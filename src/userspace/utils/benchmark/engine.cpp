/* engine.cpp — what the drawing engine's work costs on this processor. See
 * benchmark.h.
 *
 * Every way of working rows the processor offers (painter/core/cpu_path.h) is
 * measured on a tile of 256x256 pixels — the size the display composes in —
 * each piece of work the fastest of a few samples, every sample as many runs
 * as fill a millisecond. Then, once: a shadow's coverage made, a tile's canvas
 * had, memory copied, and a page of text set and drawn. */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "benchmark.h"
#include "boxos_pixel.h"
#include "painter/core/canvas.h"
#include "painter/core/cpu_path.h"
#include "painter/core/fill.h"
#include "painter/core/mask.h"
#include "painter/core/path.h"
#include "painter/core/picture.h"
#include "painter/core/shadow.h"
#include "painter/core/system_fonts.h"
#include "painter/core/text.h"
#include "painter/core/transform.h"
#include "typecase/layout.h"

namespace benchmark {

namespace {

using namespace painter::core;
/* BoxLib's C colour type is named Color too: Painter's is meant here. */
using painter::core::Color;
using Clock = std::chrono::steady_clock;

constexpr std::uint32_t Side = 256;
constexpr double        All  = static_cast<double>(Side) * Side;

/* Nanoseconds a pixel of Work, which covers Pixels: the fastest of five
 * samples after a first run that warms it, each sample as many runs as fill a
 * millisecond — one run is shorter than some clocks' step. */
template <typename Working> double NsPerPixel(Working &&Work, double Pixels)
{
    Work();
    double Least = 1e300;
    for (int Sample = 0; Sample < 5; Sample++) {
        const Clock::time_point Began = Clock::now();
        std::uint32_t           Runs  = 0;
        Clock::duration         Took{};
        do {
            Work();
            Runs++;
            Took = Clock::now() - Began;
        } while (Took < std::chrono::milliseconds(1));
        Least = std::min(Least, std::chrono::duration<double, std::nano>(Took).count() / Runs);
    }
    return Least / Pixels;
}

/* What setting a page of text and drawing it cost: ~4000 characters set
 * justified, drawn with their glyphs kept, and drawn at a size never drawn. */
void Text(Report &Out)
{
    const std::shared_ptr<const typecase::Fonts> Book = SystemFonts();
    std::optional<Canvas>                        On   = Canvas::Make(1920, 1080);
    if (!On) return;
    const char *const Words[] = {"BoxOS",  "draws",    "text",   "in",     "any",   "font,", "size", "and",
                                 "place;", "kerned",   "Wa",     "To",     "AVATAR", "set",   "and",  "drawn",
                                 "\xD1\x82\xD0\xB5\xD0\xBA\xD1\x81\xD1\x82", "\xE2\x80\x94", "justified\xE2\x80\xA6"};
    std::string       Said;
    for (std::size_t I = 0; Said.size() < 4000; I++) Said += std::string(Words[I % std::size(Words)]) + " ";
    typecase::Text Laying = typecase::Text::Of(Said);
    Laying.Style.Size     = 16;
    typecase::TextBox Box;
    Box.Width = 1800;
    Box.Align = typecase::TextAlign::Justify;
    const auto Us = [](auto &&Work, int Rounds) {
        const Clock::time_point Began = Clock::now();
        for (int Round = 0; Round < Rounds; Round++) Work();
        return std::chrono::duration<double, std::micro>(Clock::now() - Began).count() / Rounds;
    };
    std::optional<typecase::Layout> Laid;
    const double Setting = Us([&] { Laid = typecase::Layout::Of(Laying, Box, *Book); }, 8);
    const double Glyphs  = static_cast<double>(Laid->Glyphs().size());
    const Fill   Ink(Color::Rgb(240, 240, 240));
    On->FillText(*Laid, {40, 40}, Ink);
    const double Kept  = Us([&] { On->FillText(*Laid, {40, 40}, Ink); }, 8);
    double       Fresh = 0;
    for (int Round = 0; Round < 3; Round++) {
        typecase::Text Other = Laying;
        Other.Style.Size     = 17.0f + static_cast<float>(Round) * 0.37f;
        const typecase::Layout New = typecase::Layout::Of(Other, Box, *Book);
        Fresh += Us([&] { On->FillText(New, {40, 40}, Ink); }, 1) / 3;
    }
    Out.Say("engine: text in %s — %zu characters set in %.0f us (%.2f us each); %.0f glyphs drawn in %.0f us kept (%.0f ns a "
            "glyph), %.0f us at a size never drawn",
            Book->Empty() ? "boxes of codes" : Book->Default().c_str(), Laying.Characters.size(), Setting,
            Setting / static_cast<double>(Laying.Characters.size()), Glyphs, Kept, Kept * 1000 / Glyphs, Fresh);
}

}  // namespace

void Engine(Report &Out)
{
    std::optional<Canvas> Tile = Canvas::Make(Side, Side), Kept = Canvas::Make(Side, Side), Other = Canvas::Make(Side, Side);
    if (!Tile || !Kept || !Other) {
        Out.Say("engine: no room for three canvases of %ux%u — not measured", Side, Side);
        return;
    }
    Kept->Clear(Color::Rgba(40, 120, 200, 255));
    Kept->Fill(Path::Circle({128, 128}, 90), Color::Rgba(250, 200, 60, 255));
    Other->Clear(Color::Rgba(200, 60, 90, 255));
    std::vector<Pixel> Pixels;
    for (std::uint32_t Y = 0; Y < Side; Y++) Pixels.insert(Pixels.end(), Kept->Row(Y), Kept->Row(Y) + Side);
    const auto      Shown = Picture::FromPixels(Pixels, Side, Side, Side);
    const Transform Turn  = Transform::Translate(-128, -128).Then(Transform::Rotate(10)).Then(Transform::Translate(128, 128));
    const Shadow    Cast{{0, 6}, 16, 0, Color::Rgba(0, 0, 0, 96), false};
    const Path      Panel  = Path::RoundedRect(RectF{24, 24, 208, 208}, Corners::All(16));
    const Path      Square = Path::Rect(RectF{0, 0, Side, Side});
    const Fill      Along(Gradient::Linear({0, 0}, {Side, Side}, {{0, Color::Rgb(20, 24, 60)}, {1, Color::Rgb(200, 90, 120)}}));
    const Fill      FromMiddle(Gradient::Radial({128, 128}, 180, {{0, Color::Rgb(250, 240, 220)}, {1, Color::Rgb(30, 20, 40)}}));
    Stroke          Ring;
    Ring.Width = 4;
    std::vector<std::uint32_t> Glass(static_cast<std::size_t>(Side) * Side);
    const Rect                 Whole{0, 0, Side, Side};

    const CpuPath Was = ChosenCpuPath(), Best = BestCpuPath();
    for (std::uint8_t Way = 0; Way <= static_cast<std::uint8_t>(Best); Way++) {
        if (!UseCpuPath(static_cast<CpuPath>(Way))) continue;
        const Color Through = Color::Rgba(30, 160, 90, 200);
        const double Clear   = NsPerPixel([&] { Tile->Clear(Color::Rgba(20, 20, 30, 255)); }, All);
        const double Opaque  = NsPerPixel([&] { Tile->Draw(*Kept, {0, 0}, 1.0f); }, All);
        const double Half    = NsPerPixel([&] { Tile->Draw(*Kept, {0, 0}, 0.5f); }, All);
        const double Plain   = NsPerPixel([&] { (void)Tile->Fill(Square, Through); }, All);
        const double Round   = NsPerPixel([&] { (void)Tile->Fill(Path::RoundedRect(RectF{0, 0, Side, Side}, Corners::All(24)), Through); }, All);
        const double Circle  = NsPerPixel([&] { (void)Tile->Fill(Path::Circle({128, 128}, 128), Through); }, 3.14159 * 128 * 128);
        const double Linear  = NsPerPixel([&] { (void)Tile->Fill(Square, Along); }, All);
        const double Radial  = NsPerPixel([&] { (void)Tile->Fill(Square, FromMiddle); }, All);
        const double Lined   = NsPerPixel([&] { (void)Tile->Stroke(Panel, Ring, Fill(Color::Rgb(255, 255, 255))); }, All);
        const double Shade   = NsPerPixel([&] {
            if (const auto Mask = Cast.MaskFor(Panel)) Tile->DrawShadow(*Mask, Cast);
        }, All);
        const double Turned  = NsPerPixel([&] { (void)Tile->Fill(Square, Fill::Picture(Shown), Turn); }, All);
        const double Fade    = NsPerPixel([&] { Other->FadeTo(*Kept, 0.5f); }, All);
        const double Dither  = NsPerPixel([&] { Tile->CopyOut(Whole, Glass.data(), Side, BOX_PIXEL_BGR, Dither::On); }, All);
        const double Nearest = NsPerPixel([&] { Tile->CopyOut(Whole, Glass.data(), Side, BOX_PIXEL_BGR, Dither::Off); }, All);
        Out.Say("engine %s, ns a pixel: clear %.2f, opaque %.2f, half %.2f, rect %.2f, round %.2f, circle %.2f, linear %.2f, "
                "radial %.2f, ring %.2f, shadow %.2f, turned %.2f, fade %.2f, out dithered %.2f, out nearest %.2f",
                CpuPathName(static_cast<CpuPath>(Way)), Clear, Opaque, Half, Plain, Round, Circle, Linear, Radial, Lined, Shade,
                Turned, Fade, Dither, Nearest);
    }
    (void)UseCpuPath(Best);

    const double MaskUs = NsPerPixel([&] { (void)Cast.MaskFor(Panel); }, 1.0) / 1000.0;
    const double MakeUs = NsPerPixel([&] { (void)Canvas::Make(Side, Side); }, 1.0) / 1000.0;
    std::vector<std::uint32_t> From(1u << 20), To(1u << 20);
    const double Copy = NsPerPixel([&] { std::memcpy(To.data(), From.data(), From.size() * 4); }, static_cast<double>(From.size()));
    Out.Say("engine: a shadow's coverage made in %.1f us, a %ux%u canvas had in %.1f us, memory copied %.2f ns a pixel", MaskUs,
            Side, Side, MakeUs, Copy);
    Text(Out);
    (void)UseCpuPath(Was);
}

}  // namespace benchmark
