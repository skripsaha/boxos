/* read as a whole: books and places, moving, the keys, and finding. */

#include "reader.h"

#include <algorithm>
#include <format>

#include "box/cxx/clipboard.h"

#include "glyph.h"

namespace read {

namespace {

char Fold(char Ch) noexcept
{
    return Ch >= 'A' && Ch <= 'Z' ? static_cast<char>(Ch - 'A' + 'a') : Ch;
}

std::string Folded(std::string_view Text)
{
    std::string Out(Text);
    for (char &Ch : Out) Ch = Fold(Ch);
    return Out;
}

/* The first line of a clip, every control character a blank. */
std::string FirstLine(std::string_view Bytes)
{
    std::string Line(Bytes.substr(0, Bytes.find('\n')));
    for (char &Ch : Line)
        if (static_cast<unsigned char>(Ch) < 0x20 || Ch == 0x7F) Ch = ' ';
    return Line;
}

}  // namespace

Reader::Reader(tui::Stage ScreenValue, Memory RememberedValue)
    : Screen(std::move(ScreenValue)),
      Bounds(Screen.Size()),
      Remembered(std::move(RememberedValue)),
      Scheme(&ThemeNamed(Remembered.ThemeName())),
      Width(Remembered.Width())
{
}

/* ---- books and places ----------------------------------------------------- */

/* The book at Index, opened where it was left; false, said, when it would not
 * open, and the book in front stays. */
bool Reader::Show(std::size_t Index)
{
    std::expected<Book, std::string> Opening = Open(Files[Index]);
    if (!Opening) {
        Troubles.push_back(Opening.error());
        Note(Opening.error(), true);
        return false;
    }
    Current = Index;
    Here    = std::move(*Opening);
    if (std::find(Opened.begin(), Opened.end(), Here.File) == Opened.end()) Opened.push_back(Here.File);
    Marked = Remembered.Marks(Here.File);
    Anchor.reset();
    Lay();

    const std::optional<std::size_t> Place = Remembered.Place(Here.File);
    if (Place && !Here.Blocks.empty()) {
        Reading = Lines.RowOf(Here.BlockAt(*Place));
        Top     = std::min(Reading > PageRows() / 3 ? Reading - PageRows() / 3 : 0, MostTop());
    } else {
        Edge(false);
    }
    Search();
    return true;
}

/* Where the book in front was left, remembered. */
void Reader::Leave()
{
    if (Here.File != 0 && !Lines.Rows().empty()) Remembered.SetPlace(Here.File, PlaceOfReading());
}

void Reader::Lay()
{
    Lines.Make(Here, Column());
}

/* Laid again — a new width, the other way of reading — the reading line kept
 * on the same words and as far down the page as it was. */
void Reader::Relay()
{
    if (Lines.Rows().empty()) {
        Lay();
        return;
    }
    const Row         Was   = Lines.Rows()[Reading];
    const std::size_t Down  = Reading - std::min(Reading, Top);
    Lay();
    Reading = Lines.RowOf(Was.Block, Was.From);
    Top     = std::min(Reading > Down ? Reading - Down : 0, MostTop());
}

std::size_t Reader::PlaceOfReading() const
{
    if (Lines.Rows().empty() || Here.Blocks.empty()) return 0;
    const std::size_t Block = std::min(Lines.Rows()[Reading].Block, Here.Blocks.size() - 1);
    return Here.Blocks[Block].Source;
}

/* The reading line on that byte of that block, and the page about it when it
 * is not on the page already: a third of the way down, where the eye goes. */
void Reader::GoToBlock(std::size_t Block, std::size_t Byte)
{
    if (Lines.Rows().empty()) return;
    Reading = Lines.RowOf(Block, Byte);
    Anchor.reset();
    if (Reading < Top || Reading >= Top + PageRows())
        Top = std::min(Reading > PageRows() / 3 ? Reading - PageRows() / 3 : 0, MostTop());
}

/* ---- moving ------------------------------------------------------------- */

bool Reader::TextRow(std::size_t Row) const noexcept
{
    return Row < Lines.Rows().size() && Lines.Rows()[Row].Is == RowKind::Text;
}

std::size_t Reader::MostTop() const noexcept
{
    const std::size_t Rows = Lines.Rows().size(), Page = PageRows();
    return Rows > Page ? Rows - Page : 0;
}

void Reader::Follow() noexcept
{
    if (Reading < Top) Top = Reading;
    if (Reading >= Top + PageRows()) Top = Reading - PageRows() + 1;
}

/* The reading line to the next line of text, or the one before; with Shift
 * the selection grows from where the line was. */
void Reader::Step(bool Down, bool Extend)
{
    if (Extend && !Anchor) Anchor = Reading;
    if (!Extend) Anchor.reset();
    std::size_t Row = Reading;
    while (Down ? Row + 1 < Lines.Rows().size() : Row > 0) {
        Row = Down ? Row + 1 : Row - 1;
        if (TextRow(Row)) {
            Reading = Row;
            break;
        }
    }
    Follow();
}

/* A page on, or back — one line of the old page kept, so the eye has where
 * it was — and the reading line at the top of it. At the end of the book the
 * reading line goes to its last line instead. */
void Reader::Page(bool Down)
{
    Anchor.reset();
    const std::size_t Stride = this->Stride();
    if (Down && Top >= MostTop()) {
        Edge(true);
        return;
    }
    if (!Down && Top == 0) {
        Edge(false);
        return;
    }
    Top = Down ? std::min(Top + Stride, MostTop()) : (Top > Stride ? Top - Stride : 0);
    for (Reading = Top; Reading + 1 < Lines.Rows().size() && !TextRow(Reading);) Reading++;
}

void Reader::Edge(bool End)
{
    Anchor.reset();
    if (Lines.Rows().empty()) {
        Top = Reading = 0;
        return;
    }
    Top = End ? MostTop() : 0;
    if (End) {
        for (Reading = Lines.Rows().size() - 1; Reading > 0 && !TextRow(Reading);) Reading--;
    } else {
        for (Reading = 0; Reading + 1 < Lines.Rows().size() && !TextRow(Reading);) Reading++;
    }
}

/* The next heading, or the one this part is under — the one before it when
 * the reading line stands on a heading already — near the top of the page. */
void Reader::Heading(bool Next)
{
    if (Lines.Rows().empty()) return;
    const std::size_t              Block    = Lines.Rows()[Reading].Block;
    const std::vector<std::size_t> Headings = Here.Headings();
    std::optional<std::size_t>     To;
    for (const std::size_t One : Headings) {
        if (Next && One > Block) {
            To = One;
            break;
        }
        if (!Next && One < Block) To = One;
    }
    if (!To) {
        Note(Headings.empty() ? "this book has no headings" : Next ? "no heading comes after this" : "no heading comes before this");
        return;
    }
    Anchor.reset();
    Reading = Lines.RowOf(*To);
    Top     = std::min(Reading > 0 ? Reading - 1 : 0, MostTop());
}

/* ---- keys --------------------------------------------------------------- */

void Reader::Note(std::string Words, bool Trouble)
{
    Message        = std::move(Words);
    MessageTrouble = Trouble;
}

void Reader::Run()
{
    bool Showing = false;
    for (std::size_t Index = 0; Index < Files.size() && !Showing; Index++) Showing = Show(Index);
    if (Showing) {
        while (!Done) {
            if (!Paint()) break;
            const tui::Event Heard = Screen.Wait();
            if (Heard.Kind != tui::EventKind::Key) break;
            OnKey(Heard.Key);
        }
        Leave();
    }
    (void)Screen.Close();
    if (std::optional<std::string> Refused = Remembered.Save())
        Troubles.push_back("where the books were left could not be remembered: " + *Refused);
}

std::string Reader::Afterwords() const
{
    std::string Said;
    for (const std::string &One : Troubles) Said += "read: " + One + "\n";
    return Said;
}

void Reader::OnKey(const tui::KeyEvent &Key)
{
    Message.clear();
    switch (Now) {
    case Mode::Reading: OnReading(Key); break;
    case Mode::Finding: OnFinding(Key); break;
    case Mode::Listing: OnListing(Key); break;
    }
}

void Reader::OnReading(const tui::KeyEvent &Key)
{
    if (Key.IsCtrl() && !Key.IsExtended()) {
        if (Fold(Key.Ascii) == 'c') Copy();
        return;
    }
    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Up:       Step(false, Key.IsShift()); break;
        case tui::KeyCodes::Down:     Step(true, Key.IsShift()); break;
        case tui::KeyCodes::PageUp:   Page(false); break;
        case tui::KeyCodes::PageDown: Page(true); break;
        case tui::KeyCodes::Home:     Edge(false); break;
        case tui::KeyCodes::End:      Edge(true); break;
        default: break;
        }
        return;
    }

    const auto Turn = [&](bool Forward) {
        Leave();
        for (std::size_t Index = Current; Forward ? Index + 1 < Files.size() : Index > 0;) {
            Index = Forward ? Index + 1 : Index - 1;
            if (Show(Index)) return;
        }
        if (Message.empty()) Note(Files.size() == 1 ? "this is the one book" : Forward ? "this is the last book" : "this is the first book");
    };

    switch (Key.Ascii) {
    case ' ':  Page(!Key.IsShift()); break;
    case 'f':  Page(true); break;
    case 'b':  Page(false); break;
    case 'j':  Step(true, false); break;
    case 'k':  Step(false, false); break;
    case 'g':  Edge(false); break;
    case 'G':  Edge(true); break;
    case '}':  Heading(true); break;
    case '{':  Heading(false); break;
    case ']':  Turn(true); break;
    case '[':  Turn(false); break;
    case 'n':  NextMatch(true); break;
    case 'N':  NextMatch(false); break;
    case 'c':  OpenList(Window::Contents); break;
    case '\'': OpenList(Window::Bookmarks); break;
    case '?':  OpenList(Window::Keys); break;
    case 'm':  ToggleMark(); break;
    case '+':
    case '=':  Widen(WidthStep); break;
    case '-':  Widen(-WidthStep); break;
    case 'q':  Done = true; break;
    case '/':
        Now = Mode::Finding;
        Needle.clear();
        Find();
        break;
    case 't':
        Scheme = &ThemeAfter(*Scheme);
        Remembered.SetTheme(Scheme->Name);
        Note(std::format("the page is {}", Scheme->Name));
        break;
    case 'w': {
        const std::size_t Place = PlaceOfReading();
        Here                    = Here.Other();
        Lay();
        GoToBlock(Here.BlockAt(Place));
        Search();
        Note(Here.As == Reading::Prose ? "read as prose: lines joined into paragraphs, marks shown as what they mean"
                                       : "read as lines: every line as it stands");
        break;
    }
    case tui::KeyCodes::Escape:
        if (Anchor) {
            Anchor.reset();
        } else if (!Needle.empty()) {
            Needle.clear();
            Search();
        } else {
            Done = true;
        }
        break;
    default: break;
    }
}

