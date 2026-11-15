/* imprint_make.cpp — the imprint made from the system's font. See
 * imprint_make.h and boxos_imprint.h. */

#include "imprint_make.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <utility>

#include "boxos_glyph_fold.h"
#include "boxos_symbols.h"
#include "typecase/chain.h"
#include "typecase/code_box.h"
#include "typecase/font.h"
#include "typecase/font_source.h"
#include "typecase/geometry_source.h"
#include "typecase/size.h"
#include "typecase/symbol_source.h"

namespace {

/* A glyph's pixels as the imprint keeps them: the picture as the cell shows
 * it, from its baseline, cut to the cell — two bits a pixel, the lowest
 * first, rounded to the nearest of the four. */
void Lay(const typecase::GlyphImage &Picture, const typecase::Size &At, std::vector<std::uint8_t> &Pixels)
{
    const std::size_t Start = Pixels.size();
    Pixels.resize(Start + (static_cast<std::size_t>(At.Width * At.Height) * 2 + 7) / 8, 0);
    for (std::int32_t Y = 0; Y < At.Height; Y++)
        for (std::int32_t X = 0; X < At.Width; X++) {
            const std::int32_t Across = X - Picture.Left, Down = Y - (At.Baseline + Picture.Top);
            if (Across < 0 || Across >= Picture.Width || Down < 0 || Down >= Picture.Height) continue;
            const int Inked = Picture.Coverage[static_cast<std::size_t>(Down * Picture.Width + Across)];
            const int Level = (Inked * static_cast<int>(BOXOS_IMPRINT_INKED) + 127) / 255;
            const std::size_t Pixel = static_cast<std::size_t>(Y * At.Width + X);
            Pixels[Start + Pixel / 4] |= static_cast<std::uint8_t>(Level << (Pixel % 4 * 2));
        }
}

}  // namespace

std::vector<char32_t> ImprintWanted()
{
    struct Range {
        char32_t First, Last;
    };
    constexpr Range Ranges[] = {
        {0x0020, 0x007E},   {0x00A0, 0x017F},   /* ASCII, Latin-1, Latin Extended-A */
        {0x0370, 0x03FF},   {0x0400, 0x04FF},   /* Greek, Cyrillic */
        {0x2010, 0x2027},   {0x2030, 0x203A},   /* dashes, quotes, bullets, the ellipsis; per mille, primes */
        {0x20AC, 0x20AC},   {0x2116, 0x2116},   /* €, № */
        {0x2122, 0x2122},   {0x2190, 0x2199},   /* ™, arrows */
        {0x2212, 0x2212},   {0x221E, 0x221E},   /* −, ∞ */
        {0x2248, 0x2248},   {0x2260, 0x2260},   /* ≈, ≠ */
        {0x2264, 0x2265},   {0x2500, 0x259F},   /* ≤ ≥; frames, blocks and shades */
        {0x25A0, 0x25A1},   {0x25AA, 0x25AB},   /* ■ □ ▪ ▫ */
        {0x25B2, 0x25B2},   {0x25B6, 0x25B6},   /* ▲ ▶ */
        {0x25BC, 0x25BC},   {0x25C0, 0x25C0},   /* ▼ ◀ */
        {0x25C6, 0x25C6},   {0x25CB, 0x25CB},   /* ◆ ○ */
        {0x25CF, 0x25CF},   {0x2713, 0x2713},   /* ● ✓ */
        {0x2717, 0x2717},   {0xFFFD, 0xFFFD},   /* ✗; what a byte that was no character is shown as */
        {BOXOS_SYMBOL_DUPPO_LEFT, BOXOS_SYMBOL_DUPPO_RIGHT},
    };
    std::vector<char32_t> Wanted;
    for (const Range &Each : Ranges)
        for (char32_t Character = Each.First; Character <= Each.Last; Character++) Wanted.push_back(Character);
    /* And every character the text page folds to a glyph of its own, so a
     * framebuffer never shows less than the text page does (‼, √, ►...). */
    for (const BoxGlyphFold &Each : BoxGlyphFoldTable) Wanted.push_back(static_cast<char32_t>(Each.Codepoint));
    std::sort(Wanted.begin(), Wanted.end());
    Wanted.erase(std::unique(Wanted.begin(), Wanted.end()), Wanted.end());
    return Wanted;
}

