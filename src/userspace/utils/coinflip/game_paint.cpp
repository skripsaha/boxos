/* game_paint.cpp — the picture: the coin and its shadow on the floor, the
 * word it fell on, the tally and the lines of keys. */

#include "game.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#include "painter/core/color.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "coin.h"
#include "word.h"

namespace coinflip {

namespace {

namespace tui  = painter::tui;
namespace core = painter::core;

// Where everything stands on the glass, in cells, but for the coin's room
// which is in pixels, two to a row.
struct Layout {
    std::int32_t Width;
    std::int32_t StageRows;
    std::int32_t WordTop;      // the row the word's room begins at
    std::int32_t WordRows;
    std::int32_t TallyTop;
    std::int32_t TallyRows;
    std::int32_t KeyRow;
    std::int32_t Dot;          // pixels to a dot of the word's letters
};

Layout Arrange(core::Size Glass, bool Batch)
{
    Layout Is{};
    Is.Width     = Glass.Width;
    Is.Dot       = Glass.Height >= 40 ? 2 : 1;
    Is.WordRows  = LetterRows * Is.Dot / 2 + 1;
    Is.TallyRows = Batch ? 4 : 3;
    Is.StageRows = Glass.Height - 2 - Is.WordRows - Is.TallyRows;
    Is.WordTop   = 1 + Is.StageRows;
    Is.TallyTop  = Is.WordTop + Is.WordRows;
    Is.KeyRow    = Glass.Height - 1;
    return Is;
}

std::string_view WordOf(Face Landed)
{
    return Landed == Face::Heads ? "HEADS" : "TAILS";
}

// How many of Total, as a whole percent.
std::uint64_t Percent(std::uint64_t Part, std::uint64_t Total)
{
    return Total == 0 ? 0 : (Part * 100 + Total / 2) / Total;
}

}  // namespace

std::string Game::Spelled(CoinKey Key) const
{
    char Spelling[KEY_CHORD_SPELLED_MAX];
    KeyChordSpell(&Shown.Keys[Key], Spelling, sizeof(Spelling));
    return Spelling;
}

std::string Game::Verdict() const
{
    if (!Wish.Called) return {};
    const std::string_view Called = WordOf(*Wish.Called);
    if (Aloft || Counted.Total() == 0) return std::format("calling {}", Called);
    return std::format("called {}: {}", Called, Landing == *Wish.Called ? "won" : "lost");
}

void Game::Paint(std::uint64_t Now)
{
    tui::Frame &Out = Screen.FrameBuffer();
    Out.Clear(tui::TextStyle{Shown.Page, Shown.Page});
    PaintScene(Now);
    PaintResult();
    PaintTally();
    PaintBars();
    if (!Luck::Honest()) Out.Write({1, 1}, "clock-drawn: no hardware entropy", tui::TextStyle{Shown.Quiet, Shown.Page});
    if (Help) PaintHelp();
}

void Game::PaintScene(std::uint64_t Now)
{
    tui::Frame        &Out    = Screen.FrameBuffer();
    const Layout       Is     = Arrange(Screen.Size(), Wish.Count > 1);
    const std::int32_t Top    = 2;
    const std::int32_t Height = Is.StageRows * 2;
    const std::int32_t Edge   = Top + Height;

    const double Radius =
        std::max(3.0, std::min({Shown.Size / 100.0 * 0.42 * Height, 0.30 * Height, Is.Width / 5.0}));
    const std::int32_t Near     = std::max(3, Height / 9);
    const std::int32_t Floor    = Edge - Near;
    const std::int32_t Horizon  = Floor - std::max(2, static_cast<std::int32_t>(Radius / 5.0));
    const double       Reached  = std::max(0.0, static_cast<double>(Floor - Top) - 2.0 * Radius);

    for (std::int32_t Y = Horizon; Y < Edge; Y++) {
        const auto Depth = static_cast<std::uint8_t>((Y - Horizon) * 150 / std::max(1, Edge - Horizon));
        const core::Color Ink = Y == Horizon ? core::Color::Mix(Shown.Floor, core::Colors::White, 40)
                                             : core::Color::Mix(Shown.Floor, Shown.Page, Depth);
        for (std::int32_t X = 0; X < Is.Width; X++) Out.PutPixel({X, Y}, Ink);
    }

    Pose Where{0.0, 0.0, true};
    if (Current) Where = Current->At(Aloft ? static_cast<double>(Now - StartedUs) / 1e6 : Current->Length());
    const double CentreX = Is.Width / 2.0;

    // The shadow shrinks and fades as the coin goes up.
    const double Near1 = 1.0 - 0.45 * Where.Lift;
    const double Wide  = Radius * Near1;
    const double High  = std::max(1.0, Radius * 0.2 * Near1);
    const auto   Dark  = static_cast<std::uint8_t>(150.0 * (1.0 - 0.6 * Where.Lift));
    for (auto Y = static_cast<std::int32_t>(Floor - High); Y <= static_cast<std::int32_t>(Floor + High) + 1; Y++)
        for (auto X = static_cast<std::int32_t>(CentreX - Wide); X <= static_cast<std::int32_t>(CentreX + Wide) + 1; X++) {
            const double A = (X + 0.5 - CentreX) / Wide;
            const double B = (Y + 0.5 - Floor) / High;
            if (A * A + B * B <= 1.0) Out.BlendPixel({X, Y}, core::Colors::Black, Dark);
        }

    const double CentreY = Floor - Where.Lift * Reached - Radius * Reach(Where.Turn);
    PaintCoin(Out, Shown, CentreX, CentreY, Radius, Where.Turn);
}

void Game::PaintResult()
{
    tui::Frame  &Out = Screen.FrameBuffer();
    const Layout Is  = Arrange(Screen.Size(), Wish.Count > 1);
    if (Aloft) {
        Out.Centre(Is.WordTop + Is.WordRows / 2, "in the air", tui::TextStyle{Shown.Quiet, Shown.Page});
        return;
    }
    if (!Current) return;
    const std::string_view Word = WordOf(Landing);
    const std::int32_t     Left = (Is.Width - WordWidth(Word, Is.Dot)) / 2;
    const std::int32_t     Top  = Is.WordTop * 2 + (Is.WordRows * 2 - LetterRows * Is.Dot) / 2;
    PaintWord(Out, Left, Top, Word, Is.Dot, Landing == Face::Heads ? Shown.HeadsInk : Shown.TailsInk);
}

void Game::PaintTally()
{
    tui::Frame  &Out   = Screen.FrameBuffer();
    const Layout Is    = Arrange(Screen.Size(), Wish.Count > 1);
    const auto   Total = Counted.Total();
    const auto   Heads = Counted.Count(Face::Heads);
    const auto   Tails = Counted.Count(Face::Tails);
    const auto   Quiet = tui::TextStyle{Shown.Quiet, Shown.Page};
    if (Total == 0) {
        Out.Centre(Is.TallyTop, "no flips yet", Quiet);
        return;
    }

    const std::string Headed = std::format("heads {} ({}%)", Heads, Percent(Heads, Total));
    const std::string Tailed = std::format("tails {} ({}%)", Tails, Percent(Tails, Total));
    const std::int32_t Gap   = 4;
    const std::int32_t Start =
        (Is.Width - static_cast<std::int32_t>(Headed.size() + Tailed.size()) - Gap) / 2;
    Out.Write({Start, Is.TallyTop}, Headed, tui::TextStyle{Shown.HeadsInk, Shown.Page});
    Out.Write({Start + static_cast<std::int32_t>(Headed.size()) + Gap, Is.TallyTop}, Tailed,
              tui::TextStyle{Shown.TailsInk, Shown.Page});

    // The bar: heads at the left, tails at the right, in the proportion they fell.
    const std::int32_t Bar   = std::max(8, std::min(Is.Width - 8, 60));
    const std::int32_t Left  = (Is.Width - Bar) / 2;
    const auto         Share = static_cast<std::int32_t>(Percent(Heads, Total) * Bar / 100);
    Out.Fill({Left, Is.TallyTop + 1, Share, 1}, ' ', tui::TextStyle{Shown.Page, Shown.HeadsInk});
    Out.Fill({Left + Share, Is.TallyTop + 1, Bar - Share, 1}, ' ', tui::TextStyle{Shown.Page, Shown.TailsInk});

    std::string Runs = std::format("run: {} {}   longest: heads {}, tails {}", Counted.RunLength(),
                                   Counted.RunFace() == Face::Heads ? "heads" : "tails",
                                   Counted.Longest(Face::Heads), Counted.Longest(Face::Tails));
    if (Counted.Calls() > 0) Runs += std::format("   calls won: {} of {}", Counted.Won(), Counted.Calls());
    Out.Centre(Is.TallyTop + 2, Runs, tui::TextStyle{Shown.Text, Shown.Page});

    if (Wish.Count <= 1) return;
    const std::string &Recent = Counted.Recent();
    const std::size_t  Room   = static_cast<std::size_t>(std::max(1, Is.Width - 4));
    const std::size_t  From   = Recent.size() > Room ? Recent.size() - Room : 0;
    const std::int32_t Begin  = (Is.Width - static_cast<std::int32_t>(Recent.size() - From)) / 2;
    for (std::size_t At = From; At < Recent.size(); At++)
        Out.Put({Begin + static_cast<std::int32_t>(At - From), Is.TallyTop + 3}, Recent[At],
                tui::TextStyle{Recent[At] == 'H' ? Shown.HeadsInk : Shown.TailsInk, Shown.Page});
}

void Game::PaintBars()
{
    tui::Frame        &Out   = Screen.FrameBuffer();
    const Layout       Is    = Arrange(Screen.Size(), Wish.Count > 1);
    const tui::TextStyle Bar{Shown.Text, Shown.Floor};
    const tui::TextStyle Dim{Shown.Quiet, Shown.Floor};
    Out.Fill({0, 0, Is.Width, 1}, ' ', Bar);
    Out.Fill({0, Is.KeyRow, Is.Width, 1}, ' ', Bar);

    // What is said of the run goes after the name: the display keeps the top
    // right of the glass for its own clock and load.
    const std::int32_t After = static_cast<std::int32_t>(Out.Write({1, 0}, "coinflip", tui::TextStyle{Shown.Metal, Shown.Floor})) + 3;
    std::string Says = Wish.Count > 1 ? std::format("flip {} of {}", std::min(Done + (Aloft ? 1 : 0), Wish.Count), Wish.Count)
                                      : std::string();
    if (const std::string Call = Verdict(); !Call.empty()) Says += Says.empty() ? Call : "   " + Call;
    const bool Won = Wish.Called && !Aloft && Counted.Total() > 0 && Landing == *Wish.Called;
    const core::Color Ink = !Wish.Called || Aloft || Counted.Total() == 0 ? Shown.Text : Won ? Shown.Won : Shown.Lost;
    Out.Write({After, 0}, Says, tui::TextStyle{Ink, Shown.Floor});

    const std::vector<std::pair<std::string, std::string>> Keys = {
        {"space", Running() && Wish.Count > 1 ? "finish the run" : "flip"},
        {"h", "call heads"},
        {"t", "call tails"},
        {Spelled(KeyHelp), "keys"},
        {"esc", "leave"}};
    std::int32_t X = 1;
    for (const auto &[Key, Does] : Keys) {
        const auto Needs = static_cast<std::int32_t>(Key.size() + 1 + Does.size() + 2);
        if (X + Needs > Is.Width) break;
        X += static_cast<std::int32_t>(Out.Write({X, Is.KeyRow}, Key, Bar)) + 1;
        X += static_cast<std::int32_t>(Out.Write({X, Is.KeyRow}, Does, Dim)) + 2;
    }
}

void Game::PaintHelp()
{
    tui::Frame &Out = Screen.FrameBuffer();
    const std::pair<std::string, std::string> Keys[] = {
        {"space, enter", "toss the coin; in a run of many, finish the run at once"},
        {Spelled(KeyFlip), "toss it again"},
        {"h, t", "call heads or tails, and toss"},
        {Spelled(KeyHeads), "call heads, and toss"},
        {Spelled(KeyTails), "call tails, and toss"},
        {"backspace", "take the call back"},
        {Spelled(KeyHelp), "these keys"},
        {"esc", "leave"},
    };
    constexpr std::int32_t KeyWidth = 16;
    const core::Size Size   = Screen.Size();
    const core::Rect Area   = tui::Centred(Size, std::min(Size.Width - 2, 70), static_cast<std::int32_t>(std::size(Keys)) + 4);
    const core::Rect Inside = tui::DrawWindow(Out, Area, "the coin's keys", "settings coinflip keys", "any key closes this");
    std::int32_t     Row    = Inside.Y + 1;
    for (const auto &[Key, Does] : Keys) {
        if (Row >= Inside.Y + Inside.Height) break;
        tui::PutText(Out, {Inside.X + 2, Row}, Key, KeyWidth, tui::TitleInk, tui::WindowPaper);
        tui::PutText(Out, {Inside.X + 2 + KeyWidth, Row}, Does, Inside.Width - KeyWidth - 3, tui::WindowInk,
                     tui::WindowPaper);
        Row++;
    }
}

}  // namespace coinflip