/* What is typed is looked for as it is typed; Enter keeps what was found
 * lit, Esc lets it go. Ctrl+V types the newest clip's first line. */
void Reader::OnFinding(const tui::KeyEvent &Key)
{
    if (Key.IsExtended()) return;
    if (Key.IsCtrl()) {
        if (Fold(Key.Ascii) != 'v') return;
        box::result<std::vector<box::clip>> Newest = box::clipboard::list(1);
        if (!Newest || Newest->empty()) return;
        box::result<std::string> Bytes = box::clipboard::take(Newest->front());
        if (!Bytes) return;
        Needle += FirstLine(*Bytes);
        Find();
        return;
    }
    switch (Key.Ascii) {
    case tui::KeyCodes::Escape:
        Now = Mode::Reading;
        Needle.clear();
        Search();
        return;
    case tui::KeyCodes::Enter:
    case '\r':
        Now = Mode::Reading;
        if (!Needle.empty() && Matches.empty()) Note(std::format("nothing here says {}", Needle));
        return;
    case tui::KeyCodes::Backspace:
    case 0x7F: {
        std::size_t Cut = 0;
        for (std::size_t Byte = 0; Byte < Needle.size(); Byte += GlyphAt(Needle, Byte).Length) Cut = Byte;
        Needle.resize(Needle.empty() ? 0 : Cut);
        Find();
        return;
    }
    default:
        if (Key.Ascii >= 0x20 && Key.Ascii < 0x7F) {
            Needle += Key.Ascii;
            Find();
        }
        return;
    }
}