std::expected<MadeImprint, std::string> MakeImprint(std::vector<std::uint8_t> Bytes)
{
    std::expected<typecase::Font, typecase::Fault> Read = typecase::Font::Read(std::move(Bytes));
    if (!Read) return std::unexpected(Read.error().Said);
    std::vector<typecase::Size> Cells;
    for (const std::int32_t Height : ImprintHeights) {
        const typecase::Size At = typecase::FitCell(*Read, Height);
        if (At.Width > 255 || At.Height > 255 || At.Baseline < 0 || At.Baseline > 255)
            return std::unexpected(std::format("a cell of {}x{} is larger than an imprint holds", At.Width, At.Height));
        Cells.push_back(At);
    }

    typecase::Chain Chain;
    Chain.Add(std::make_unique<typecase::SymbolSource>());
    Chain.Add(std::make_unique<typecase::GeometrySource>());
    Chain.Add(std::make_unique<typecase::FontSource>(std::move(*Read)));

    MadeImprint Made;
    for (const char32_t Character : ImprintWanted())
        if (Chain.Has(Character)) Made.Characters.push_back(static_cast<std::uint32_t>(Character));
    for (const typecase::Size &At : Cells) {
        Made.Sizes.push_back(BoxImprintSize{static_cast<std::uint8_t>(At.Width), static_cast<std::uint8_t>(At.Height),
                                            static_cast<std::uint8_t>(At.Baseline), 0,
                                            static_cast<std::uint32_t>(Made.Pixels.size())});
        for (const std::uint32_t Character : Made.Characters) Lay(Chain.Draw(Character, At), At, Made.Pixels);
        Lay(typecase::DrawEmptyBox(At), At, Made.Pixels);
    }
    return Made;
}

std::string ImprintHeader(const MadeImprint &Made, const std::string &FontName)
{
    std::string Cells;
    for (const BoxImprintSize &Each : Made.Sizes) Cells += std::format("{}{}x{}", Cells.empty() ? "" : ", ", Each.Width, Each.Height);
    std::string Out = std::format(
        "/* boxos_imprint_data.h — the imprint (boxos_imprint.h) of\n"
        " * {}: {} characters and the empty box, in cells of {}.\n"
        " * Written by tools/imprint at every build and not kept in the tree. One file\n"
        " * of a program includes it: its arrays are that file's own. */\n"
        "#ifndef BOXOS_IMPRINT_DATA_H\n#define BOXOS_IMPRINT_DATA_H\n\n#include \"boxos_imprint.h\"\n\n",
        FontName, Made.Characters.size(), Cells);
    Out += std::format("static const uint32_t BoxImprintCharacters[{}] = {{", Made.Characters.size());
    for (std::size_t Index = 0; Index < Made.Characters.size(); Index++)
        Out += std::format("{}0x{:04X},", Index % 12 ? " " : "\n    ", Made.Characters[Index]);
    Out += std::format("\n}};\n\nstatic const BoxImprintSize BoxImprintSizes[{}] = {{\n", Made.Sizes.size());
    for (const BoxImprintSize &Each : Made.Sizes)
        Out += std::format("    {{{}, {}, {}, 0, {}}},\n", Each.Width, Each.Height, Each.Baseline, Each.Offset);
    Out += std::format("}};\n\nstatic const uint8_t BoxImprintPixels[{}] = {{", Made.Pixels.size());
    for (std::size_t Index = 0; Index < Made.Pixels.size(); Index++)
        Out += std::format("{}0x{:02X},", Index % 16 ? " " : "\n    ", Made.Pixels[Index]);
    Out += std::format("\n}};\n\nstatic const BoxImprint BoxSystemImprint = {{BoxImprintCharacters, {}, BoxImprintSizes, {}, "
                       "BoxImprintPixels}};\n\n#endif /* BOXOS_IMPRINT_DATA_H */\n",
                       Made.Characters.size(), Made.Sizes.size());
    return Out;
}
