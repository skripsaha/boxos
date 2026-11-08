/* The file's card, and a tab's text put into its file. */

#include "card.h"

#include <algorithm>
#include <format>

#include "volume.h"

namespace edit {

namespace {

std::string Trimmed(std::string_view Text)
{
    while (!Text.empty() && Text.front() == ' ') Text.remove_prefix(1);
    while (!Text.empty() && Text.back() == ' ') Text.remove_suffix(1);
    return std::string(Text);
}

/* The name and the tags as the volume now holds them. */
void Refresh(Tab &For)
{
    if (std::expected<FileFacts, std::string> Facts = ReadFacts(For.File)) {
        For.Name = Facts->Name;
        For.Tags = Facts->Tags;
    }
}

}  // namespace

bool SaveTab(Tab &For, std::string &Said)
{
    const std::string Bytes = For.Body.ToBytes();
    if (std::optional<std::string> Refused = WriteBytes(For.File, Bytes)) {
        Said = std::format("{} was not saved: {}", For.Name, *Refused);
        return false;
    }
    For.MarkSaved();
    Said = std::format("saved {}: {} line{}, {} byte{}", For.Name, For.Body.LineCount(),
                       For.Body.LineCount() == 1 ? "" : "s", Bytes.size(), Bytes.size() == 1 ? "" : "s");
    return true;
}

void Card::Begin(const Tab &For, Then After, core::Size ScreenValue)
{
    Screen  = ScreenValue;
    Next    = After;
    OnTags  = For.File == 0;
    Context = For.File == 0 ? ContextTags() : std::string();
    Trouble.clear();
    Name.Set(For.Name);
    Tags.Set(Joined(ShownTags(For.Tags, For.Name), ", "));
}

/* A file born: made with its name and tags, then its text written. */
bool Card::Make(Tab &For, std::string &Said)
{
    const std::string Named = Trimmed(Name.Value());
    if (Named.empty()) {
        Trouble = "a file needs a name";
        return false;
    }
    std::expected<file_id_t, std::string> Made = MakeFile(Named, Joined(TagWords(Tags.Value()), ","));
    if (!Made) {
        Trouble = Made.error();
        return false;
    }
    For.File = *Made;
    For.Name = Named;
    Refresh(For);
    return SaveTab(For, Said);
}

/* A file renamed and retagged: the name first, as the volume holds the tag
 * for a name with the name. */
bool Card::Change(Tab &For, std::string &Said)
{
    const std::string Named = Trimmed(Name.Value());
    if (Named.empty()) {
        Trouble = "a file needs a name";
        return false;
    }
    if (Named != For.Name) {
        if (std::optional<std::string> Refused = Rename(For.File, Named)) {
            Trouble = *Refused;
            return false;
        }
        Refresh(For);
    }
    std::optional<std::string> Refused = Retag(For.File, For.Name, For.Tags, TagWords(Tags.Value()));
    Refresh(For);
    if (Refused) {
        Trouble = *Refused;
        Tags.Set(Joined(ShownTags(For.Tags, For.Name), ", "));
        return false;
    }
    const std::vector<std::string> Worn = ShownTags(For.Tags, For.Name);
    Said = Worn.empty() ? std::format("{} wears no tags", For.Name)
                        : std::format("{} wears {}", For.Name, Joined(Worn, ", "));
    return true;
}

Card::Outcome Card::OnKey(const tui::KeyEvent &Key, Tab &For, std::string &Said)
{
    const char Ch = Key.Ascii;
    if (!Key.IsExtended() && Ch == tui::KeyCodes::Escape) return Outcome::Closed;
    if (!Key.IsExtended() && Ch == tui::KeyCodes::Tab) {
        OnTags = !OnTags;
        return Outcome::Stay;
    }
    if (Key.IsExtended() && (Key.Scancode == tui::KeyCodes::Up || Key.Scancode == tui::KeyCodes::Down)) {
        OnTags = Key.Scancode == tui::KeyCodes::Down;
        return Outcome::Stay;
    }
    if (!Key.IsExtended() && (Ch == tui::KeyCodes::Enter || Ch == '\r')) {
        Trouble.clear();
        if (For.File != 0) return Change(For, Said) ? Outcome::Done : Outcome::Stay;
        /* Made but not written: the card has done what it can, and what was to
         * follow it — a tab closed, edit left — must not. */
        if (!Make(For, Said)) return For.File == 0 ? Outcome::Stay : Outcome::Closed;
        return Outcome::Done;
    }
    (void)(OnTags ? Tags : Name).OnKey(Key);
    return Outcome::Stay;
}

void Card::Paint(tui::Frame &Out, const Tab &For) const
{
    const bool       Born  = For.File == 0;
    const core::Rect Area  = Centred(Screen, std::min(Screen.Width - 4, 64), 5);
    const core::Rect Inside = DrawWindow(Out, Area, Born ? "Save a new file" : "The file's card", Born ? "new" : "",
                                         Born ? "Enter save  Tab next  Esc cancel" : "Enter keep  Tab next  Esc cancel");
    Name.Paint(Out, {Inside.X, Inside.Y}, Inside.Width, !OnTags);
    Tags.Paint(Out, {Inside.X, Inside.Y + 1}, Inside.Width, OnTags);
    if (!Trouble.empty())
        PutText(Out, {Inside.X, Inside.Y + 2}, Trouble, Inside.Width, TroubleInk, WindowPaper);
    else if (Born && !Context.empty())
        PutText(Out, {Inside.X, Inside.Y + 2}, "the Use Context adds " + Context, Inside.Width, HintInk, WindowPaper);
    else
        PutText(Out, {Inside.X, Inside.Y + 2}, "tags go between blanks or commas: work, idea", Inside.Width, HintInk,
                WindowPaper);
}

}  // namespace edit
