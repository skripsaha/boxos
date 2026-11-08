/* The file's card. */

#include "card.h"

#include <algorithm>
#include <format>

#include "volume.h"

namespace kin {

namespace {

/* The list of tags under the card: eight of them and its frame, or what room
 * the glass has below the card. */
constexpr std::int32_t HintsHeight = 10;

std::string Trimmed(std::string_view Text)
{
    while (!Text.empty() && Text.front() == ' ') Text.remove_prefix(1);
    while (!Text.empty() && Text.back() == ' ') Text.remove_suffix(1);
    return std::string(Text);
}

/* Where the word being typed begins: past the last blank or comma. */
std::size_t WordStart(std::string_view Text)
{
    const std::size_t Cut = Text.find_last_of(" ,");
    return Cut == std::string_view::npos ? 0 : Cut + 1;
}

}  // namespace

void Card::Begin(file_id_t FileValue, std::string Name, std::string Tags, Scope LookingValue, core::Size ScreenValue)
{
    File    = FileValue;
    Called  = std::move(Name);
    Worn    = std::move(Tags);
    Screen  = ScreenValue;
    Looking = LookingValue;
    OnTags  = false;
    Trouble.clear();
    Had = TagWords(Worn);
    NameField.Set(Called);
    TagsField.Set(Joined(Had, ", "));
    Hints.Close();
}

void Card::Rescope(Scope LookingValue)
{
    Looking = LookingValue;
    if (!Hints.Open()) return;
    Hints.Begin(Looking);
    Offer();
}

/* The list narrowed to the word being typed, the words before it left out. */
void Card::Offer()
{
    const std::string_view Text = TagsField.Value();
    const std::size_t      From = WordStart(Text);
    Hints.Narrow(Text.substr(From), TagWords(Text.substr(0, From)));
}

/* The chosen tag in place of the word being typed, and room for the next. */
void Card::Take(const std::string &Tag)
{
    const std::string &Text = TagsField.Value();
    TagsField.Set(Text.substr(0, WordStart(Text)) + Tag + ", ");
}

/* What changed between the tags the card was opened with and those it holds,
 * then the name. A refusal leaves what was typed to be put right, and the next
 * Enter is judged against what the file wears then. */
bool Card::Keep(std::string &Said)
{
    const std::string Named = Trimmed(NameField.Value());
    if (Named.empty()) {
        Trouble = "a file needs a name";
        return false;
    }
    std::optional<std::string> Refused = Retag(File, Had, TagWords(TagsField.Value()));
    if (!Refused && Named != Called) Refused = Rename(File, Named);
    if (std::expected<FileFacts, std::string> Facts = ReadFacts(File)) {
        Called = Facts->Name;
        Worn   = Facts->Tags;
    }
    Had = TagWords(Worn);
    if (Refused) {
        Trouble = *Refused;
        return false;
    }
    Said = Had.empty() ? std::format("{} wears no tags", Called) : std::format("{} wears {}", Called, Joined(Had, ", "));
    return true;
}

Card::Outcome Card::OnKey(const tui::KeyEvent &Key, std::string &Said)
{
    if (Hints.Open()) {
        switch (Hints.OnKey(Key)) {
        case Suggest::Outcome::Stay:
        case Suggest::Outcome::Closed: return Outcome::Stay;
        case Suggest::Outcome::Chosen: Take(Hints.Chosen()); return Outcome::Stay;
        case Suggest::Outcome::Passed: break;
        }
    }

    const char Ch    = Key.Ascii;
    const bool Plain = !Key.IsExtended() && !Key.IsCtrl() && !Key.IsAlt();
    if (Plain && Ch == tui::KeyCodes::Escape) return Outcome::Closed;
    if (Plain && Ch == tui::KeyCodes::Tab) {
        if (!OnTags) {
            OnTags = true;
        } else if (Hints.Open()) {
            Hints.Close();
        } else {
            Hints.Begin(Looking);
            Offer();
        }
        return Outcome::Stay;
    }
    if (Key.IsExtended() && (Key.Scancode == tui::KeyCodes::Up || Key.Scancode == tui::KeyCodes::Down)) {
        OnTags = Key.Scancode == tui::KeyCodes::Down;
        Hints.Close();
        return Outcome::Stay;
    }
    if (Plain && (Ch == tui::KeyCodes::Enter || Ch == '\r')) {
        Trouble.clear();
        return Keep(Said) ? Outcome::Done : Outcome::Stay;
    }
    if ((OnTags ? TagsField : NameField).OnKey(Key) && OnTags && Hints.Open()) Offer();
    return Outcome::Stay;
}

core::Rect Card::Area() const noexcept
{
    return Centred(Screen, std::min(Screen.Width - 4, 64), 5);
}

void Card::Paint(tui::Frame &Out) const
{
    const core::Rect Box    = Area();
    const core::Rect Inside = DrawWindow(Out, Box, "The file's card", "",
                                         OnTags ? "Enter keep  Tab the tags there are  Esc cancel"
                                                : "Enter keep  Tab to the tags  Esc cancel");
    NameField.Paint(Out, {Inside.X, Inside.Y}, Inside.Width, !OnTags);
    TagsField.Paint(Out, {Inside.X, Inside.Y + 1}, Inside.Width, OnTags);
    if (!Trouble.empty())
        PutText(Out, {Inside.X, Inside.Y + 2}, Trouble, Inside.Width, TroubleInk, WindowPaper);
    else
        PutText(Out, {Inside.X, Inside.Y + 2}, "tags go between blanks or commas: work, idea", Inside.Width, HintInk,
                WindowPaper);

    if (!Hints.Open()) return;
    const std::int32_t Top    = Box.Y + Box.Height;
    const std::int32_t Height = std::min(HintsHeight, Screen.Height - Top - 1);
    if (Height >= 3) Hints.Paint(Out, {Box.X, Top, Box.Width, Height});
}

}  // namespace kin
