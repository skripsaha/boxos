/* board.cpp — the board's reading, its list and its keys. */

#include "board.h"

#include <algorithm>
#include <format>
#include <utility>

#include "box/clock.h"
#include "box/core/cabin.h"
#include "box/cxx/error.h"
#include "box/display.h"
#include "box/system.h"

namespace taskboard {

namespace {

namespace tui = painter::tui;

// The first reading after the start comes soon: the processor a program used
// is the difference of two.
constexpr std::uint64_t FirstReadUs = 300000;
// Up or down a page: so many programs.
constexpr std::ptrdiff_t PageStep = 10;

Who WhoOf(const Program &One)
{
    return Who{One.Seen.Pid, One.Seen.Generation};
}

std::string Why(int Answer)
{
    return std::string(box::error(static_cast<::error_t>(Answer < 0 ? -Answer : Answer)).message());
}

}  // namespace

Board::Board(tui::Stage Opened, std::string Kept)
    : Screen(std::move(Opened)), Shown(ReadLook()), Filter(std::move(Kept))
{
    if (!Said.Hears()) Status = "programs started and ended are not heard: the journal stays empty";
}

void Board::Run()
{
    Screen.HearEnd();
    Read();
    NextReadUs = clock_uptime_us() + FirstReadUs;
    for (;;) {
        if (Watch.changed()) Shown = ReadLook();
        Paint();
        if (!Screen.Present()) return;

        const std::uint64_t Now = clock_uptime_us();
        const auto          Left = NextReadUs > Now ? static_cast<std::uint32_t>((NextReadUs - Now) / 1000u) : 0u;
        const tui::Event    Heard = Screen.Wait(Left == 0 ? 1 : Left);
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) return;
        if (Heard.Kind == tui::EventKind::Key && !OnKey(Heard.Key)) return;
        if (clock_uptime_us() >= NextReadUs) {
            Read();
            NextReadUs = clock_uptime_us() + static_cast<std::uint64_t>(Shown.RefreshMs) * 1000u;
        }
    }
}

std::vector<file_id_t> Board::Given() const
{
    std::vector<file_id_t> Files;
    for (const Mark &One : Marked)
        if (One.File != 0 && std::find(Files.begin(), Files.end(), One.File) == Files.end()) Files.push_back(One.File);
    return Files;
}

void Board::Read()
{
    // A death heard now is named by what the board still knows.
    Said.Listen(Known);
    std::expected<Reading, int> Fresh = ReadMachine();
    if (!Fresh) {
        Status = std::format("the machine could not be read: {}", Why(Fresh.error()));
        return;
    }
    Load.Take(*Fresh, Shown.HistorySeconds);
    Known.Take(*Fresh);
    Said.Take(Known);
    Now = std::move(*Fresh);
    // One asked to finish that did: the word about it is old.
    if (std::erase_if(Asked, [&](const Who &One) { return !Known.Find(One.Pid, One.Generation); }) > 0) Status.clear();
    Move(0);
}

std::vector<Board::Line> Board::Lines() const
{
    std::vector<Line>     Out;
    std::optional<Living> Group;
    for (const Program *One : Known.Shown(Shown, Filter)) {
        if (Shown.Group == GroupBy::Living && Group != One->Where) {
            Group = One->Where;
            Out.push_back(Line{Line::Kind::Group, nullptr, One->Where, 0});
        }
        Out.push_back(Line{Line::Kind::Program, One, One->Where, 0});
        const bool Opened = std::find(Open.begin(), Open.end(), WhoOf(*One)) != Open.end();
        if (Opened != Shown.Strands)
            for (std::size_t Strand = 0; Strand < One->Seen.Strands.size(); Strand++)
                Out.push_back(Line{Line::Kind::Strand, One, One->Where, Strand});
    }
    return Out;
}

const Program *Board::Chosen() const
{
    return Choice ? Known.Find(Choice->Pid, Choice->Generation) : nullptr;
}

void Board::Move(std::ptrdiff_t By)
{
    std::vector<const Program *> Listed;
    for (const Line &One : Lines())
        if (One.What == Line::Kind::Program) Listed.push_back(One.Of);
    if (Listed.empty()) {
        Choice.reset();
        return;
    }
    // The chosen one where it is now; one that ended, or that the filter
    // leaves out, gives its place to the program now there.
    std::ptrdiff_t  At    = static_cast<std::ptrdiff_t>(ChoiceAt);
    const Program  *Was   = Chosen();
    const auto      Found = std::find(Listed.begin(), Listed.end(), Was);
    if (Was && Found != Listed.end()) At = Found - Listed.begin();
    At       = std::clamp<std::ptrdiff_t>(At + By, 0, static_cast<std::ptrdiff_t>(Listed.size()) - 1);
    ChoiceAt = static_cast<std::size_t>(At);
    Choice   = WhoOf(*Listed[ChoiceAt]);
}

