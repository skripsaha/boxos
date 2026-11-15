/* typecase stand — the imprint held to boxos_imprint.h and imprint_make.h:
 * what it holds and leaves out, every glyph at every size the chain's picture
 * to within half a step, the empty box last, the reader finding and choosing
 * as it says, and ImprintSource drawing it in the middle of a larger cell.
 * The digest is said here and by a program built on the header tools/imprint
 * writes (typecase_stand_imprint.c), and typecase_stand.sh holds the two
 * alike — the header is what the kernel and the display take. */

#include <cstdint>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "boxos_symbols.h"
#include "imprint_make.h"
#include "typecase/chain.h"
#include "typecase/code_box.h"
#include "typecase/font_source.h"
#include "typecase/geometry_source.h"
#include "typecase/imprint_source.h"
#include "typecase/size.h"
#include "typecase/symbol_source.h"
#include "typecase_stand.h"

namespace stand {

namespace {

/* The pixel X, Y of a cell At as Picture shows it from the cell's baseline. */
int Shown(const typecase::GlyphImage &Picture, const typecase::Size &At, std::int32_t X, std::int32_t Y)
{
    const std::int32_t Across = X - Picture.Left, Down = Y - (At.Baseline + Picture.Top);
    if (Across < 0 || Across >= Picture.Width || Down < 0 || Down >= Picture.Height) return 0;
    return Picture.Coverage[static_cast<std::size_t>(Down * Picture.Width + Across)];
}

/* Whether glyph Glyph at size Size, a cell At, is Picture to within half a
 * step. */
bool Alike(const BoxImprint &Of, std::uint32_t Size, std::uint32_t Glyph, const typecase::Size &At,
           const typecase::GlyphImage &Picture)
{
    const BoxImprintSize &Cell = Of.Sizes[Size];
    for (std::int32_t Y = 0; Y < Cell.Height; Y++)
        for (std::int32_t X = 0; X < Cell.Width; X++) {
            const int Step = static_cast<int>(BoxImprintShade(&Of, Size, Glyph, static_cast<std::uint32_t>(X),
                                                              static_cast<std::uint32_t>(Y))) * 85;
            if (std::abs(Step - Shown(Picture, At, X, Y)) > 43) return false;
        }
    return true;
}

}  // namespace

void SayImprintDigest(const BoxImprint &Of)
{
    std::printf("imprint %u", Of.Count);
    for (std::uint32_t Size = 0; Size < Of.SizeCount; Size++) {
        const BoxImprintSize &Cell = Of.Sizes[Size];
        unsigned long long    Sum  = 0;
        for (std::uint32_t Glyph = 0; Glyph <= Of.Count; Glyph++)
            for (std::uint32_t Y = 0; Y < Cell.Height; Y++)
                for (std::uint32_t X = 0; X < Cell.Width; X++)
                    Sum += BoxImprintShade(&Of, Size, Glyph, X, Y) * (Glyph % 7 + 1) * (X + 2 * Y + 1);
        std::printf(" %ux%u+%u@%u:%llu", Cell.Width, Cell.Height, Cell.Baseline, Cell.Offset, Sum);
    }
    std::printf(" find %u %u %u\n", BoxImprintFind(&Of, 0x0416), BoxImprintFind(&Of, BOXOS_SYMBOL_DUPPO_RIGHT),
                BoxImprintFind(&Of, 0x4E00));
}

void CheckImprint(Tally &Count, const std::vector<std::uint8_t> &Font)
{
    const auto Made = MakeImprint(Font);
    if (!Made) return Count.Check(false, "the imprint is made: " + Made.error());
    const BoxImprint Of = Made->View();

    auto Read = typecase::Font::Read(Font);
    typecase::Chain Chain;
    Chain.Add(std::make_unique<typecase::SymbolSource>());
    Chain.Add(std::make_unique<typecase::GeometrySource>());
    Chain.Add(std::make_unique<typecase::FontSource>(std::move(*Read)));

    bool Kept = std::is_sorted(Made->Characters.begin(), Made->Characters.end()) &&
                std::adjacent_find(Made->Characters.begin(), Made->Characters.end()) == Made->Characters.end();
    for (const char32_t Wanted : ImprintWanted())
        Kept = Kept && (BoxImprintFind(&Of, Wanted) < Of.Count) == Chain.Has(Wanted);
    for (const char32_t Must : {U'A', U'ж', U'Ω', U'─', U'█', U'░', U'→', U'—', U'�', char32_t{BOXOS_SYMBOL_DUPPO_LEFT}})
        Kept = Kept && BoxImprintFind(&Of, Must) < Of.Count;
    Count.Check(Kept, "the imprint holds, in order and once each, every character it is asked for that the chain "
                      "draws, and no other — Latin, Cyrillic, Greek, frames, blocks, arrows, U+FFFD, Duppo among them (" +
                          std::to_string(Of.Count) + ")");

    std::vector<typecase::Size> Cells;
    for (const std::int32_t Height : ImprintHeights) Cells.push_back(typecase::FitCell(*typecase::Font::Read(Font), Height));
    bool          Laid = Of.SizeCount == Cells.size();
    std::uint32_t Next = 0;
    for (std::uint32_t Size = 0; Laid && Size < Of.SizeCount; Size++) {
        const BoxImprintSize &Cell = Of.Sizes[Size];
        const typecase::Size &At   = Cells[Size];
        Laid = Cell.Width == At.Width && Cell.Height == At.Height && Cell.Baseline == At.Baseline && Cell.Offset == Next;
        Next += (Of.Count + 1) * ((static_cast<std::uint32_t>(Cell.Width) * Cell.Height * 2 + 7) / 8);
    }
    Count.Check(Laid && Next == Made->Pixels.size(),
                "its sizes are the font's cells 16, 24 and 32 tall, each a glyph for every character and the empty "
                "box, laid one after another");

    bool Drawn = true, Boxed = true;
    for (std::uint32_t Size = 0; Laid && Size < Of.SizeCount; Size++) {
        const typecase::Size &At = Cells[Size];
        for (std::uint32_t Glyph = 0; Glyph < Of.Count && Drawn; Glyph++) {
            Drawn = Alike(Of, Size, Glyph, At, Chain.Draw(Of.Characters[Glyph], At));
            if (!Drawn) std::printf("  U+%04X at %dx%d\n", Of.Characters[Glyph], At.Width, At.Height);
        }
        Boxed = Boxed && Alike(Of, Size, Of.Count, At, typecase::DrawEmptyBox(At));
    }
    Count.Check(Drawn, "every glyph at every size is the chain's picture of its character in the cell, to within half "
                       "of one of its four steps");
    Count.Check(Boxed, "after the characters, at every size, the empty box");

    const bool Found = BoxImprintFind(&Of, 0x4E00) == Of.Count && BoxImprintFind(&Of, 0) == Of.Count &&
                       BoxImprintFind(&Of, 0x10FFFF) == Of.Count && BoxImprintFind(&Of, Of.Characters[0]) == 0 &&
                       BoxImprintFind(&Of, Of.Characters[Of.Count - 1]) == Of.Count - 1;
    const bool Chosen = BoxImprintSizeFor(&Of, 10) == 0 && BoxImprintSizeFor(&Of, 23) == 0 &&
                        BoxImprintSizeFor(&Of, 24) == 1 && BoxImprintSizeFor(&Of, 31) == 1 &&
                        BoxImprintSizeFor(&Of, 32) == 2 && BoxImprintSizeFor(&Of, 200) == 2;
    Count.Check(Found && Chosen, "the reader finds the first and the last character, and the empty box for one it "
                                 "lacks; it takes the largest cell no taller than asked, the smallest when none is");

    typecase::ImprintSource Source(Of);
    const std::uint32_t     A = BoxImprintFind(&Of, U'A');
    const auto Exact = Source.Draw(U'A', typecase::Size{0, 7, 16, 12}), Wider = Source.Draw(U'A', typecase::Size{0, 9, 20, 15});
    const auto Larger = Source.Draw(U'A', typecase::Size{0, 12, 26, 20}), Smaller = Source.Draw(U'A', typecase::Size{0, 5, 10, 8});
    bool Centred = Exact && Wider && Larger && Smaller && Source.Has(U'A') && !Source.Has(U'一') &&
                   !Source.Draw(U'一', typecase::Size{0, 7, 16, 12});
    for (std::uint32_t Y = 0; Centred && Y < 16; Y++)
        for (std::uint32_t X = 0; X < 7; X++) {
            const int Value = static_cast<int>(BoxImprintShade(&Of, 0, A, X, Y)) * 85;
            Centred = Exact->Coverage[Y * 7 + X] == Value && Wider->Coverage[(Y + 2) * 9 + X + 1] == Value;
            if (X >= 1 && X < 6 && Y >= 3 && Y < 13) Centred = Centred && Smaller->Coverage[(Y - 3) * 5 + X - 1] == Value;
        }
    for (std::uint32_t Y = 0; Centred && Y < 24; Y++)
        for (std::uint32_t X = 0; X < 11; X++)
            Centred = Larger->Coverage[(Y + 1) * 12 + X] == static_cast<int>(BoxImprintShade(&Of, 1, A, X, Y)) * 85;
    Count.Check(Centred, "ImprintSource draws a glyph of the largest size the cell holds, in its middle — 7x16 in "
                         "9x20, 11x24 in 12x26, 7x16 cut to 5x10 — and has only what the imprint has");
}

}  // namespace stand
