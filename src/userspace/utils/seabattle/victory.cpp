/* victory.cpp — the end of the battle: the winner's name between two waving
 * flags, fireworks over the sea, both players' waters laid bare with every
 * ship and every shot, the figures of the game, and what to do next. */

#include <algorithm>
#include <array>
#include <format>

#include "painter/tui/text.h"

#include "board_view.h"
#include "effects.h"
#include "hud.h"
#include "layout.h"
#include "pages.h"
#include "sea.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr std::array<std::uint32_t, 6> Colours = {0xFF5A5F, 0xFFC857, 0x7BE08F, 0x6EC5FF, 0xC792EA, 0xFFFFFF};
constexpr std::uint32_t SaluteLife = 2300;
constexpr std::uint32_t SaluteEvery = 520;

struct Choice {
    const char *Words;
    PageId      Goes;
    bool        Again;      // the same game once more, the fleets to be laid anew
};

constexpr std::array<Choice, 4> Choices = {{{"Rematch", PageId::Place, true}, {"New setup", PageId::Setup, false}, {"Main menu", PageId::Title, false}, {"Quit", PageId::Quit, false}}};

// A pennant on a pole, its cloth in three bands, rippling from the pole outwards.
void PaintFlag(Canvas &Out, int X, int Y, std::uint64_t Ms, std::array<Color, 3> Bands, bool Right)
{
    const int Wide = 15, High = 6;
    Out.Fill(X, Y - 1, 1, High + 5, Rgb(0x8A, 0x6A, 0x44));
    for (int Across = 0; Across < Wide; Across++) {
        const float Ripple = Wave(static_cast<float>(Across) * 0.085f - static_cast<float>(Ms % 1800) / 1800.0f);
        const float Lift = Ripple * 1.4f * static_cast<float>(Across) / static_cast<float>(Wide);
        const int   Tip = High - Across * High / (2 * Wide);
        for (int Down = 0; Down < Tip; Down++) {
            const Color Cloth = Bands[static_cast<std::size_t>(Down * 3 / High)];
            const int   Column = Right ? X + 1 + Across : X - 1 - Across;
            Out.Put(Column, Y + Down + static_cast<int>(Lift), Toned(Cloth, 0.86f + 0.16f * Ripple));
        }
    }
}

class VictoryPage final : public Page {
public:
    explicit VictoryPage(Context &Shared)
        : Page(Shared), Winner(Shared.Game->Winner()), Place(RevealFor(Shared.Screen.Size(), Shared.Game->Chosen().Grid)), NextSalute(Shared.Now)
    {
    }

    void Step(std::uint32_t ElapsedMs) override;
    void Key(const tui::KeyEvent &Pressed) override;
    std::uint32_t FrameMs() const override { return 60; }

protected:
    void Pixels(Canvas &Out) override;
    void Shapes(tui::FineCanvas &Out) override;
    void Words(tui::Frame &Out) override;

private:
    BoardScene SceneOf(int Side) const
    {
        return BoardScene{.Waters = Ctx.Game->At(Side).Waters, .At = Side == 0 ? Place.Aim : Place.Own, .Shown = Ctx.Chosen, .Ms = Ctx.Now,
                          .Text = Ctx.Text(), .ShowShips = true};
    }
    void PaintStats(tui::Frame &Out) const;

    int           Winner;
    Arena         Place;
    std::uint64_t NextSalute;
    Effects       Fx;
    int           Picked = 0;
    std::uint32_t Count = 0;
};

void VictoryPage::Step(std::uint32_t ElapsedMs)
{
    (void)ElapsedMs;
    Fx.Prune(Ctx.Now);
    if (Ctx.Now < NextSalute) return;
    NextSalute = Ctx.Now + SaluteEvery;
    const Canvas &Glass = Ctx.Picture;
    const auto    Across = static_cast<float>(8 + Ctx.Chance.Below(static_cast<std::uint32_t>(std::max(1, Glass.Width - 16))));
    const auto    Height = static_cast<float>(6 + Ctx.Chance.Below(static_cast<std::uint32_t>(std::max(1, Glass.Height * 2 / 5))));
    Fx.Add(Effect{.Kind = EffectKind::Salute,
                  .X = Across,
                  .Y = Height,
                  .FromY = static_cast<float>(Glass.Height - 2),
                  .Reach = static_cast<float>(3 + Ctx.Chance.Below(3)),
                  .Born = Ctx.Now,
                  .Life = SaluteLife,
                  .Tone = Color::FromRaw(Colours[Count++ % Colours.size()])});
}

void VictoryPage::Key(const tui::KeyEvent &Pressed)
{
    const auto Total = static_cast<int>(Choices.size());
    if (Pressed.IsExtended()) {
        if (Pressed.Scancode == tui::KeyCodes::Left) Picked = (Picked + Total - 1) % Total;
        else if (Pressed.Scancode == tui::KeyCodes::Right) Picked = (Picked + 1) % Total;
        return;
    }
    if (Pressed.IsCtrl() || Pressed.IsAlt()) return;
    if (Pressed.Ascii == tui::KeyCodes::Tab) {
        Picked = (Picked + 1) % Total;
    } else if (Pressed.Ascii == tui::KeyCodes::Escape) {
        Ctx.Go(PageId::Title);
    } else if (Pressed.Ascii == tui::KeyCodes::Enter || Pressed.Ascii == ' ') {
        const Choice &Chosen = Choices[static_cast<std::size_t>(Picked)];
        if (Chosen.Again) Ctx.StartGame();
        else Ctx.Go(Chosen.Goes);
    }
}

