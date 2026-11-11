/* handover.cpp — the screen passed from one player to the other. What was on
 * the glass darkens to black; then a single panel names the player who is to
 * have it, and nothing of either fleet is anywhere on it. The turn begins when
 * that player presses enter, and not before. */

#include <algorithm>
#include <format>

#include "painter/tui/text.h"
#include "painter/tui/window.h"

#include "hud.h"
#include "keys.h"
#include "pages.h"

namespace seabattle {

namespace {

namespace tui = painter::tui;
namespace core = painter::core;

constexpr std::uint32_t FadeOutMs = 600;      // the picture goes dark
constexpr std::uint32_t FadeInMs = 500;       // the panel comes up
constexpr std::uint32_t DeafMs = 350;         // after it is there, a key still waiting from the last player is not heard
constexpr int           FadeStep = 168;       // of 256 of the light kept by each frame while it darkens
constexpr int           Stars = 70;

class HandoverPage final : public Page {
public:
    explicit HandoverPage(Context &Shared) : Page(Shared), Who(Shared.HandTo()) {}

    void Step(std::uint32_t ElapsedMs) override { Age += ElapsedMs; }
    void Paint(tui::Frame &Out) override;
    void Key(const tui::KeyEvent &Pressed) override;
    std::uint32_t FrameMs() const override { return Age < FadeOutMs + FadeInMs ? 50 : 250; }

protected:
    void Pixels(Canvas &Out) override;
    void Words(tui::Frame &Out) override;

private:
    const Player &Next() const { return Ctx.Game->At(Who); }
    const Player &Other() const { return Ctx.Game->At(1 - Who); }

    int           Who;
    std::uint32_t Age = 0;
    LeavePrompt   Leaving;
};

void HandoverPage::Paint(tui::Frame &Out)
{
    if (Age < FadeOutMs) {
        Darken(Out, FadeStep);
        return;
    }
    Page::Paint(Out);
    if (Age < FadeOutMs + FadeInMs) Darken(Out, static_cast<int>(256 * (Age - FadeOutMs) / FadeInMs));
}

void HandoverPage::Key(const tui::KeyEvent &Pressed)
{
    if (Leaving.Open()) {
        if (Leaving.Answer(Pressed) == LeavePrompt::Verdict::Leave) Ctx.Go(PageId::Title);
        return;
    }
    if (Pressed.IsExtended() || Pressed.IsCtrl() || Pressed.IsAlt() || Age < FadeOutMs + FadeInMs + DeafMs) return;
    if (Pressed.Ascii == tui::KeyCodes::Enter) Ctx.Go(Ctx.Purpose == Errand::PlaceFleet ? PageId::Place : PageId::Battle);
    else if (Pressed.Ascii == tui::KeyCodes::Escape) Leaving.Ask();
}

void HandoverPage::Pixels(Canvas &Out)
{
    Out.Fill(0, 0, Out.Width, Out.Height, Rgb(0x02, 0x05, 0x0A));
    for (int Star = 0; Star < Stars; Star++) {
        const int X = static_cast<int>(Hash(Star, 1) % static_cast<std::uint32_t>(Out.Width)), Y = static_cast<int>(Hash(Star, 2) % static_cast<std::uint32_t>(Out.Height));
        const float Twinkle = Beat(Age, 2600 + Hash(Star, 3) % 3000, Grain(Star, 4));
        Out.Cover(X, Y, Rgb(0x9A, 0xB4, 0xD0), 0.06f + 0.2f * Twinkle * Twinkle);
    }
}

void HandoverPage::Words(tui::Frame &Out)
{
    const Look &Shown = Ctx.Chosen;
    const core::Size Size = Out.Size();
    const core::Rect Inside = PaintPanel(Out, Shown, tui::Centred(Size, 58, 15), "HAND OVER THE SCREEN", "");
    const Color Paper = Mixed(Shown.Bars, Shown.Sea, 0.16f);
    const bool Placing = Ctx.Purpose == Errand::PlaceFleet;

    PutCentred(Out, Inside.X, Inside.Width, Inside.Y + 1, "Pass the screen to", Shown.Quiet, Paper);
    PutCentred(Out, Inside.X, Inside.Width, Inside.Y + 3, "\xE2\x97\x84  " + Next().Name + "  \xE2\x96\xBA", Shown.Title, Paper);
    PutCentred(Out, Inside.X, Inside.Width, Inside.Y + 5, Placing ? "Lay your fleet." : std::format("Your turn to fire: turn {}.", Ctx.Game->TurnNumber()), Shown.Text, Paper);
    PutCentred(Out, Inside.X, Inside.Width, Inside.Y + 7, std::format("{}, look away from the screen!", Other().Name), Shown.Fire, Paper);
    const bool Ready = Age >= FadeOutMs + FadeInMs + DeafMs;
    const Color Glow = Ready && Beat(Age, 1400) > 0.4f ? Shown.Good : Shown.Quiet;
    PutCentred(Out, Inside.X, Inside.Width, Inside.Y + 10, std::format("Press ENTER when {} is ready", Next().Name), Glow, Paper);
    Out.Fill(core::Rect{0, Size.Height - 1, Size.Width, 1}, ' ', {Shown.Quiet, Rgb(0x02, 0x05, 0x0A)});
    PutCentred(Out, 0, Size.Width, Size.Height - 1, "enter  ready      esc  leave the game", Darker(Shown.Quiet, 0.35f), Rgb(0x02, 0x05, 0x0A));
    Leaving.Paint(Out, Shown);
}

}  // namespace

std::unique_ptr<Page> MakeHandover(Context &Shared)
{
    return std::make_unique<HandoverPage>(Shared);
}

}  // namespace seabattle
