/* reveal.cpp — the reels, their timing and their picture. */

#include "reveal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include "box/time.h"

#include "painter/tui/console_surface.h"
#include "painter/tui/frame.h"

namespace random_pick {

namespace {

using painter::core::Color;
using painter::tui::ConsoleSurface;
using painter::tui::Frame;
using painter::tui::TextStyle;

// More reels than this are not a roll but a list.
constexpr std::size_t MostReels = 12;
constexpr int         Indent    = 2;
constexpr int         TurnMs    = 40;   // about how long the first turn of a reel lasts
constexpr int         TickMs    = 8;
constexpr Color       Backdrop  = painter::core::Colors::Black;

std::uint64_t UptimeMs()
{
    std::uint64_t Ms = 0;
    time_uptime_ms(&Ms);
    return Ms;
}

// Text the glass has every letter of: the font has no others than these.
bool Letters(const std::string &Text)
{
    return std::all_of(Text.begin(), Text.end(), [](char Each) { return Each >= 0x20 && Each <= 0x7E; });
}

// One reel: it turns through what the pool holds at moments that grow apart,
// slowing as it nears EndMs, and its last turn is the choice.
class Reel {
public:
    Reel(std::string Choice, std::uint32_t EndMs, std::string Start)
        : Chosen(std::move(Choice)), End(EndMs), Turns(std::max<std::uint32_t>(6, EndMs / TurnMs)),
          Text(std::move(Start))
    {
    }

    bool Settled() const noexcept { return Turned == Turns; }
    const std::string &Shown() const noexcept { return Text; }

    // The reel at ElapsedMs; whether it turned.
    bool Advance(std::uint64_t ElapsedMs, const Request &Asked, Source &Random)
    {
        bool Moved = false;
        while (!Settled() && ElapsedMs >= TurnAt(Turned + 1)) {
            Turned++;
            Text  = Settled() ? Chosen : Candidate(Asked, Random);
            Moved = true;
        }
        return Moved;
    }

private:
    // When turn Number happens: the speed falls to nothing at the end.
    std::uint64_t TurnAt(std::uint32_t Number) const
    {
        const double Part = static_cast<double>(Number) / static_cast<double>(Turns);
        return static_cast<std::uint64_t>(static_cast<double>(End) * (1.0 - std::sqrt(1.0 - Part)));
    }

    std::string   Chosen;
    std::uint32_t End;
    std::uint32_t Turns;
    std::uint32_t Turned = 0;
    std::string   Text;
};

// How many letters the widest of what might show is, or 0 when one is not
// for the glass.
std::size_t WidthOf(const Request &Asked, const std::vector<Drawn> &Chosen)
{
    std::size_t Widest = 0;
    for (const Drawn &One : Chosen) {
        if (!Letters(One.Text)) return 0;
        Widest = std::max(Widest, One.Text.size());
    }
    if (Asked.From == Kind::Range) {
        Widest = std::max({Widest, std::to_string(Asked.Low).size(), std::to_string(Asked.High).size()});
        return Widest;
    }
    for (const std::string &Name : Asked.Names) {
        if (!Letters(Name)) return 0;
        Widest = std::max(Widest, Name.size());
    }
    return Widest;
}

// The row of the console the reels begin at, the console scrolled up first
// when they would not fit under the cursor, with a row left for the prompt.
std::expected<int, box::error> RowToBegin(int Rows, ConsoleSurface &Console)
{
    const std::expected<painter::core::Point, box::error> Cursor = Console.Cursor();
    if (!Cursor) return std::unexpected(Cursor.error());
    int Top = Cursor->Y;
    for (const int Last = Console.Size().Height - Rows - 1; Top > Last; Top--)
        if (const box::status Scrolled = Console.ScrollUp(); !Scrolled) return std::unexpected(Scrolled.error());
    return Top;
}

void Picture(Frame &Canvas, const std::vector<Reel> &Reels, std::size_t Width, const Look &Shown)
{
    Canvas.Clear({Shown.Rolling, Backdrop});
    const int Closing = Indent + 3 + static_cast<int>(Width);
    for (std::size_t Row = 0; Row < Reels.size(); Row++) {
        const int Y = static_cast<int>(Row);
        Canvas.Put({Indent, Y}, '[', {Shown.Frame, Backdrop});
        Canvas.Put({Closing, Y}, ']', {Shown.Frame, Backdrop});
        Canvas.Write({Indent + 2, Y}, Reels[Row].Shown(), {Reels[Row].Settled() ? Shown.Chosen : Shown.Rolling, Backdrop});
    }
}

// The reels end one after another: the first soonest, the last when the roll
// has lasted as long as was set.
std::vector<Reel> MakeReels(const Request &Asked, const std::vector<Drawn> &Chosen, std::uint32_t RollMs,
                            Source &Random)
{
    std::vector<Reel> Reels;
    const std::size_t Count = Chosen.size();
    for (std::size_t Place = 0; Place < Count; Place++) {
        const std::uint32_t End = static_cast<std::uint32_t>(RollMs * (Count + Place + 1) / (2 * Count));
        Reels.emplace_back(Chosen[Place].Text, End, Candidate(Asked, Random));
    }
    return Reels;
}

}  // namespace

bool Reveal(const Request &Asked, const std::vector<Drawn> &Chosen, const Look &Shown, Source &Random)
{
    const std::size_t Width = WidthOf(Asked, Chosen);
    if (Chosen.empty() || Chosen.size() > MostReels || Width == 0) return false;

    std::expected<ConsoleSurface, painter::tui::StageError> Opened = ConsoleSurface::Open(30, 10);
    if (!Opened) return false;
    ConsoleSurface &Console = *Opened;
    const int       Rows    = static_cast<int>(Chosen.size());
    const int       Across  = Indent + static_cast<int>(Width) + 4;
    if (Across > Console.Size().Width || Rows + 2 > Console.Size().Height) return false;
    const std::expected<int, box::error> Top = RowToBegin(Rows, Console);
    if (!Top) return false;
    std::expected<Frame, box::error> Made = Frame::Create({Across, Rows});
    if (!Made) return false;

    std::vector<Reel> Reels = MakeReels(Asked, Chosen, Shown.RollMs, Random);
    const std::uint64_t Began = UptimeMs();
    for (bool Done = false; !Done;) {
        const std::uint64_t Elapsed = UptimeMs() - Began;
        Done = true;
        for (Reel &One : Reels) {
            One.Advance(Elapsed, Asked, Random);
            Done = Done && One.Settled();
        }
        Picture(*Made, Reels, Width, Shown);
        if (!Console.Paint({0, *Top}, *Made)) return false;
        if (!Done) std::this_thread::sleep_for(std::chrono::milliseconds(TickMs));
    }
    return Console.SetCursor({0, *Top + Rows}) && Console.Commit();
}

}  // namespace random_pick
