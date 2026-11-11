/* view.cpp — the garden's reading, its ranking and its keys. */

#include "view.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>

#include "box/clock.h"
#include "box/system.h"

namespace garden {

namespace {

namespace tui = painter::tui;

// A frame of the wind and the wings; the list and a window over the garden
// are still, and wait for a key or for the gardener.
constexpr std::uint32_t FrameMs = 70;
constexpr std::uint32_t StillMs = 400;
constexpr std::int32_t  SparkleFrames = 24;
constexpr std::int32_t  SlotLeast = 11;
constexpr std::ptrdiff_t PageStep = 5;

bool Contains(const std::string &Text, const std::string &Part)
{
    return std::search(Text.begin(), Text.end(), Part.begin(), Part.end(), [](char A, char B) {
               return std::tolower(static_cast<unsigned char>(A)) == std::tolower(static_cast<unsigned char>(B));
           }) != Text.end();
}

}  // namespace

View::View(tui::Stage Opened, const std::string &Focus) : Screen(std::move(Opened)), Shown(ReadLook())
{
    Grew = touch_pair_choose(GrewTag());
    if (Grew == TOUCH_TAG_INVALID || touch_claim(Grew, TOUCH_REST, 0, 0) != 0) {
        Grew   = TOUCH_TAG_INVALID;
        Status = "the gardener cannot be heard: the garden shows what was kept when it opened";
    }
    Read();
    Settled = true;
    if (Focus.empty()) return;
    const auto Found = std::find_if(Ranking.begin(), Ranking.end(), [&](const Plant *One) { return One->Name == Focus; });
    const auto Near  = Found != Ranking.end() ? Found : std::find_if(Ranking.begin(), Ranking.end(), [&](const Plant *One) {
                                                            return Contains(One->Name, Focus);
                                                        });
    if (Near == Ranking.end()) {
        Status = std::format("no plant here is called {}", Focus);
        return;
    }
    Choice = static_cast<std::size_t>(Near - Ranking.begin());
    Wanted = (*Near)->Name;
}

View::~View()
{
    if (Grew != TOUCH_TAG_INVALID) touch_release(Grew);
}

void View::Run()
{
    Screen.HearEnd();
    LastUs = clock_uptime_us();
    for (;;) {
        if (Watch.changed()) {
            Shown = ReadLook();
            Rank();
        }
        Touch Heard{};
        bool  Grown = false;
        while (Grew != TOUCH_TAG_INVALID && touch_try_pop_tag(Grew, &Heard)) Grown = true;
        if (Grown) Read();
        Advance();
        Paint();
        if (!Screen.Present()) return;

        const tui::Event Said = Screen.Wait(List || Help ? StillMs : FrameMs);
        if (Said.Kind == tui::EventKind::Gone || Said.Kind == tui::EventKind::EndAsked) return;
        if (Said.Kind == tui::EventKind::Key && !OnKey(Said.Key)) return;
    }
}

void View::Read()
{
    if (const std::optional<std::string> Trouble = ReadRecord(Kept)) Status = *Trouble;
    ProcMate *Raw = nullptr;
    Keeper        = proc_crew("gardener", &Raw, nullptr) > 0;
    free(Raw);
    Rank();
}

// The plants ranked again; the chosen one kept where it moved to.
void View::Rank()
{
    Ranking = Ranked(Kept, Shown);
    const auto Found = std::find_if(Ranking.begin(), Ranking.end(), [&](const Plant *One) { return One->Name == Wanted; });
    Choice = Found != Ranking.end() ? static_cast<std::size_t>(Found - Ranking.begin())
                                    : std::min(Choice, Ranking.empty() ? std::size_t{0} : Ranking.size() - 1);
    Wanted = Chosen() ? Chosen()->Name : std::string();
    for (const Plant *One : Ranking) {
        Grower &Grows = GrowerOf(*One);
        if (Grows.Starts < One->Starts) Grows.Pulse = SparkleFrames;
        Grows.Starts = One->Starts;
    }
}

View::Grower &View::GrowerOf(const Plant &One)
{
    const auto Found = std::find_if(Growers.begin(), Growers.end(), [&](const Grower &Is) { return Is.Name == One.Name; });
    if (Found != Growers.end()) return *Found;
    Growers.push_back(Grower{One.Name, 0.0, Settled ? SparkleFrames : 0, One.Starts});
    return Growers.back();
}

// The wind's time, and each plant's growth a little toward its height.
void View::Advance()
{
    const std::uint64_t Now = clock_uptime_us();
    const double        Passed = std::min(0.25, static_cast<double>(Now - LastUs) / 1e6);
    LastUs = Now;
    Time += Passed * Shown.Speed / 100.0;
    const double Room  = LayoutOf().Room;
    const double Scale = std::min(1.0, Room / TallestHeight);
    for (const Plant *One : Ranking) {
        Grower      &Grows  = GrowerOf(*One);
        const double Target = std::min(GrowthOf(*One, clock_unix_now(), Shown).Height * Scale, Room);
        Grows.Height += (Target - Grows.Height) * (1.0 - std::exp(-3.0 * Passed));
        if (std::abs(Target - Grows.Height) < 0.05) Grows.Height = Target;
        if (Grows.Pulse > 0) Grows.Pulse--;
    }
}

View::Layout View::LayoutOf() const
{
    const painter::core::Size Size = Screen.Size();
    Layout                    Where;
    Where.LabelRow  = Size.Height - 5;
    Where.Sky       = Scene{Size.Width, 2, 2 * Where.LabelRow - 2, 2 * Where.LabelRow};
    Where.Slots     = std::max(1, (Size.Width - 2) / SlotLeast);
    Where.SlotWidth = (Size.Width - 2) / Where.Slots;
    Where.Room      = Where.Sky.Ground - Where.Sky.Top - 4;
    return Where;
}

const Plant *View::Chosen() const noexcept
{
    return Choice < Ranking.size() ? Ranking[Choice] : nullptr;
}

void View::Scroll(std::size_t Visible)
{
    if (Choice < First) First = Choice;
    else if (Choice >= First + Visible) First = Choice - Visible + 1;
    First = std::min(First, Ranking.size() > Visible ? Ranking.size() - Visible : std::size_t{0});
}

void View::Move(std::ptrdiff_t By)
{
    if (Ranking.empty()) return;
    const std::ptrdiff_t Last = static_cast<std::ptrdiff_t>(Ranking.size()) - 1;
    Choice = static_cast<std::size_t>(std::clamp<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(Choice) + By, 0, Last));
    Wanted = Ranking[Choice]->Name;
}

