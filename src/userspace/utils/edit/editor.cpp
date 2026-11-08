/* edit as a whole: the keys, the tabs and their files, and leaving. */

#include "editor.h"

#include <algorithm>
#include <charconv>
#include <format>

#include "volume.h"

namespace edit {

Editor::Editor(tui::Stage ScreenValue) : Screen(std::move(ScreenValue)), Bounds(Screen.Size()) {}

/* ---- tabs and files ----------------------------------------------------- */

void Editor::Remember(file_id_t File)
{
    if (File != 0 && std::find(Touched.begin(), Touched.end(), File) == Touched.end()) Touched.push_back(File);
}

void Editor::Note(std::string Words, bool Trouble)
{
    Message        = std::move(Words);
    MessageTrouble = Trouble;
}

std::size_t Editor::IndexOf(const Tab *Which) const noexcept
{
    for (std::size_t Index = 0; Index < Tabs.size(); Index++)
        if (Tabs[Index].get() == Which) return Index;
    return Current;
}

void Editor::Open(file_id_t File)
{
    for (std::size_t Index = 0; Index < Tabs.size(); Index++) {
        if (Tabs[Index]->File != File) continue;
        Current = Index;
        return;
    }

    std::expected<FileFacts, std::string> Facts = ReadFacts(File);
    if (!Facts) {
        Note(std::format("that file would not open: {}", Facts.error()), true);
        return;
    }
    std::expected<std::string, std::string> Bytes = ReadBytes(File, Facts->Size);
    if (!Bytes) {
        Note(std::format("{} would not open: {}", Facts->Name, Bytes.error()), true);
        return;
    }

    Tabs.push_back(std::make_unique<Tab>(Facts->Name, File, Facts->Tags, Text::FromBytes(*Bytes)));
    Current = Tabs.size() - 1;
    Remember(File);
    /* A zero byte is no text anybody typed: what such a file holds is looked
     * at, not written by accident. */
    if (Bytes->find('\0') != std::string::npos) {
        Active().Safety = true;
        Note(std::format("{} holds bytes that are no text: it is open with SAFETY on", Facts->Name));
    }
}

void Editor::OpenNew(std::string Name)
{
    for (std::size_t Index = 0; Index < Tabs.size(); Index++) {
        if (Tabs[Index]->File != 0 || Tabs[Index]->Name != Name) continue;
        Current = Index;
        return;
    }
    Tabs.push_back(std::make_unique<Tab>(std::move(Name), 0, std::string(), Text()));
    Current = Tabs.size() - 1;
}

void Editor::Close(std::size_t Index)
{
    Remember(Tabs[Index]->File);
    Tabs.erase(Tabs.begin() + static_cast<std::ptrdiff_t>(Index));
    if (Current >= Index && Current > 0) Current--;
    Now = Mode::Writing;
    if (Tabs.empty()) OpenChooser();
}

void Editor::OpenChooser()
{
    std::vector<file_id_t> Open;
    for (const std::unique_ptr<Tab> &One : Tabs)
        if (One->File != 0) Open.push_back(One->File);
    Chooser.Begin(Bounds, std::move(Open));
    Now = Mode::Picking;
}

void Editor::Save()
{
    Tab &Here = Active();
    if (Here.File == 0) {
        FileCard.Begin(Here, Card::Then::Stay, Bounds);
        Now = Mode::Carding;
        return;
    }
    std::string Said;
    const bool  Saved = SaveTab(Here, Said);
    if (Saved) SavedNames.push_back(Here.Name);
    Note(std::move(Said), !Saved);
}

/* The question of changes not saved: about every tab when edit is left, about
 * the one tab when it is closed. Nothing unsaved, nothing asked. */
void Editor::AskToLeave(bool All)
{
    Unsaved.clear();
    LeavingAll = All;
    Closing    = All ? nullptr : &Active();
    for (const std::unique_ptr<Tab> &One : Tabs)
        if ((All || One.get() == Closing) && One->Dirty()) Unsaved.push_back(One.get());

    if (!Unsaved.empty()) {
        Now = Mode::Leaving;
        return;
    }
    if (All) Done = true;
    else     Close(Current);
}

/* Saves the unsaved one after another; a new file is saved through its card,
 * and this goes on when the card is done. Any refusal stops the leaving. */
void Editor::GoOnLeaving()
{
    while (!Unsaved.empty()) {
        Tab &Next = *Unsaved.front();
        Current   = IndexOf(&Next);
        if (Next.File == 0) {
            FileCard.Begin(Next, Card::Then::GoOnLeaving, Bounds);
            Now = Mode::Carding;
            return;
        }
        std::string Said;
        if (!SaveTab(Next, Said)) {
            Note(std::move(Said), true);
            Unsaved.clear();
            Now = Mode::Writing;
            return;
        }
        SavedNames.push_back(Next.Name);
        Unsaved.erase(Unsaved.begin());
    }
    if (LeavingAll) Done = true;
    else            Close(IndexOf(Closing));
}

std::vector<file_id_t> Editor::Given() const
{
    std::vector<file_id_t> Files = Touched;
    for (const std::unique_ptr<Tab> &One : Tabs)
        if (One->File != 0 && std::find(Files.begin(), Files.end(), One->File) == Files.end())
            Files.push_back(One->File);
    return Files;
}

std::string Editor::Summary() const
{
    std::vector<std::string> Lost;
    for (const std::unique_ptr<Tab> &One : Tabs)
        if (One->Dirty()) Lost.push_back(One->Name);

    std::vector<std::string> Saved;
    for (const std::string &Name : SavedNames)
        if (std::find(Saved.begin(), Saved.end(), Name) == Saved.end()) Saved.push_back(Name);

    std::string Said = Saved.empty() ? "edit: nothing saved" : "edit: saved " + Joined(Saved, ", ");
    if (!Lost.empty()) Said += "; left unsaved: " + Joined(Lost, ", ");
    return Said;
}

/* ---- the loop ----------------------------------------------------------- */

void Editor::Run()
{
    if (Tabs.empty()) OpenChooser();
    while (!Done) {
        if (!Paint()) break;
        /* No deadline: nothing changes between keys, and the cabin sleeps
         * until one comes. A wait that ends without a key means there is
         * nothing left to hear. */
        const tui::Event Heard = Screen.Wait();
        if (Heard.Kind != tui::EventKind::Key) break;
        OnKey(Heard.Key);
    }
    (void)Screen.Close();
}

void Editor::OnKey(const tui::KeyEvent &Key)
{
    Message.clear();
    switch (Now) {
    case Mode::Writing: OnWriting(Key); break;
    case Mode::Picking: OnPicking(Key); break;
    case Mode::Finding: {
        std::string Said;
        if (Finder.OnKey(Key, Active(), TextRows(), Said) == Search::Outcome::Closed) Now = Mode::Writing;
        if (!Said.empty()) Note(std::move(Said));
        break;
    }
    case Mode::GoingTo: OnGoingTo(Key); break;
    case Mode::Helping: OnHelping(Key); break;
    case Mode::Carding: OnCarding(Key); break;
    case Mode::Leaving: OnLeaving(Key); break;
    }
    if (!Tabs.empty()) Active().Follow(TextRows(), TextCols());
}

/* ---- keys over the text ------------------------------------------------- */

void Editor::OnWriting(const tui::KeyEvent &Key)
{
    Tab                       &Here  = Active();
    const std::optional<Heard> Asked = Hear(Key);
    if (!Asked) {
        const bool Typed = !Key.IsExtended() && !Key.IsCtrl() && !Key.IsAlt() && Key.Ascii >= 0x20 && Key.Ascii < 0x7F;
        if (!Typed) return;
        if (Here.Safety) Note("SAFETY is on: Ctrl+Tab lets this tab be written", true);
        else             Here.Type(Key.Ascii, Overtype);
        return;
    }
    if (Writes(Asked->Does) && Here.Safety) {
        Note("SAFETY is on: Ctrl+Tab lets this tab be written", true);
        return;
    }
    Do(*Asked);
}

void Editor::Do(const Heard &Asked)
{
    Tab              &Here = Active();
    const bool        More = Asked.Extend;
    const std::size_t Rows = TextRows();
    switch (Asked.Does) {
    case Command::Left:      Here.Left1(More); break;
    case Command::Right:     Here.Right1(More); break;
    case Command::Up:        Here.Vertical(false, More); break;
    case Command::Down:      Here.Vertical(true, More); break;
    case Command::WordLeft:  Here.WordLeft(More); break;
    case Command::WordRight: Here.WordRight(More); break;
    case Command::LineStart: Here.LineStart(More); break;
    case Command::LineEnd:   Here.LineEnd(More); break;
    case Command::PageUp:    Here.Page(false, Rows, More); break;
    case Command::PageDown:  Here.Page(true, Rows, More); break;
    case Command::FileStart: Here.MoveTo({0, 0}, false); break;
    case Command::FileEnd:   Here.MoveTo(Here.Body.End(), false); break;
    case Command::SelectAll: Here.SelectAll(); break;
    case Command::NewLine:   Here.NewLine(); break;
    case Command::Backspace: Here.Backspace(); break;
    case Command::Delete:    Here.Delete(); break;
    case Command::Indent:    Here.Indent(); break;
    case Command::Unindent:  Here.Unindent(); break;
    case Command::Undo:      if (!Here.Undo()) Note("there is nothing to undo"); break;
    case Command::Redo:      if (!Here.Redo()) Note("there is nothing undone to do again"); break;
    case Command::Copy:      Clipboard = Here.Copy(); Note(Clipboard.WholeLine ? "copied the line" : "copied"); break;
    case Command::Cut:       Clipboard = Here.Cut(); Note(Clipboard.WholeLine ? "cut the line" : "cut"); break;
    case Command::Paste:
        if (Clipboard.Bytes.empty()) Note("nothing has been copied yet");
        else                         Here.Paste(Clipboard);
        break;
    case Command::Overtype: Overtype = !Overtype; break;
    case Command::Safety:
        Here.Safety = !Here.Safety;
        Note(Here.Safety ? "SAFETY on: this tab can be read, not written" : "SAFETY off: this tab can be written");
        break;
    default: DoBeyondText(Asked); break;
    }
}

/* The commands that open a window, or move between tabs and files. */
void Editor::DoBeyondText(const Heard &Asked)
{
    switch (Asked.Does) {
    case Command::Save:     Save(); break;
    case Command::Open:     OpenChooser(); break;
    case Command::CloseTab: AskToLeave(false); break;
    case Command::Leave:    AskToLeave(true); break;
    case Command::Card:
        FileCard.Begin(Active(), Card::Then::Stay, Bounds);
        Now = Mode::Carding;
        break;
    case Command::GoToTab: {
        const std::size_t Wanted = Asked.Digit == 0 ? 9 : Asked.Digit - 1u;
        if (Wanted < Tabs.size()) Current = Wanted;
        else                      Note(std::format("there is no tab {}", Wanted + 1));
        break;
    }
    case Command::NextTab:  Current = (Current + 1) % Tabs.size(); break;
    case Command::PriorTab: Current = (Current + Tabs.size() - 1) % Tabs.size(); break;
    case Command::GoToLine:
        LineField.Clear();
        LineOrigin = Active().Caret;
        Now        = Mode::GoingTo;
        break;
    case Command::Find:
    case Command::Replace:
        Finder.Begin(Active(), Asked.Does == Command::Replace, Bounds);
        Now = Mode::Finding;
        break;
    case Command::Help:
        HelpFirst = 0;
        Now       = Mode::Helping;
        break;
    default: break;
    }
}

/* ---- keys of the windows ------------------------------------------------ */

void Editor::OnPicking(const tui::KeyEvent &Key)
{
    switch (Chooser.OnKey(Key)) {
    case Picker::Outcome::Stay: return;
    case Picker::Outcome::Closed:
        if (Tabs.empty()) Done = true;
        else              Now = Mode::Writing;
        return;
    case Picker::Outcome::Chosen:
        Now = Mode::Writing;
        if (Chooser.Chosen().File != 0) Open(Chooser.Chosen().File);
        else                            OpenNew(Chooser.Chosen().Name);
        if (Tabs.empty()) OpenChooser();
        return;
    }
}

/* A line's number, or a line and a column, "120" or "120:8"; the caret goes
 * there while it is typed, and back where it was when the window is shut. */
void Editor::OnGoingTo(const tui::KeyEvent &Key)
{
    Tab &Here = Active();
    if (!Key.IsExtended() && Key.Ascii == tui::KeyCodes::Escape) {
        Here.MoveTo(LineOrigin, false);
        Now = Mode::Writing;
        return;
    }
    if (!Key.IsExtended() && (Key.Ascii == tui::KeyCodes::Enter || Key.Ascii == '\r')) {
        Now = Mode::Writing;
        return;
    }
    if (!LineField.OnKey(Key)) return;

    const std::string &Said   = LineField.Value();
    const char        *Start  = Said.data();
    const char        *Finish = Said.data() + Said.size();
    const char        *Colon  = std::find(Start, Finish, ':');
    std::size_t        Line = 0, Column = 1;
    const auto         LineRead = std::from_chars(Start, Colon, Line);
    if (LineRead.ec != std::errc{} || LineRead.ptr != Colon || Line == 0) {
        Here.MoveTo(LineOrigin, false);
        return;
    }
    if (Colon != Finish) {
        const auto ColumnRead = std::from_chars(Colon + 1, Finish, Column);
        if (ColumnRead.ec != std::errc{} || Column == 0) Column = 1;
    }
    Line = std::min(Line, Here.Body.LineCount()) - 1;
    Here.MoveTo({Line, Here.Body.ByteAtColumn(Line, Column - 1)}, false);
    if (Line < Here.Top || Line >= Here.Top + TextRows()) Here.Centre(TextRows());
}

void Editor::OnHelping(const tui::KeyEvent &Key)
{
    if (Key.IsExtended() && Key.Scancode == tui::KeyCodes::Down) HelpFirst++;
    else if (Key.IsExtended() && Key.Scancode == tui::KeyCodes::Up) HelpFirst = HelpFirst > 0 ? HelpFirst - 1 : 0;
    else Now = Mode::Writing;
}

void Editor::OnCarding(const tui::KeyEvent &Key)
{
    const bool  Born = Active().File == 0;
    std::string Said;
    switch (FileCard.OnKey(Key, Active(), Said)) {
    case Card::Outcome::Stay: return;
    case Card::Outcome::Closed:
        Unsaved.clear();
        Now = Mode::Writing;
        if (!Said.empty()) Note(std::move(Said), true);
        Remember(Active().File);
        return;
    case Card::Outcome::Done:
        Note(std::move(Said));
        Remember(Active().File);
        if (Born) SavedNames.push_back(Active().Name);
        if (FileCard.After() == Card::Then::GoOnLeaving) {
            Unsaved.erase(Unsaved.begin());
            GoOnLeaving();
        } else {
            Now = Mode::Writing;
        }
        return;
    }
}

void Editor::OnLeaving(const tui::KeyEvent &Key)
{
    if (Key.IsExtended() || Key.IsCtrl()) return;
    switch (Key.Ascii) {
    case 's': case 'S':
        GoOnLeaving();
        return;
    case 'd': case 'D':
        Unsaved.clear();
        if (LeavingAll) Done = true;
        else            Close(IndexOf(Closing));
        return;
    case tui::KeyCodes::Escape:
        Unsaved.clear();
        Now = Mode::Writing;
        return;
    default:
        return;
    }
}

}  // namespace edit
