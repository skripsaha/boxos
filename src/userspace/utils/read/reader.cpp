/* read as a whole: the tabs and their books, the keys, and leaving. */

#include "reader.h"

#include <algorithm>
#include <format>

#include "kin/clipboard.h"
#include "kin/volume.h"

namespace read {

Reader::Reader(tui::Stage ScreenValue, Memory RememberedValue)
    : Screen(std::move(ScreenValue)), Bounds(Screen.Size()), Remembered(std::move(RememberedValue))
{
}

void Reader::Note(std::string Words, bool Trouble)
{
    Message        = std::move(Words);
    MessageTrouble = Trouble;
}

/* ---- tabs and files ----------------------------------------------------- */

/* A zero byte is no text anybody wrote: such a file is edit's to look at,
 * not read's. The book opens where it was left. */
void Reader::Open(file_id_t File)
{
    for (std::size_t Index = 0; Index < Tabs.size(); Index++) {
        if (Tabs[Index]->Body.File != File) continue;
        Show(Index);
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
    if (Bytes->find('\0') != std::string::npos) {
        Note(std::format("{} holds bytes that are no text: edit shows them", Facts->Name), true);
        return;
    }

    Remembered.Settle(File, *Bytes);
    auto One    = std::make_unique<Tab>(Book::Make(File, Facts->Name, std::move(*Bytes), ReadingFor(Facts->Name)),
                                        Facts->Tags);
    One->Marked = Remembered.Marks(File);
    One->Lay(Column(), PageRows());
    if (const std::optional<Mark> Place = Remembered.Place(File); Place && !One->Body.Blocks.empty())
        One->GoTo(One->Body.BlockAt(Place->Source), 0, PageRows());

    Tabs.push_back(std::move(One));
    if (std::find(Opened.begin(), Opened.end(), File) == Opened.end()) Opened.push_back(File);
    Show(Tabs.size() - 1);
}

/* A tab laid at another width while it was behind is laid again now. */
void Reader::Show(std::size_t Index)
{
    Current = Index;
    Active().Lay(Column(), PageRows());
}

/* Where the book was left, remembered: it opens there again. */
void Reader::Leave(const Tab &One)
{
    if (!One.Lines.Rows().empty()) Remembered.SetPlace(One.Body.File, One.PlaceOfReading(), One.Body.FileBytes());
}

void Reader::Close(std::size_t Index)
{
    Leave(*Tabs[Index]);
    Tabs.erase(Tabs.begin() + static_cast<std::ptrdiff_t>(Index));
    if (Current >= Index && Current > 0) Current--;
    Now = Mode::Reading;
    if (Tabs.empty()) OpenChooser();
    else              Show(Current);
}

void Reader::OpenChooser()
{
    std::vector<file_id_t> Open;
    for (const std::unique_ptr<Tab> &One : Tabs) Open.push_back(One->Body.File);
    Chooser.Begin(Bounds, std::move(Open), Looking, false);
    Now = Mode::Picking;
}

/* The other way of looking, for every window at once. */
void Reader::Rescope()
{
    Looking = Looking == Scope::InContext ? Scope::Everywhere : Scope::InContext;
    Note(Looking == Scope::Everywhere ? "looking at the whole volume" : "looking through the Use Context");
    if (Now == Mode::Picking) Chooser.Rescope(Looking);
    if (Now == Mode::Carding) FileCard.Rescope(Looking);
}

std::string Reader::Afterwords() const
{
    std::string Said;
    for (const std::string &One : Troubles) Said += "read: " + One + "\n";
    return Said;
}

/* ---- the loop ----------------------------------------------------------- */

void Reader::Run()
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
    for (const std::unique_ptr<Tab> &One : Tabs) Leave(*One);
    (void)Screen.Close();
    if (std::optional<std::string> Refused = Remembered.Save())
        Troubles.push_back("the bookmarks could not be kept: " + *Refused);
}

void Reader::OnKey(const tui::KeyEvent &Key)
{
    Message.clear();
    if (const std::optional<Heard> Asked = Hear(Key); Asked && Asked->Does == Command::Scope) {
        Rescope();
        return;
    }
    switch (Now) {
    case Mode::Reading: OnReading(Key); break;
    case Mode::Picking: OnPicking(Key); break;
    case Mode::Finding:
        if (Finder.OnKey(Key, Active(), PageRows()) == Search::Outcome::Closed) Now = Mode::Reading;
        break;
    case Mode::Listing: OnListing(Key); break;
    case Mode::Helping:
        if (Keys.OnKey(Key) == Help::Outcome::Closed) Now = Mode::Reading;
        break;
    case Mode::Carding: OnCarding(Key); break;
    }
}

/* ---- keys over the page ------------------------------------------------- */

void Reader::OnReading(const tui::KeyEvent &Key)
{
    if (const std::optional<Heard> Asked = Hear(Key)) Do(*Asked);
}

void Reader::Do(const Heard &Asked)
{
    Tab              &Here = Active();
    const std::size_t Rows = PageRows();
    const bool        More = Asked.Extend;
    switch (Asked.Does) {
    case Command::Up:        Here.Step(false, More, Rows); break;
    case Command::Down:      Here.Step(true, More, Rows); break;
    case Command::PageUp:    Here.Page(false, More, Rows); break;
    case Command::PageDown:  Here.Page(true, More, Rows); break;
    case Command::FileStart: Here.Edge(false, Rows); break;
    case Command::FileEnd:   Here.Edge(true, Rows); break;
    case Command::SelectAll: Here.SelectAll(Rows); break;
    case Command::NextHeading:
    case Command::PriorHeading: {
        const bool Next = Asked.Does == Command::NextHeading;
        if (!Here.Heading(Next, Rows))
            Note(Here.Body.Headings().empty() ? "this file has no headings"
                 : Next                        ? "no heading comes after this"
                                               : "no heading comes before this");
        break;
    }
    case Command::Contents: OpenList(Window::Contents); break;
    case Command::Copy:     Copy(); break;
    case Command::Mark:     ToggleMark(); break;
    case Command::Marks:    OpenList(Window::Bookmarks); break;
    case Command::Wider:    Widen(WidthStep); break;
    case Command::Narrower: Widen(-WidthStep); break;
    case Command::Turn:
        Here.Turn(Column(), Rows);
        Note(Here.Body.As == Reading::Prose ? "read as prose: lines joined into paragraphs, marks shown as what they mean"
                                            : "read as lines: every line as it stands");
        break;
    case Command::Find:
        Finder.Begin(Here, Bounds);
        Now = Mode::Finding;
        break;
    case Command::Help:
        Keys.Begin(HelpLines(), "read");
        Now = Mode::Helping;
        break;
    case Command::Open: OpenChooser(); break;
    case Command::Card:
        FileCard.Begin(Here.Body.File, Here.Body.Name, Here.Tags, Looking, Bounds);
        Now = Mode::Carding;
        break;
    case Command::CloseTab: Close(Current); break;
    case Command::Leave:    Done = true; break;
    case Command::GoToTab: {
        const std::size_t Wanted = Asked.Digit == 0 ? 9 : Asked.Digit - 1u;
        if (Wanted < Tabs.size()) Show(Wanted);
        else                      Note(std::format("there is no tab {}", Wanted + 1));
        break;
    }
    case Command::NextTab:  Show((Current + 1) % Tabs.size()); break;
    case Command::PriorTab: Show((Current + Tabs.size() - 1) % Tabs.size()); break;
    case Command::Scope:    Rescope(); break;
    }
}

/* The selection, or the paragraph at the reading line, as the newest clip:
 * the words as they are shown — a paragraph's wrapped rows joined again, a
 * code block's lines kept. */
void Reader::Copy()
{
    const Tab              &Here = Active();
    const std::vector<Row> &Rows = Here.Lines.Rows();
    if (Rows.empty()) return;
    std::string Text;
    std::string Said;
    if (Here.HasSelection()) {
        const auto [From, To] = Here.Selection();
        std::size_t Was = Rows.size(), Count = 0;
        for (std::size_t Row = From; Row <= To; Row++) {
            if (!Here.TextRow(Row)) continue;
            const Block &One   = Here.Body.Blocks[Rows[Row].Block];
            const bool   Lined = One.Is == Kind::Code || One.Is == Kind::Table || One.Is == Kind::Line;
            if (Was != Rows.size()) Text += Rows[Row].Block == Was ? (Lined ? "\n" : " ") : (Lined ? "\n" : "\n\n");
            Text += std::string_view(One.Text).substr(Rows[Row].From, Rows[Row].To - Rows[Row].From);
            Was = Rows[Row].Block;
            Count++;
        }
        Said = std::format("copied {} line{}", Count, Count == 1 ? "" : "s");
    } else {
        const Block &One = Here.Body.Blocks[Rows[Here.Reading].Block];
        Text             = One.Text;
        Said = One.Is == Kind::Line ? "copied the line" : One.Is == Kind::Code ? "copied the code" : "copied the paragraph";
    }
    if (Text.empty()) {
        Note("there is nothing there to copy");
        return;
    }
    if (std::expected<std::uint64_t, std::string> Put = PutClip(Text); !Put) Note(Put.error(), true);
    else                                                                       Note(std::move(Said));
}

/* A bookmark is on a paragraph: the one on the reading line's paragraph is
 * taken away, wherever in it the reading line stands. */
void Reader::ToggleMark()
{
    Tab &Here = Active();
    if (Here.Lines.Rows().empty()) return;
    const std::size_t Block = Here.BlockOfReading();
    const auto        On    = std::find_if(Here.Marked.begin(), Here.Marked.end(),
                                           [&](const Mark &One) { return Here.Body.BlockAt(One.Source) == Block; });
    if (On != Here.Marked.end()) {
        Remembered.Unmark(Here.Body.File, On->Source);
        Note("the bookmark here is taken away");
    } else {
        Remembered.Put(Here.Body.File, Here.PlaceOfReading(), Here.Body.FileBytes());
        Note("a bookmark is put here");
    }
    Here.Marked = Remembered.Marks(Here.Body.File);
}

/* Held to what the glass allows, and kept only while read runs: a column is
 * the glass's as much as the person's. */
void Reader::Widen(std::int32_t By)
{
    const std::int32_t Was = Column();
    Width                  = Was + By;
    Width                  = Column();
    if (Width == Was) {
        Note(By > 0 ? "the column is as wide as the glass lets it be" : "the column is as narrow as it goes");
        return;
    }
    Active().Lay(Width, PageRows());
    Note(std::format("the column is {} wide", Width));
}

/* ---- the windows -------------------------------------------------------- */

/* The contents or the bookmarks, the line the reading line is in or after
 * chosen. A bookmark whose words are gone from the file says so. */
void Reader::OpenList(Window Which)
{
    const Tab         &Here    = Active();
    const std::size_t  AtBlock = Here.BlockOfReading();
    std::vector<Entry> Entries;
    std::size_t        Chosen   = 0;
    const auto         PageNote = [&](std::size_t Block) {
        return std::format("p {}", Here.PageOf(Here.Lines.RowOf(Block), PageRows()));
    };

    if (Which == Window::Contents) {
        for (const std::size_t Block : Here.Body.Headings()) {
            const read::Block &One = Here.Body.Blocks[Block];
            if (Block <= AtBlock) Chosen = Entries.size();
            Entries.push_back({One.Text, PageNote(Block), 2 * std::max(0, One.Level - 1), Block});
        }
        if (Entries.empty()) {
            Note("this file has no headings");
            return;
        }
        Shown.Begin("Contents", "Enter go there  Esc close", std::move(Entries), Chosen, false, Bounds);
    } else {
        for (std::size_t Index = 0; Index < Here.Marked.size(); Index++) {
            const Mark       &One   = Here.Marked[Index];
            const std::size_t Block = Here.Body.BlockAt(One.Source);
            if (Block <= AtBlock) Chosen = Index;
            const std::string Words = Here.Body.Blocks.empty() ? std::string() : FirstLine(Here.Body.Blocks[Block].Text);
            Entries.push_back({Words, One.Lost ? PageNote(Block) + ", its words are gone" : PageNote(Block), 0, Index,
                               One.Lost});
        }
        if (Entries.empty()) {
            Note("no bookmarks yet: Ctrl+M puts one on the reading line");
            return;
        }
        Shown.Begin("Bookmarks", "Enter go there  Delete take away  Esc close", std::move(Entries), Chosen, true,
                    Bounds);
    }
    Front = Which;
    Now   = Mode::Listing;
}

void Reader::OnListing(const tui::KeyEvent &Key)
{
    Tab &Here = Active();
    switch (Shown.OnKey(Key)) {
    case List::Outcome::Stay: return;
    case List::Outcome::Closed: Now = Mode::Reading; return;
    case List::Outcome::Chosen:
        Now = Mode::Reading;
        if (Front == Window::Contents) Here.Land(Shown.Picked().Target, PageRows());
        else                           Here.GoTo(Here.Body.BlockAt(Here.Marked[Shown.Picked().Target].Source), 0, PageRows());
        return;
    case List::Outcome::Dropped:
        Remembered.Unmark(Here.Body.File, Here.Marked[Shown.Picked().Target].Source);
        Here.Marked = Remembered.Marks(Here.Body.File);
        Note("the bookmark is taken away");
        if (Here.Marked.empty()) Now = Mode::Reading;
        else                     OpenList(Window::Bookmarks);
        return;
    }
}

void Reader::OnPicking(const tui::KeyEvent &Key)
{
    switch (Chooser.OnKey(Key)) {
    case Picker::Outcome::Stay: return;
    case Picker::Outcome::Closed:
        if (Tabs.empty()) Done = true;
        else              Now = Mode::Reading;
        return;
    case Picker::Outcome::Chosen:
        Now = Mode::Reading;
        Open(Chooser.Chosen().File);
        if (Tabs.empty()) OpenChooser();
        return;
    }
}

/* The tab says the name and tags the volume holds since the card last asked
 * to change them, kept or refused. */
void Reader::OnCarding(const tui::KeyEvent &Key)
{
    std::string         Said;
    const Card::Outcome Answer = FileCard.OnKey(Key, Said);
    Tab                &Here   = Active();
    Here.Body.Name             = FileCard.Name();
    Here.Tags                  = FileCard.Tags();
    switch (Answer) {
    case Card::Outcome::Stay: return;
    case Card::Outcome::Closed: Now = Mode::Reading; return;
    case Card::Outcome::Done:
        Note(std::move(Said));
        Now = Mode::Reading;
        return;
    }
}

}  // namespace read