void VictoryPage::Pixels(Canvas &Out)
{
    PaintBackdrop(Out, Ctx.Chosen.Sea, Ctx.Text(), Ctx.Now);
    PaintBoard(Out, SceneOf(0));
    PaintBoard(Out, SceneOf(1));
    const std::array<Color, 3> Bands = {Ctx.Chosen.Fire, Rgb(0xF4, 0xF4, 0xF4), Ctx.Chosen.Title};
    const int                  Middle = Out.Width / 2;
    PaintFlag(Out, Middle - 34, 3, Ctx.Now, Bands, true);
    PaintFlag(Out, Middle + 34, 3, Ctx.Now + 700, Bands, false);
    Fx.DrawPixels(Out, Ctx.Now);
}

void VictoryPage::Shapes(tui::FineCanvas &Out)
{
    ShapeBoard(Out, SceneOf(0));
    ShapeBoard(Out, SceneOf(1));
    Fx.DrawFine(Out, Ctx.Now);
}

void VictoryPage::PaintStats(tui::Frame &Out) const
{
    const Look &Shown = Ctx.Chosen;
    const Stats &One = Ctx.Game->At(0).Record, &Two = Ctx.Game->At(1).Record;
    struct Line {
        const char *Label;
        int         First, Second;
        bool        Percent;
    };
    const Line Lines[] = {{"Shots", One.Shots, Two.Shots, false},           {"Hits", One.Hits, Two.Hits, false},
                          {"Accuracy", One.AccuracyPercent(), Two.AccuracyPercent(), true}, {"Ships sunk", One.Sunk, Two.Sunk, false},
                          {"Turns", One.Turns, Two.Turns, false},            {"Longest streak", One.BestStreak, Two.BestStreak, false}};
    const std::int32_t Middle = Out.Size().Width / 2;
    const std::int32_t Top = Place.Aim.RowBelow() + 1;
    tui::PutText(Out, core::Point{Middle - 24 - static_cast<std::int32_t>(tui::CellsOf(Ctx.Game->At(0).Name)), Top}, Ctx.Game->At(0).Name, 20, Winner == 0 ? Shown.Title : Shown.Text, Shown.Bars);
    tui::PutText(Out, core::Point{Middle + 10, Top}, Ctx.Game->At(1).Name, 20, Winner == 1 ? Shown.Title : Shown.Text, Shown.Bars);
    std::int32_t Row = Top + 1;
    for (const Line &One : Lines) {
        const auto Show = [&](int Value) { return std::format("{}{}", Value, One.Percent ? "%" : ""); };
        const std::string Left = Show(One.First), Right = Show(One.Second);
        PutCentred(Out, Middle - 12, 24, Row, One.Label, Shown.Quiet, Shown.Bars);
        tui::PutText(Out, core::Point{Middle - 14 - static_cast<std::int32_t>(Left.size()), Row}, Left, 8, One.First > One.Second ? Shown.Good : Shown.Text, Shown.Bars);
        tui::PutText(Out, core::Point{Middle + 14, Row}, Right, 8, One.Second > One.First ? Shown.Good : Shown.Text, Shown.Bars);
        Row++;
    }
}

void VictoryPage::Words(tui::Frame &Out)
{
    const Look &Shown = Ctx.Chosen;
    const core::Size Size = Out.Size();
    const Player &Won = Ctx.Game->At(Winner);
    PaintBar(Out, Shown, "SEA BATTLE", "the battle is over");
    PaintHints(Out, Shown, {{"\xE2\x86\x94", "choose", 1}, {"enter", "go", 0, true}, {"esc", "main menu", 2}});

    const std::string Banner = std::format("{} WINS", Won.Name);
    Out.Fill(core::Rect{Size.Width / 2 - 24, 1, 48, 1}, ' ', {Shown.Title, Shown.Bars});
    PutCentred(Out, Size.Width / 2 - 24, 48, 1, Banner, Beat(Ctx.Now, 900) > 0.5f ? Shown.Title : Lighter(Shown.Title, 0.4f), Shown.Bars);
    const std::string Detail = std::format("on turn {}, with {} shots and {} hits", Ctx.Game->TurnNumber(), Won.Record.Shots, Won.Record.Hits);
    Out.Fill(core::Rect{Size.Width / 2 - 24, 2, 48, 1}, ' ', {Shown.Text, Shown.Bars});
    PutCentred(Out, Size.Width / 2 - 24, 48, 2, Detail, Shown.Text, Shown.Bars);

    LabelBoard(Out, Shown, Place.Aim, std::format("{}'s waters", Ctx.Game->At(0).Name));
    LabelBoard(Out, Shown, Place.Own, std::format("{}'s waters", Ctx.Game->At(1).Name));
    PaintStats(Out);

    const std::int32_t Row = Size.Height - 3;
    Out.Fill(core::Rect{0, Row, Size.Width, 1}, ' ', {Shown.Text, Shown.Bars});
    std::int32_t Wide = 0;
    for (const Choice &One : Choices) Wide += static_cast<std::int32_t>(tui::CellsOf(One.Words)) + 6;
    std::int32_t Column = (Size.Width - Wide) / 2;
    for (std::size_t At = 0; At < Choices.size(); At++) {
        const bool Here = static_cast<int>(At) == Picked;
        const std::string Words = std::format("  {}  ", Choices[At].Words);
        const Color Under = Here ? Mixed(Shown.Bars, Shown.Title, 0.3f) : Shown.Bars;
        Column += tui::PutText(Out, core::Point{Column, Row}, Words, 40, Here ? Shown.Title : Shown.Text, Under) + 2;
    }
}

}  // namespace

std::unique_ptr<Page> MakeVictory(Context &Shared)
{
    return std::make_unique<VictoryPage>(Shared);
}

}  // namespace seabattle
