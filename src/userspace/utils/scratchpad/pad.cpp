/* pad.cpp — the pad's notes and its keys. */

#include "pad.h"

#include <algorithm>
#include <format>
#include <utility>

namespace scratchpad {

namespace {

namespace tui = painter::tui;

// How long the pad waits for a key before it looks at its settings again.
constexpr std::uint32_t WatchMs = 250;

std::string Joined(const std::vector<std::string> &Names)
{
    std::string Out;
    for (const std::string &One : Names) Out += (Out.empty() ? "" : ", ") + One;
    return Out;
}

}  // namespace

Pad::Pad(tui::Stage Opened, std::vector<Note> Held)
    : Screen(std::move(Opened)), Shown(ReadLook()), Notes(std::move(Held))
{
    for (Note &One : Notes) One.Follow(Shown);
}

void Pad::Run()
{
    Screen.HearEnd();
    for (;;) {
        if (Watch.changed()) {
            Shown = ReadLook();
            for (Note &One : Notes) One.Follow(Shown);
        }
        Paint();
        if (!Screen.Present()) return;
        const tui::Event Heard = Screen.Wait(WatchMs);
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) {
            (void)Leave();
            return;
        }
        if (Heard.Kind == tui::EventKind::Key && !OnKey(Heard.Key)) return;
    }
}

std::vector<file_id_t> Pad::Given() const
{
    std::vector<file_id_t> Files;
    for (const Note &One : Notes)
        if (One.File() != 0) Files.push_back(One.File());
    return Files;
}

std::string Pad::Summary() const
{
    std::string Out;
    if (!Saved.empty()) Out = std::format("scratchpad: saved {}", Joined(Saved));
    std::vector<std::string> Emptied;
    for (const Note &One : Notes)
        if (One.File() != 0 && One.Text.Blank() && One.Text.Changed()) Emptied.push_back(One.Name());
    if (!Emptied.empty()) {
        Out += Out.empty() ? "scratchpad: " : "; ";
        Out += std::format("{} was emptied and is left as it was — {} trashes a note", Joined(Emptied),
                           Spelled(KeyTrash));
    }
    return Out;
}

std::string Pad::Spelled(PadKey Key) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Shown.Keys[Key], Spelling, sizeof(Spelling));
    return Spelling;
}

std::int32_t Pad::TextWidth() const
{
    return TextArea().Width;
}

bool Pad::Leave()
{
    if (Warned) return true;
    std::vector<std::string> Trouble;
    for (Note &One : Notes) {
        if (One.Text.Blank()) continue;
        const bool Writes = One.Unsaved();
        if (const std::optional<std::string> Said = One.Save()) Trouble.push_back(*Said);
        else if (Writes) Saved.push_back(One.Name());
    }
    if (Trouble.empty()) return true;
    Status = std::format("{} — {} again leaves without saving", Trouble.front(), "esc");
    Warned = true;
    return false;
}

void Pad::Turn(std::ptrdiff_t By)
{
    const auto Last = static_cast<std::ptrdiff_t>(Notes.size()) - 1;
    At              = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(At) + By, 0, Last));
}

void Pad::NewPaper()
{
    Here().Repaint(OtherSheet(Shown, Here().Paints()));
}

void Pad::TrashHere()
{
    Now = Asking::Nothing;
    if (const std::optional<std::string> Trouble = Here().Trash()) {
        Status = *Trouble;
        return;
    }
    Notes.erase(Notes.begin() + static_cast<std::ptrdiff_t>(At));
    if (At >= Notes.size() && At > 0) At--;
    Status = "the note is in the trash";
}

void Pad::OpenAll()
{
    const std::expected<std::vector<file_id_t>, std::string> Found = AllNotes();
    if (!Found) {
        Status = Found.error();
        return;
    }
    std::vector<Note> Brought;
    for (const file_id_t File : Found.value()) {
        const bool Open = std::any_of(Notes.begin(), Notes.end(), [&](const Note &One) { return One.File() == File; });
        if (Open) continue;
        std::expected<Note, std::string> Read = Note::Read(File, Shown);
        if (Read) Brought.push_back(std::move(*Read));
        else Status = Read.error();
    }
    if (Brought.empty() && Status.empty()) {
        Status = "there is no other note";
        return;
    }
    // An empty new note gives its place to the notes brought.
    if (!Brought.empty() && Notes.size() == 1 && Here().File() == 0 && Here().Text.Blank()) Notes.clear();
    At = Notes.size();
    for (Note &One : Brought) Notes.push_back(std::move(One));
    if (At >= Notes.size()) At = Notes.size() - 1;
    if (Status.empty()) Status = std::format("{} notes", Notes.size());
}

bool Pad::OnKey(const tui::KeyEvent &Key)
{
    if (Now == Asking::Help) {
        Now = Asking::Nothing;
        return true;
    }
    if (Now == Asking::Trash) {
        if (!Key.IsExtended() && !Key.IsCtrl() && (Key.Ascii == 'y' || Key.Ascii == 'Y')) {
            TrashHere();
            return !Notes.empty();
        }
        Now    = Asking::Nothing;
        Status = "the note stays";
        return true;
    }
    const bool Leaving = !Key.IsExtended() && Key.Ascii == tui::KeyCodes::Escape;
    if (!Leaving) Warned = false;
    Status.clear();
    return OnWriting(Key);
}

bool Pad::OnWriting(const tui::KeyEvent &Key)
{
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const auto       Is = [&](PadKey Which) { return KeyChordMatches(&Shown.Keys[Which], &Raw); };
    if (Is(KeyHelp)) {
        Now = Asking::Help;
    } else if (Is(KeyColor)) {
        NewPaper();
    } else if (Is(KeyTrash)) {
        Now = Asking::Trash;
    } else if (Is(KeyAll)) {
        OpenAll();
    } else if (!Key.IsExtended() && !Key.IsCtrl() && !Key.IsAlt() && Key.Ascii == tui::KeyCodes::Escape) {
        return !Leave();
    } else {
        Write(Key);
    }
    return true;
}

void Pad::Write(const tui::KeyEvent &Key)
{
    NoteText &Text = Here().Text;
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:     Text.Left(); break;
        case tui::KeyCodes::Right:    Text.Right(); break;
        case tui::KeyCodes::Up:       Text.Up(TextWidth()); break;
        case tui::KeyCodes::Down:     Text.Down(TextWidth()); break;
        case tui::KeyCodes::Home:     Text.RowStart(TextWidth()); break;
        case tui::KeyCodes::End:      Text.RowEnd(TextWidth()); break;
        case tui::KeyCodes::Delete:   Text.Delete(); break;
        case tui::KeyCodes::PageUp:   Turn(-1); break;
        case tui::KeyCodes::PageDown: Turn(1); break;
        default:                      break;
        }
        return;
    }
    if (Key.IsCtrl() || Key.IsAlt()) return;
    bool Room = true;
    if (Key.Ascii == tui::KeyCodes::Enter || Key.Ascii == '\r') Room = Text.NewLine();
    else if (Key.Ascii == tui::KeyCodes::Backspace || Key.Ascii == 0x7F) Text.Backspace();
    else if (Key.Ascii >= 0x20 && Key.Ascii < 0x7F) Room = Text.Type(Key.Ascii);
    if (!Room) Status = "the note is full — it is for a few lines";
}

}  // namespace scratchpad