bool View::IsMarked(const Plant &One) const
{
    return std::find(Marked.begin(), Marked.end(), One.Name) != Marked.end();
}

void View::Mark()
{
    const Plant *One = Chosen();
    if (!One) return;
    if (const auto Found = std::find(Marked.begin(), Marked.end(), One->Name); Found != Marked.end()) Marked.erase(Found);
    else Marked.push_back(One->Name);
}

std::string View::Spelled(GardenKey Key) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Shown.Keys[Key], Spelling, sizeof(Spelling));
    return Spelling;
}

bool View::OnKey(const tui::KeyEvent &Key)
{
    if (Help) {
        Help = false;
        return true;
    }
    Status.clear();
    const kb_event_t Raw{Key.Scancode, Key.Ascii, Key.Mods};
    const auto       Is = [&](GardenKey Which) { return KeyChordMatches(&Shown.Keys[Which], &Raw); };
    if (Is(KeyHelp)) {
        Help = true;
        return true;
    }
    if (Is(KeyView)) {
        List = !List;
        return true;
    }
    if (Is(KeyOrder)) {
        static constexpr Order Next[] = {Order::Recent, Order::Name, Order::Starts};
        Shown.Ranking = Next[static_cast<std::size_t>(Shown.Ranking)];
        Rank();
        Status = std::format("ranked by {}", OrderName(Shown.Ranking));
        return true;
    }
    if (Key.IsCtrl() || Key.IsAlt()) return true;

    if (Key.IsExtended()) {
        const std::ptrdiff_t Page = List ? PageStep : static_cast<std::ptrdiff_t>(LayoutOf().Slots);
        switch (Key.Scancode) {
        case tui::KeyCodes::Left:
        case tui::KeyCodes::Up:       Move(-1); break;
        case tui::KeyCodes::Right:
        case tui::KeyCodes::Down:     Move(1); break;
        case tui::KeyCodes::PageUp:   Move(-Page); break;
        case tui::KeyCodes::PageDown: Move(Page); break;
        case tui::KeyCodes::Home:     Move(-static_cast<std::ptrdiff_t>(Ranking.size())); break;
        case tui::KeyCodes::End:      Move(static_cast<std::ptrdiff_t>(Ranking.size())); break;
        default:                      break;
        }
        return true;
    }
    if (Key.Ascii == tui::KeyCodes::Escape) return false;
    if (Key.Ascii == ' ') Mark();
    return true;
}

}  // namespace garden
