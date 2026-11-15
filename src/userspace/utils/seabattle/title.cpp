/* title.cpp — the title: the name in large pixel letters over a restless sea
 * with ships passing, two radars sweeping, and the menu. */

#include <algorithm>
#include <array>
#include <cmath>

#include "painter/tui/glyph.h"
#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "hud.h"
#include "pages.h"
#include "radar.h"
#include "sea.h"
#include "ship_art.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr int LetterWide = 5;
constexpr int LetterHigh = 7;

// S E A B T L, five pixels across and seven down.
constexpr std::array<std::array<const char *, LetterHigh>, 6> Letters = {{
    {".####", "#....", "#....", ".###.", "....#", "....#", "####."},
    {"#####", "#....", "#....", "####.", "#....", "#....", "#####"},
    {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"},
    {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."},
    {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."},
    {"#....", "#....", "#....", "#....", "#....", "#....", "#####"},
}};

// "SEA BATTLE" as places in Letters, -1 for the gap.
constexpr std::array<int, 10> Name = {0, 1, 2, -1, 3, 2, 4, 4, 5, 1};

struct Sailing {
    int   Length;
    float Row;         // of the glass's height
    float Speed;       // pixels a second
    float Start;
};

constexpr std::array<Sailing, 3> Passing = {{{4, 0.80f, 5.0f, 0.0f}, {3, 0.88f, 7.0f, 90.0f}, {2, 0.95f, 9.5f, 40.0f}}};

struct Item {
    const char *Words;
    PageId      Goes;
};

constexpr std::array<Item, 4> Menu = {{{"New game", PageId::Setup}, {"Rules", PageId::Rules}, {"Hall of records", PageId::Hall}, {"Quit", PageId::Quit}}};

class TitlePage final : public Page {
public:
    explicit TitlePage(Context &Shared) : Page(Shared) {}

    void Key(const tui::KeyEvent &Pressed) override
    {
        const auto Count = static_cast<int>(Menu.size());
        if (Pressed.IsExtended()) {
            if (Pressed.Scancode == tui::KeyCodes::Up) Choice = (Choice + Count - 1) % Count;
            else if (Pressed.Scancode == tui::KeyCodes::Down) Choice = (Choice + 1) % Count;
            else if (Pressed.Scancode == tui::KeyCodes::Home) Choice = 0;
            else if (Pressed.Scancode == tui::KeyCodes::End) Choice = Count - 1;
        } else if (Pressed.Character == tui::KeyCodes::Enter || Pressed.Character == ' ') {
            Ctx.Go(Menu[static_cast<std::size_t>(Choice)].Goes);
        } else if (Pressed.Character == tui::KeyCodes::Escape) {
            Ctx.Go(PageId::Quit);
        } else if (Pressed.Character == tui::KeyCodes::Tab) {
            Choice = (Choice + 1) % Count;
        }
    }

protected:
    void Pixels(Canvas &Out) override;
    void Words(tui::Frame &Out) override;

private:
    void PaintName(Canvas &Out) const;
    void PaintShips(Canvas &Out) const;

    int Choice = 0;
};

void TitlePage::PaintName(Canvas &Out) const
{
    const int Scale = std::clamp((Out.Width - 8) / 56, 1, 3);
    const int Left = (Out.Width - 56 * Scale) / 2, Top = 6;
    const Color Gold = Ctx.Chosen.Title;
    for (int Pass = 0; Pass < 2; Pass++) {
        int Pen = Left;
        for (const int Which : Name) {
            if (Which < 0) {
                Pen += 3 * Scale;
                continue;
            }
            for (int Row = 0; Row < LetterHigh * Scale; Row++) {
                for (int Col = 0; Col < LetterWide * Scale; Col++) {
                    if (Letters[static_cast<std::size_t>(Which)][static_cast<std::size_t>(Row / Scale)][static_cast<std::size_t>(Col / Scale)] != '#') continue;
                    const int X = Pen + Col, Y = Top + Row;
                    if (Pass == 0) {
                        Out.Cover(X + Scale, Y + Scale, Rgb(0x03, 0x0A, 0x14), 0.7f);
                        continue;
                    }
                    const float Down = static_cast<float>(Row) / static_cast<float>(LetterHigh * Scale);
                    const float Sweep = std::fmod(static_cast<float>(X) * 0.9f - static_cast<float>(Ctx.Now % 4200) * 0.06f + 400.0f, 70.0f);
                    Color Ink = Mixed(Lighter(Gold, 0.35f), Darker(Gold, 0.35f), Down);
                    if (Row % Scale == 0 && Col % Scale == 0) Ink = Lighter(Ink, 0.18f);
                    if (Sweep < 5.0f) Ink = Lighter(Ink, 0.5f * (1.0f - Sweep / 5.0f));
                    Out.Put(X, Y, Ink);
                }
            }
            Pen += (LetterWide + 1) * Scale;
        }
    }
}

void TitlePage::PaintShips(Canvas &Out) const
{
    const float Seconds = static_cast<float>(Ctx.Now % 3600000) * 0.001f;
    const int   Cell = std::clamp(Out.Width / 16, 4, 10);
    for (const Sailing &One : Passing) {
        const float Span = static_cast<float>(Out.Width + 80);
        const float X = std::fmod(Seconds * One.Speed + One.Start, Span) - 50.0f;
        const int   Y = static_cast<int>(One.Row * static_cast<float>(Out.Height)) - Cell;
        for (int Back = 2; Back < 46; Back++) {
            const float Fade = 1.0f - static_cast<float>(Back) / 46.0f;
            const float Spread = static_cast<float>(Back) * 0.1f;
            const float Mid = static_cast<float>(Y) + static_cast<float>(Cell) * 0.5f;
            const int   Stern = static_cast<int>(X) + Cell / 4 - Back;
            Out.Cover(Stern, static_cast<int>(Mid - Spread), Rgb(0xFF, 0xFF, 0xFF), 0.34f * Fade);
            Out.Cover(Stern, static_cast<int>(Mid + Spread), Rgb(0xFF, 0xFF, 0xFF), 0.34f * Fade);
            if (Back % 3 == 0) Out.Cover(Stern, static_cast<int>(Mid), Rgb(0xFF, 0xFF, 0xFF), 0.18f * Fade);
        }
        DrawShip(Out, ShipDraw{.X = static_cast<int>(X), .Y = Y, .Cell = Cell, .Length = One.Length, .Steel = Ctx.Chosen.Ship, .Water = Ctx.Chosen.Sea, .Ms = Ctx.Now, .Plain = Ctx.Text()});
    }
}

void TitlePage::Pixels(Canvas &Out)
{
    PaintSea(Out, SeaPatch{0, 0, Out.Width, Out.Height, Darker(Ctx.Chosen.Sea, 0.22f), 0.65f, 1.0f, Ctx.Text()}, Ctx.Now);
    PaintShips(Out);
    PaintName(Out);
    if (Out.Width >= 120) {
        PaintRadar(Out, 26.0f, static_cast<float>(Out.Height) * 0.55f, 15.0f, Ctx.Chosen.Good, Ctx.Now, 11);
        PaintRadar(Out, static_cast<float>(Out.Width) - 26.0f, static_cast<float>(Out.Height) * 0.55f, 15.0f, Ctx.Chosen.Fire, Ctx.Now + 2100, 29);
    }
}

void TitlePage::Words(tui::Frame &Out)
{
    const Look &Shown = Ctx.Chosen;
    const core::Size Size = Out.Size();
    PaintBar(Out, Shown, "SEA BATTLE", "two captains, one screen");
    PaintHints(Out, Shown, {{"\xE2\x86\x95", "choose", 1}, {"enter", "select", 0, true}, {"esc", "quit", 2}});

    const int Scale = std::clamp((Size.Width - 8) / 56, 1, 3);
    const std::int32_t TaglineRow = (6 + LetterHigh * Scale) / 2 + 2;
    PutCentred(Out, 0, Size.Width, TaglineRow, "the classic battle of the ships: sink the whole fleet before yours is sunk", Shown.Text, Shown.Bars);

    const std::int32_t Wide = 34, High = static_cast<std::int32_t>(Menu.size()) * 2 + 3;
    const core::Rect Area = tui::Centred(Size, Wide, High);
    const core::Rect Inside = PaintPanel(Out, Shown, core::Rect{Area.X, std::max(Area.Y, TaglineRow + 2), Area.Width, High}, "MAIN MENU", "");
    const Color Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
    for (std::size_t At = 0; At < Menu.size(); At++) {
        const std::int32_t Row = Inside.Y + 1 + static_cast<std::int32_t>(At) * 2;
        const bool Chosen = static_cast<int>(At) == Choice;
        const Color Under = Chosen ? Mixed(Paper, Shown.Title, 0.22f) : Paper;
        Out.Fill(core::Rect{Inside.X, Row, Inside.Width, 1}, ' ', {Shown.Text, Under});
        if (Chosen) Out.Put({Inside.X + 1, Row}, tui::Glyphs::Pointer, {Shown.Title, Under});
        PutCentred(Out, Inside.X, Inside.Width, Row, Menu[At].Words, Chosen ? Shown.Title : Shown.Text, Under);
    }
}

}  // namespace

std::unique_ptr<Page> MakeTitle(Context &Shared)
{
    return std::make_unique<TitlePage>(Shared);
}

}  // namespace seabattle