void Reader::OnListing(const tui::KeyEvent &Key)
{
    switch (Shown.OnKey(Key)) {
    case List::Outcome::Stay: return;
    case List::Outcome::Closed: Now = Mode::Reading; return;
    case List::Outcome::Chosen:
        Now = Mode::Reading;
        if (Front == Window::Contents) {
            Reading = Lines.RowOf(Shown.Picked().Target);
            Anchor.reset();
            Top = std::min(Reading > 0 ? Reading - 1 : 0, MostTop());
        } else if (Front == Window::Bookmarks) {
            GoToBlock(Here.BlockAt(Marked[Shown.Picked().Target]));
        }
        return;
    case List::Outcome::Dropped:
        Remembered.Unmark(Here.File, Marked[Shown.Picked().Target]);
        Marked.erase(Marked.begin() + static_cast<std::ptrdiff_t>(Shown.Picked().Target));
        OpenList(Window::Bookmarks);
        Note("the bookmark is taken away");
        if (Marked.empty()) Now = Mode::Reading;
        return;
    }
}

/* The contents, the bookmarks, or the keys, in a window in front. */
void Reader::OpenList(Window Which)
{
    std::vector<Entry> Entries;
    std::size_t        Chosen = 0;
    const std::size_t  AtBlock  = Lines.Rows().empty() ? 0 : Lines.Rows()[Reading].Block;
    const auto         PageNote = [&](std::size_t Block) { return std::format("p {}", PageOf(Lines.RowOf(Block))); };

    if (Which == Window::Contents) {
        for (const std::size_t Block : Here.Headings()) {
            const read::Block &One = Here.Blocks[Block];
            if (Block <= AtBlock) Chosen = Entries.size();
            Entries.push_back({One.Text, PageNote(Block), 2 * std::max(0, One.Level - 1), Block});
        }
        if (Entries.empty()) {
            Note("this book has no headings");
            return;
        }
        Shown.Begin("Contents", "Enter go there  Esc close", std::move(Entries), Chosen, false, Bounds);
    } else if (Which == Window::Bookmarks) {
        for (std::size_t Index = 0; Index < Marked.size(); Index++) {
            const std::size_t Block = Here.BlockAt(Marked[Index]);
            if (Block <= AtBlock) Chosen = Index;
            Entries.push_back({FirstLine(Here.Blocks.empty() ? std::string() : Here.Blocks[Block].Text), PageNote(Block),
                               0, Index});
        }
        if (Entries.empty()) {
            Note("no bookmarks yet: m puts one on the reading line");
            return;
        }
        Shown.Begin("Bookmarks", "Enter go there  Del take away  Esc close", std::move(Entries), Chosen, true, Bounds);
    } else {
        const std::pair<const char *, const char *> Keys[] = {
            {"the next page", "Space  PgDn  f"},
            {"the page before", "Shift+Space  PgUp  b"},
            {"the next line, the one before", "Down  Up  j  k"},
            {"the start, the end", "Home  End  g  G"},
            {"the next heading, the one before", "}  {"},
            {"the next book, the one before", "]  ["},
            {"the contents", "c"},
            {"look for words, found as they are typed", "/"},
            {"the next found, the one before", "n  N"},
            {"a bookmark here, or none", "m"},
            {"the bookmarks", "'"},
            {"dark, sepia, light", "t"},
            {"a wider column, a narrower one", "+  -"},
            {"prose, or every line as it stands", "w"},
            {"lines selected", "Shift+Down  Shift+Up"},
            {"copy what is selected, or the paragraph", "Ctrl+C"},
            {"leave read", "q  Esc"},
        };
        for (const auto &[Says, Pressed] : Keys) Entries.push_back({Says, Pressed, 0, 0});
        Shown.Begin("Keys", "any key closes", std::move(Entries), 0, false, Bounds);
    }
    Front = Which;
    Now  = Mode::Listing;
}