void Board::ToggleOpen()
{
    if (!Choice) return;
    if (const auto Found = std::find(Open.begin(), Open.end(), *Choice); Found != Open.end()) Open.erase(Found);
    else Open.push_back(*Choice);
}

void Board::ToggleMark()
{
    const Program *One = Chosen();
    if (!One) return;
    const auto Found =
        std::find_if(Marked.begin(), Marked.end(), [&](const Mark &Was) { return Was.Program == *Choice; });
    if (Found != Marked.end()) Marked.erase(Found);
    else Marked.push_back(Mark{*Choice, One->Seen.File});
}

std::string Board::Spelled(BoardKey Key) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Shown.Keys[Key], Spelling, sizeof(Spelling));
    return Spelling;
}

// The end key over the chosen program: asked to finish first, so it can keep
// what it holds; ended at once when the key comes again. One the machine runs
// on is left to finish's "anyway", and the board itself to its own Esc.
void Board::EndChosen()
{
    const Program *One = Chosen();
    if (!One) return;
    const std::string &Name = One->Name;
    if (One->Seen.Pid == cabin_info()->pid) {
        Status = "the board itself ends with esc";
        return;
    }
    if (One->System) {
        Status = std::format("{} runs the machine: it is not ended from here — finish {} anyway ends it", Name, Name);
        return;
    }
    const Who Id = WhoOf(*One);
    if (const auto Was = std::find(Asked.begin(), Asked.end(), Id); Was != Asked.end()) {
        Asked.erase(Was);
        const int Rc = proc_finish(Id.Pid, Id.Generation);
        Status       = Rc == 0 ? std::format("{} ended", Name) : std::format("{} could not be ended: {}", Name, Why(Rc));
        if (Rc == 0) Said.Say(Happened::Board, std::format("{} ended from the board", Name));
        return;
    }
    Asked.push_back(Id);
    const int Rc = display_ask_end(Id.Pid, Id.Generation);
    if (Rc == 1) {
        Status = std::format("{} is asked to finish — {} again ends it now", Name, Spelled(KeyEnd));
        Said.Say(Happened::Board, std::format("{} asked to finish", Name));
    } else if (Rc == 0) {
        Status = std::format("{} reads no keys, so it cannot be asked — {} again ends it", Name, Spelled(KeyEnd));
    } else {
        Status = std::format("{} could not be asked to finish ({}) — {} again ends it", Name, Why(Rc),
                             Spelled(KeyEnd));
    }
}

bool Board::OnKey(const tui::KeyEvent &Key)
{
    if (Help) {
        Help = false;
        return true;
    }
    Status.clear();
    const kb_event_t Raw{Key.Character, Key.Scancode, Key.Mods};
    const auto       Is = [&](BoardKey Which) { return KeyChordMatches(&Shown.Keys[Which], &Raw); };
    if (Is(KeyHelp)) {
        Help = true;
        return true;
    }
    if (Is(KeyEnd)) {
        EndChosen();
        return true;
    }
    if (Is(KeySort)) {
        static constexpr SortBy Next[] = {SortBy::Memory, SortBy::Name, SortBy::Age, SortBy::Processor};
        Shown.Sort = Next[static_cast<std::size_t>(Shown.Sort)];
        Move(0);
        return true;
    }
    if (Is(KeyGroup)) {
        Shown.Group = Shown.Group == GroupBy::Living ? GroupBy::None : GroupBy::Living;
        Move(0);
        return true;
    }
    if (Key.IsCtrl() || Key.IsAlt()) return true;

    if (Key.IsExtended()) {
        switch (Key.Scancode) {
        case tui::KeyCodes::Up:       Move(-1); break;
        case tui::KeyCodes::Down:     Move(1); break;
        case tui::KeyCodes::PageUp:   Move(-PageStep); break;
        case tui::KeyCodes::PageDown: Move(PageStep); break;
        case tui::KeyCodes::Home:     Move(-static_cast<std::ptrdiff_t>(Known.All().size())); break;
        case tui::KeyCodes::End:      Move(static_cast<std::ptrdiff_t>(Known.All().size())); break;
        default:                      break;
        }
        return true;
    }
    switch (Key.Character) {
    case tui::KeyCodes::Escape:
        if (Filter.empty()) return false;
        Filter.clear();
        Move(0);
        return true;
    case tui::KeyCodes::Enter:
    case '\r':
        ToggleOpen();
        return true;
    case ' ':
        ToggleMark();
        return true;
    case tui::KeyCodes::Backspace:
        if (!Filter.empty()) Filter.pop_back();
        Move(0);
        return true;
    default:
        break;
    }
    if (Key.Character > ' ' && Key.Character < 0x7F) {
        Filter += Key.Character;
        Move(0);
    }
    return true;
}

}  // namespace taskboard
