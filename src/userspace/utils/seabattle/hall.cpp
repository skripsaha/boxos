/* hall.cpp — the hall of records: every name that has played, the most wins first. */

#include <algorithm>
#include <format>

#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "hud.h"
#include "pages.h"
#include "sea.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

class HallPage final : public Page {
public:
    explicit HallPage(Context &Shared) : Page(Shared) {}

    std::uint32_t FrameMs() const override { return 200; }

    void Key(const tui::KeyEvent &Pressed) override
    {
        if (!Pressed.IsExtended() && (Pressed.Ascii == tui::KeyCodes::Escape || Pressed.Ascii == tui::KeyCodes::Enter)) Ctx.Go(PageId::Title);
    }

protected:
    void Pixels(Canvas &Out) override { PaintBackdrop(Out, Ctx.Chosen.Sea, Ctx.Text(), Ctx.Now); }

    void Words(tui::Frame &Out) override
    {
        const Look &Shown = Ctx.Chosen;
        const core::Size Size = Out.Size();
        PaintBar(Out, Shown, "SEA BATTLE", "the hall of records");
        PaintHints(Out, Shown, {{"esc", "back", 0, true}});
        const std::span<const Record> Ranked = Ctx.Hall.Ranked();
        const std::int32_t Rows = std::min<std::int32_t>(static_cast<std::int32_t>(Ranked.size()), 14);
        const core::Rect Inside = PaintPanel(Out, Shown, tui::Centred(Size, std::min(Size.Width - 6, 76), std::max(8, Rows + 7)), "HALL OF RECORDS", "");
        const Color Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
        if (Ranked.empty()) {
            PutCentred(Out, Inside.X, Inside.Width, Inside.Y + Inside.Height / 2, "No battle has been fought yet.", Shown.Quiet, Paper);
            return;
        }
        tui::PutText(Out, core::Point{Inside.X + 1, Inside.Y + 1}, std::format("{:>2}  {:<{}}  {:>6}  {:>5}  {:>5}  {:>7}", "#", "name", MostName, "played", "won", "win%", "aim%"), Inside.Width - 2, Shown.Quiet, Paper);
        for (std::int32_t At = 0; At < Rows; At++) {
            const Record &One = Ranked[static_cast<std::size_t>(At)];
            const int Won = One.Games == 0 ? 0 : One.Wins * 100 / One.Games;
            tui::PutText(Out, core::Point{Inside.X + 1, Inside.Y + 3 + At},
                         std::format("{:>2}  {:<{}}  {:>6}  {:>5}  {:>4}%  {:>6}%", At + 1, One.Name, MostName, One.Games, One.Wins, Won, One.AccuracyPercent()),
                         Inside.Width - 2, At == 0 ? Shown.Title : Shown.Text, Paper);
        }
        if (!Ctx.Hall.Trouble().empty())
            tui::PutText(Out, core::Point{Inside.X + 1, Inside.Y + Inside.Height - 1}, Ctx.Hall.Trouble(), Inside.Width - 2, Shown.Bad, Paper);
    }
};

}  // namespace

std::unique_ptr<Page> MakeHall(Context &Shared)
{
    return std::make_unique<HallPage>(Shared);
}

}  // namespace seabattle