void Reader::ToggleMark()
{
    if (Lines.Rows().empty()) return;
    const bool Put = Remembered.Toggle(Here.File, PlaceOfReading());
    Marked         = Remembered.Marks(Here.File);
    Note(Put ? "a bookmark is put here" : "the bookmark here is taken away");
}

/* The selection, or the paragraph at the reading line, as the newest clip:
 * the words as they are shown — a paragraph's wrapped rows joined again, a
 * code block's lines kept. */
void Reader::Copy()
{
    if (Lines.Rows().empty()) return;
    const std::vector<Row> &Rows = Lines.Rows();
    std::string             Text;
    std::string             Said;
    if (Anchor && *Anchor != Reading) {
        const std::size_t From = std::min(*Anchor, Reading), To = std::max(*Anchor, Reading);
        std::size_t       Was = Rows.size(), Count = 0;
        for (std::size_t Row = From; Row <= To; Row++) {
            if (!TextRow(Row)) continue;
            const Block      &One   = Here.Blocks[Rows[Row].Block];
            const bool        Lined = One.Is == Kind::Code || One.Is == Kind::Table || One.Is == Kind::Line;
            if (Was != Rows.size()) Text += Rows[Row].Block == Was ? (Lined ? "\n" : " ") : (Lined ? "\n" : "\n\n");
            Text += std::string_view(One.Text).substr(Rows[Row].From, Rows[Row].To - Rows[Row].From);
            Was = Rows[Row].Block;
            Count++;
        }
        Said = std::format("copied {} line{}", Count, Count == 1 ? "" : "s");
    } else {
        const Block &One = Here.Blocks[Rows[Reading].Block];
        Text             = One.Text;
        Said             = One.Is == Kind::Line ? "copied the line" : One.Is == Kind::Code ? "copied the code" : "copied the paragraph";
    }
    if (Text.empty()) {
        Note("there is nothing there to copy");
        return;
    }
    box::result<std::uint64_t> Put = box::clipboard::put(Text);
    if (!Put) Note(std::format("the clipboard would not keep it: {}", Put.error().message()), true);
    else      Note(std::move(Said));
}

