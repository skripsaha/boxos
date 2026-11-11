/* board.cpp — the map's reading, its cells and its keys. */

#include "board.h"

#include <algorithm>
#include <format>
#include <functional>
#include <utility>

#include "box/clock.h"
#include "box/cxx/error.h"

namespace memmap {

namespace {

namespace tui = painter::tui;

// The first reading comes at once; after it, the one the person set. The wait
// for a key is cut short so that a saved setting shows soon.
constexpr std::uint32_t LongestWaitMs = 500;

// A number that is the same for two readings that would be drawn the same:
// memory held by each owner, what each gained just now, and the regions.
std::uint64_t Fingerprint(const Reading &Now, const std::vector<Owner> &Owners)
{
    std::uint64_t Value = 14695981039346656037ull;
    const auto    Mix   = [&](std::uint64_t Part) { Value = (Value ^ Part) * 1099511628211ull; };
    Mix(Now.Total);
    Mix(Now.Used);
    for (const Owner &One : Owners) {
        Mix(std::hash<std::string>{}(One.Key));
        Mix(One.Bytes);
        Mix(One.Glow);
    }
    for (const RegionSeen &One : Now.Regions) {
        Mix(One.Base);
        Mix(One.Bytes);
        Mix(static_cast<std::uint64_t>(One.What) << 32 | One.Cabin);
    }
    return Value;
}

std::string Why(int Answer)
{
    return std::string(box::error(static_cast<::error_t>(Answer < 0 ? -Answer : Answer)).message());
}

}  // namespace

Board::Board(tui::Stage Opened, std::string Kept)
    : Screen(std::move(Opened)), Shown(ReadLook()), Filter(std::move(Kept))
{
}

void Board::Run()
{
    Screen.HearEnd();
    Read();
    NextReadUs = clock_uptime_us() + static_cast<std::uint64_t>(Shown.RefreshMs) * 1000u;
    for (;;) {
        if (Watch.changed()) {
            Shown = ReadLook();
            Remake();
            Rebuild();
            Dirty = true;
        }
        if (Dirty) {
            Paint();
            if (!Screen.Present()) return;
            Dirty = false;
        }
        const std::uint64_t Time = clock_uptime_us();
        std::uint32_t       Wait = LongestWaitMs;
        if (!Paused && NextReadUs > Time) Wait = std::min(Wait, static_cast<std::uint32_t>((NextReadUs - Time) / 1000u) + 1u);
        if (!Paused && NextReadUs <= Time) Wait = 1;
        const tui::Event Heard = Screen.Wait(Wait);
        if (Heard.Kind == tui::EventKind::Gone || Heard.Kind == tui::EventKind::EndAsked) return;
        if (Heard.Kind == tui::EventKind::Key) {
            Dirty = true;
            if (!OnKey(Heard.Key)) return;
        }
        if (!Paused && clock_uptime_us() >= NextReadUs) {
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
    std::expected<Reading, int> Fresh = Source.Read();
    if (!Fresh) {
        Status = std::format("the machine could not be read: {}", Why(Fresh.error()));
        Dirty  = true;
        return;
    }
    Now   = std::move(*Fresh);
    Parts = Resolve(Now->Regions, Now->Span);
    Remake();
    if (!Now->RegionsKnown) Status = "the registry of tagged memory could not be read: the address view has nothing in it";
    // A reading drawn as the one before is not drawn again, but for the time
    // a program has lived that the cursor's line may be saying.
    const std::uint64_t Before = Seen;
    Seen                       = Fingerprint(*Now, Fine);
    if (Seen == Before && !Ticks && Status.empty()) return;
    Rebuild();
    Dirty = true;
}

// Every owner of the reading, in the colors now set; what each held before is
// the ledger's, and the same reading taken twice changes none of it.
void Board::Remake()
{
    if (!Now) return;
    Fine = MemoryOwners(*Now, Shown);
    Kept.Take(Fine, Now->AtUs);
}

// The owners the view shows: made of the reading, grouped and put in order.
void Board::Rebuild()
{
    if (!Now) return;
    if (Shown.View == ViewOf::Owners) {
        Owners = Fine;
        Group(Owners, Shown);
    } else {
        Owners = AddressOwners(*Now, Shown, Parts);
    }
    std::erase_if(Owners, [](const Owner &One) { return One.Bytes == 0; });
    Order(Owners, Shown.Sort);
    Stale = true;
}

// The cells again, when the owners have changed or the glass has room for
// another number of them.
void Board::Lay()
{
    const std::size_t Capacity = static_cast<std::size_t>(std::max(0, Grid.Cols) * std::max(0, Grid.Rows));
    if (!Stale && LaidFor == Capacity) return;
    if (!Now || Capacity == 0) {
        Laid = Mosaic{};
    } else if (Shown.View == ViewOf::Owners) {
        Laid = LayOutOwners(Owners, Capacity);
    } else {
        Laid = LayOutAddress(Owners, Parts, Now->Span, Capacity);
    }
    LaidFor = Capacity;
    Stale   = false;
    Cursor  = Laid.Cells.empty() ? 0 : std::min(Cursor, Laid.Cells.size() - 1);
}

const Owner *Board::Chosen() const
{
    if (Stale || Cursor >= Laid.Cells.size() || Laid.Cells[Cursor].Owner >= Owners.size()) return nullptr;
    return &Owners[Laid.Cells[Cursor].Owner];
}

void Board::MoveCursor(std::ptrdiff_t By)
{
    if (Laid.Cells.empty()) return;
    const std::ptrdiff_t Last = static_cast<std::ptrdiff_t>(Laid.Cells.size()) - 1;
    Cursor = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(Cursor) + By, 0, Last));
}

void Board::ToggleMark()
{
    const Owner *One = Chosen();
    if (!One || !One->IsProgram()) return;
    const auto Found = std::find_if(Marked.begin(), Marked.end(), [&](const Mark &Was) { return Was.Key == One->Key; });
    if (Found != Marked.end()) Marked.erase(Found);
    else Marked.push_back(Mark{One->Key, One->File});
}

std::string Board::Spelled(MapKey Key) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Shown.Keys[Key], Spelling, sizeof(Spelling));
    return Spelling;
}

