/* slots.cpp — the word's slots and their letters (slots.h). */

#include "slots.h"

#include <algorithm>
#include <cstddef>

#include "painter/tui/glyph.h"

#include "lettering.h"

namespace hangman {

namespace {

namespace G = painter::tui::Glyphs;

constexpr std::uint64_t RiseMs = 240;
constexpr std::uint64_t GlowMs = 520;
constexpr std::uint64_t RevealGapMs = 120;
constexpr std::int32_t  BigPitch = LetterWide + 1;   // cells from one big letter to the next

// How one place of the word looks now: nothing yet, or its letter, in a colour that warms from the
// flash of being found, and how much of the way it has risen into view (1 when it has).
struct SlotLook {
    bool  Written = false;
    char  Letter = 0;
    Color Rule;
    Color Colour;
    float Risen = 1.0f;
};

SlotLook SlotLookOf(const Facts &Now, std::size_t Place)
{
    const Round &Word = Now.Playing;
    const Look  &Shown = Now.Shown;
    SlotLook     Seen{false, Word.Word()[Place], Shown.Quiet, Shown.Text, 1.0f};
    if (Word.Shown(Place) && Now.Lit[Place] <= Now.Now) {
        const float Age = static_cast<float>(Since(Now, Now.Lit[Place]));
        const Color Warm = Now.Given.test(static_cast<std::size_t>(Seen.Letter - 'A')) ? Shown.Title : Shown.Good;
        Seen.Written = true;
        Seen.Rule = Warm;
        Seen.Colour = Mixed(Mixed(Shown.Text, Warm, 0.3f), Lighter(Warm, 0.75f), 1.0f - Clamped(Age / static_cast<float>(GlowMs)));
        Seen.Risen = Clamped(Age / static_cast<float>(RiseMs));
    } else if (Word.State() == Phase::Lost && !Word.Shown(Place) && Since(Now, Now.EndedAt) >= RevealGapMs * (Place + 1)) {
        Seen.Written = true;
        Seen.Rule = Shown.Bad;
        Seen.Colour = Shown.Bad;
    }
    return Seen;
}

// A letter of five by seven pixels over a rule, rising from the rule when it is found. The cells are
// primed to the paper first, for a pixel changes only its half of a cell.
void PaintBigSlot(const Pen &Page, const Facts &Now, std::size_t Place, std::int32_t X, std::int32_t Row)
{
    const SlotLook Seen = SlotLookOf(Now, Place);
    Page.Out.Fill({X, Row, LetterWide, SlotRows}, G::UpperHalf, {Page.Paper, Page.Paper});
    if (Seen.Written)
        PaintLetter(Page.Out, Seen.Letter, {X, Row * 2 + 1}, std::max(1, static_cast<std::int32_t>(Seen.Risen * static_cast<float>(LetterHigh))), Seen.Colour);
    for (std::int32_t Across = 0; Across < LetterWide; Across++) Page.Out.PutPixel({X + Across, Row * 2 + 9}, Seen.Rule);
}

void PaintPlainSlot(const Pen &Page, const Facts &Now, std::size_t Place, std::int32_t X, std::int32_t Row)
{
    const SlotLook Seen = SlotLookOf(Now, Place);
    if (Seen.Written) Page.Out.Put({X, Row + 3}, Seen.Letter, {Seen.Colour, Page.Paper});
    Page.Out.Put({X, Row + 4}, G::UpperHalf, {Seen.Rule, Page.Paper});
}

}  // namespace

void PaintSlots(const Pen &Page, const Facts &Now, std::int32_t Row)
{
    const std::int32_t Length = static_cast<std::int32_t>(Now.Playing.Word().size());
    const bool         Big = Length * BigPitch - 1 <= Page.Area.Width;
    const std::int32_t Pitch = Big ? BigPitch : (Length * 3 <= Page.Area.Width ? 3 : 2);
    const std::int32_t Total = Length * Pitch - (Big ? 1 : Pitch - 1);
    const std::int32_t Start = Page.Area.X + std::max(0, (Page.Area.Width - Total) / 2);
    for (std::int32_t Place = 0; Place < Length; Place++) {
        if (Big) PaintBigSlot(Page, Now, static_cast<std::size_t>(Place), Start + Place * Pitch, Row);
        else PaintPlainSlot(Page, Now, static_cast<std::size_t>(Place), Start + Place * Pitch, Row);
    }
}

}  // namespace hangman