void Reader::Widen(std::int32_t By)
{
    const std::int32_t Was = Column();
    Width                  = Was + By;
    Width                  = Column();   /* held to what the glass allows */
    if (Width == Was) {
        Note(By > 0 ? "the column is as wide as the glass lets it be" : "the column is as narrow as it goes");
        return;
    }
    Remembered.SetWidth(Width);
    Relay();
    Note(std::format("the column is {} wide", Width));
}

/* ---- finding ------------------------------------------------------------ */

/* Every place the needle is said, big and small letters one. */
void Reader::Search()
{
    Matches.clear();
    AtMatch = 0;
    if (Needle.empty()) return;
    const std::string Wanted = Folded(Needle);
    for (std::size_t Block = 0; Block < Here.Blocks.size(); Block++) {
        const std::string Low = Folded(Here.Blocks[Block].Text);
        for (std::size_t At = Low.find(Wanted); At != std::string::npos; At = Low.find(Wanted, At + Wanted.size()))
            Matches.push_back({Block, At, At + Wanted.size()});
    }
}

/* Searched, and the reading line at the first place found from where it
 * stands. */
void Reader::Find()
{
    Search();
    if (Matches.empty() || Lines.Rows().empty()) return;

    const Row &From = Lines.Rows()[Reading];
    for (std::size_t Index = 0; Index < Matches.size(); Index++)
        if (Matches[Index].Block > From.Block || (Matches[Index].Block == From.Block && Matches[Index].From >= From.From)) {
            AtMatch = Index;
            break;
        }
    ShowMatch();
}

void Reader::NextMatch(bool Forward)
{
    if (Matches.empty()) {
        Note(Needle.empty() ? "nothing is being looked for: / looks" : std::format("nothing here says {}", Needle));
        return;
    }
    AtMatch = Forward ? (AtMatch + 1) % Matches.size() : (AtMatch + Matches.size() - 1) % Matches.size();
    ShowMatch();
}

void Reader::ShowMatch()
{
    const Match &One = Matches[AtMatch];
    GoToBlock(One.Block, One.From);
}

}  // namespace read
