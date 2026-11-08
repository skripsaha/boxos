/* One line typed into a window. */

#include "field.h"

#include <algorithm>

#include "paint.h"

namespace edit {

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

bool Field::OnKey(const tui::KeyEvent &Key)
{
    const bool WasOffered = Offered;
    Offered = false;
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:   if (Caret > 0) Caret--; return true;
        case tui::KeyCodes::Right:  if (Caret < Words.size()) Caret++; return true;
        case tui::KeyCodes::Home:   Caret = 0; return true;
        case tui::KeyCodes::End:    Caret = Words.size(); return true;
        case tui::KeyCodes::Delete: if (Caret < Words.size()) Words.erase(Caret, 1); return true;
        default:                    Offered = WasOffered; return false;
        }
    }
    if (Key.IsCtrl() || Key.IsAlt()) {
        Offered = WasOffered;
        return false;
    }
    if (Key.Ascii == tui::KeyCodes::Backspace || Key.Ascii == 0x7F) {
        if (WasOffered) Clear();
        else if (Caret > 0) Words.erase(--Caret, 1);
        return true;
    }
    if (Key.Ascii >= 0x20 && Key.Ascii < 0x7F) {
        if (WasOffered) Clear();
        Words.insert(Caret++, 1, Key.Ascii);
        return true;
    }
    Offered = WasOffered;
    return false;
}

/* A value wider than the field is shown from where the caret can be seen. */
void Field::Paint(tui::Frame &Out, core::Point At, std::int32_t Width, bool Listening) const
{
    std::int32_t Col = At.X;
    if (!Label.empty()) Col += PutText(Out, {Col, At.Y}, Label, Width, HintInk, WindowPaper) + 1;
    const std::int32_t Room = At.X + Width - Col;
    if (Room <= 0) return;

    Out.Fill({Col, At.Y, Room, 1}, tui::Cell{' ', FieldInk, FieldPaper});
    const auto        Visible = static_cast<std::size_t>(Room - 1);
    const std::size_t From    = Caret > Visible ? Caret - Visible : 0;
    PutText(Out, {Col, At.Y}, std::string_view(Words).substr(From), Room, FieldInk,
            Offered ? SelectedPaper : FieldPaper);
    if (Listening) (void)Out.SetCaret(core::Point{Col + static_cast<std::int32_t>(Caret - From), At.Y});
}

}  // namespace edit