bool Board::OnKey(const tui::KeyEvent &Key)
{
    if (Help) {
        Help = false;
        return true;
    }
    Status.clear();
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const auto       Is = [&](MapKey Which) { return KeyChordMatches(&Shown.Keys[Which], &Raw); };
    if (Is(KeyHelp)) {
        Help = true;
        return true;
    }
    if (Is(KeyView)) {
        Shown.View = Shown.View == ViewOf::Owners ? ViewOf::Address : ViewOf::Owners;
        Rebuild();
        return true;
    }
    if (Is(KeyGroup)) {
        Shown.Group = Shown.Group == GroupBy::Program ? GroupBy::Kind : GroupBy::Program;
        Rebuild();
        return true;
    }
    if (Is(KeySort)) {
        static constexpr SortBy Next[] = {SortBy::Age, SortBy::Name, SortBy::Size};
        Shown.Sort = Next[static_cast<std::size_t>(Shown.Sort)];
        Rebuild();
        return true;
    }
    if (Is(KeyPause)) {
        Paused = !Paused;
        if (!Paused) NextReadUs = 0;
        return true;
    }
    if (Key.IsCtrl() || Key.IsAlt()) return true;

    if (Key.IsExtended()) {
        const std::ptrdiff_t Row  = std::max(1, Grid.Cols);
        const std::ptrdiff_t Page = Row * std::max(1, Grid.Rows - 1);
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:     MoveCursor(-1); break;
        case tui::KeyCodes::Right:    MoveCursor(1); break;
        case tui::KeyCodes::Up:       MoveCursor(-Row); break;
        case tui::KeyCodes::Down:     MoveCursor(Row); break;
        case tui::KeyCodes::PageUp:   MoveCursor(-Page); break;
        case tui::KeyCodes::PageDown: MoveCursor(Page); break;
        case tui::KeyCodes::Home:     Cursor = 0; break;
        case tui::KeyCodes::End:      MoveCursor(static_cast<std::ptrdiff_t>(Laid.Cells.size())); break;
        default:                      break;
        }
        return true;
    }
    switch (Key.Ascii) {
    case tui::KeyCodes::Escape:
        if (Filter.empty()) return false;
        Filter.clear();
        return true;
    case ' ':
        ToggleMark();
        return true;
    case tui::KeyCodes::Backspace:
        if (!Filter.empty()) Filter.pop_back();
        return true;
    default:
        break;
    }
    if (Key.Ascii > ' ' && Key.Ascii < 0x7F) Filter += Key.Ascii;
    return true;
}

}  // namespace memmap
