/* One line typed into a window. */

#include "field.h"

#include "clipboard.h"
#include "glyph.h"
#include "look.h"

namespace kin {

namespace {

/* A field holds no tab and no control character, so a glyph's shape in it
 * does not hang on its column. */
Glyph GlyphOf(std::string_view Words, std::size_t Byte) noexcept
{
    return GlyphAt(Words, Byte, 0);
}

/* Where the glyph that ends at Byte begins. */
std::size_t GlyphBefore(std::string_view Words, std::size_t Byte) noexcept
{
    std::size_t At = 0;
    while (At < Byte) {
        const std::size_t Next = At + GlyphOf(Words, At).Length;
        if (Next >= Byte) return At;
        At = Next;
    }
    return 0;
}

}  // namespace

void Field::Set(std::string_view Value)
{
    Words   = Value;
    Caret   = Words.size();
    Offered = false;
}

void Field::Offer(std::string_view Value)
{
    Set(Value);
    Offered = !Words.empty();
}

void Field::Clear() noexcept
{
    Words.clear();
    Caret   = 0;
    Offered = false;
}

void Field::Put(std::string_view Bytes)
{
    Words.insert(Caret, Bytes);
    Caret += Bytes.size();
}

bool Field::OnKey(const tui::KeyEvent &Key)
{
    const bool WasOffered = Offered;
    Offered = false;
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:   Caret = GlyphBefore(Words, Caret); return true;
        case tui::KeyCodes::Right:  if (Caret < Words.size()) Caret += GlyphOf(Words, Caret).Length; return true;
        case tui::KeyCodes::Home:   Caret = 0; return true;
        case tui::KeyCodes::End:    Caret = Words.size(); return true;
        case tui::KeyCodes::Delete:
            if (Caret < Words.size()) Words.erase(Caret, GlyphOf(Words, Caret).Length);
            return true;
        default: Offered = WasOffered; return false;
        }
    }
    /* Ctrl+V, as everywhere: the newest clip's first line, put where the
     * caret is — over an offered value, as typing is. */
    if (Key.IsCtrl() && !Key.IsShift() && !Key.IsAlt() && (Key.Ascii == 'v' || Key.Ascii == 'V')) {
        if (std::expected<std::optional<Taken>, std::string> Newest = TakeNewest(); Newest && *Newest) {
            if (WasOffered) Clear();
            Put(FirstLine((*Newest)->Bytes));
        } else {
            Offered = WasOffered;
        }
        return true;
    }
    if (Key.IsCtrl() || Key.IsAlt()) {
        Offered = WasOffered;
        return false;
    }
    if (Key.Ascii == tui::KeyCodes::Backspace || Key.Ascii == 0x7F) {
        if (WasOffered) {
            Clear();
        } else if (Caret > 0) {
            const std::size_t From = GlyphBefore(Words, Caret);
            Words.erase(From, Caret - From);
            Caret = From;
        }
        return true;
    }
    if (Key.Ascii >= 0x20 && Key.Ascii < 0x7F) {
        if (WasOffered) Clear();
        Put(std::string_view(&Key.Ascii, 1));
        return true;
    }
    Offered = WasOffered;
    return false;
}

/* A value wider than the field is shown from where the caret can be seen:
 * the glyphs before the caret, as many as leave room for it. */
void Field::Paint(tui::Frame &Out, core::Point At, std::int32_t Width, bool Listening) const
{
    std::int32_t Col = At.X;
    if (!Label.empty()) Col += PutText(Out, {Col, At.Y}, Label, Width, HintInk, WindowPaper) + 1;
    const std::int32_t Room = At.X + Width - Col;
    if (Room <= 0) return;

    Out.Fill({Col, At.Y, Room, 1}, tui::Cell{' ', FieldInk, FieldPaper});
    const auto  Visible = static_cast<std::size_t>(Room - 1);
    std::size_t From    = 0;
    while (CellsOf(std::string_view(Words).substr(From, Caret - From)) > Visible)
        From += GlyphOf(Words, From).Length;
    PutText(Out, {Col, At.Y}, std::string_view(Words).substr(From), Room, FieldInk,
            Offered ? SelectedPaper : FieldPaper);
    if (Listening) {
        const auto Before = static_cast<std::int32_t>(CellsOf(std::string_view(Words).substr(From, Caret - From)));
        (void)Out.SetCaret(core::Point{Col + Before, At.Y});
    }
}

}  // namespace kin
