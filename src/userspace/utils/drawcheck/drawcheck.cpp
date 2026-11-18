/* drawcheck — the drawing engine held to itself on this very processor
 * (painter/core). Every way the processor offers to work the engine's rows
 * — plain, SSE2, AVX2 (painter/core/cpu_path.h) — draws the same scenes of
 * every kind of thing, and scenes of text in the system's fonts; each must
 * make the very pixels the plain way makes, and write the very numbers to
 * the glass, near and dithered. Then how long each way takes, for a frame of
 * 1920x1080, and how long setting and drawing a page of text takes. The last line says PASS when
 * every way is alike, FAIL when one is not. The host's draw stand
 * (tools/draw_stand.sh) holds the engine to the brush and to exact areas;
 * this holds it on the machine. */

#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include "box/form.h"

#include "boxos_pixel.h"
#include "drawcheck.h"
#include "painter/core/cpu_path.h"
#include "painter/core/system_fonts.h"

BOX_FORM("");

namespace {

using namespace painter::core;
/* BoxLib's C colour type is named Color too: Painter's is meant here. */
using painter::core::Color;

constexpr std::uint32_t Shapes = 24, Texts = 8, Scenes = Shapes + Texts;
constexpr std::uint32_t Wide = 640, High = 400;

/* FNV-1a over the bytes of what one scene made. */
std::uint64_t Digest(std::uint64_t Hash, const void *Bytes, std::size_t Count)
{
    const auto *Byte = static_cast<const std::uint8_t *>(Bytes);
    for (std::size_t I = 0; I < Count; I++) Hash = (Hash ^ Byte[I]) * 1099511628211ull;
    return Hash;
}

/* Every pixel of each scene, and the glass's numbers near and dithered, as
 * one digest a scene. */
std::optional<std::vector<std::uint64_t>> Draw(const typecase::Fonts &Book)
{
    std::vector<std::uint64_t> Digests;
    std::vector<std::uint32_t> Glass(static_cast<std::size_t>(Wide) * High);
    for (std::uint32_t Seed = 1; Seed <= Scenes; Seed++) {
        std::optional<Canvas> C = Canvas::Make(Wide, High);
        if (!C) return std::nullopt;
        if (Seed <= Shapes) drawcheck::DrawScene(*C, Seed);
        else drawcheck::DrawTextScene(*C, Seed - Shapes, Book);
        std::uint64_t Hash = 14695981039346656037ull;
        for (std::uint32_t Y = 0; Y < High; Y++) Hash = Digest(Hash, C->Row(Y), Wide * sizeof(Pixel));
        for (Dither Mode : {Dither::Off, Dither::On}) {
            C->CopyOut(Rect{0, 0, static_cast<std::int32_t>(Wide), static_cast<std::int32_t>(High)}, Glass.data(), Wide, BOX_PIXEL_BGR, Mode);
            Hash = Digest(Hash, Glass.data(), Glass.size() * sizeof(std::uint32_t));
        }
        Digests.push_back(Hash);
    }
    return Digests;
}

}  // namespace

int main()
{
    const CpuPath Best = BestCpuPath();
    std::printf("drawcheck: the processor offers %s — the engine works it so\n", CpuPathName(Best));
    const std::shared_ptr<const typecase::Fonts> Book = SystemFonts();
    const std::vector<std::string>               Families = Book->Families();
    std::string                                  Named;
    for (const std::string &Each : Families) Named += (Named.empty() ? "" : ", ") + Each;
    std::printf("drawcheck: the system's fonts — %s; text set in %s\n", Named.empty() ? "none" : Named.c_str(),
                Book->Empty() ? "boxes of codes" : Book->Default().c_str());
    UseCpuPath(CpuPath::Plain);
    const auto Plainly = Draw(*Book);
    if (!Plainly) {
        std::printf("drawcheck: FAIL — no room for a canvas of %ux%u\n", Wide, High);
        return 1;
    }
    bool Alike = true;
    for (std::uint8_t Way = 1; Way <= static_cast<std::uint8_t>(Best); Way++) {
        UseCpuPath(static_cast<CpuPath>(Way));
        const auto    Made  = Draw(*Book);
        std::uint32_t Wrong = 0;
        for (std::uint32_t K = 0; K < Scenes; K++) Wrong += !Made || (*Made)[K] != (*Plainly)[K];
        std::printf("drawcheck: %s — %u scenes, %s\n", CpuPathName(static_cast<CpuPath>(Way)), Scenes,
                    Wrong ? "NOT the plain way's pixels" : "every pixel the plain way's, near and dithered");
        if (Wrong) std::printf("drawcheck: %s unlike the plain way in %u of %u scenes\n", CpuPathName(static_cast<CpuPath>(Way)), Wrong, Scenes);
        Alike = Alike && !Wrong;
    }
    for (std::uint8_t Way = 0; Way <= static_cast<std::uint8_t>(Best); Way++) {
        UseCpuPath(static_cast<CpuPath>(Way));
        drawcheck::SaySpeed();
    }
    UseCpuPath(Best);
    drawcheck::SayTextSpeed(*Book);
    std::printf(Alike ? "drawcheck: PASS — every way the processor offers draws the plain way's very pixels\n"
                      : "drawcheck: FAIL — a way of the processor's draws other pixels than the plain way\n");
    return Alike ? 0 : 1;
}
