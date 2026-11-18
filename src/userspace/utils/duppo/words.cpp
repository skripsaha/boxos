/*
 * words.cpp — now and then Duppo says a word. See words.h.
 */

#include "words.h"

#include <algorithm>
#include <array>

#include "box/clock.h"
#include "box/system.h"
#include "box/time.h"
#include "painter/gui/measure.h"

namespace duppo {

namespace {

using namespace std::chrono_literals;
namespace gui = painter::gui;
using painter::core::PointF;

constexpr PointF        Padding{14, 8};   // round a word, in points
constexpr float         Edge   = 8;       // kept from the glass's edge
constexpr std::uint64_t First  = 1600;    // ms after Duppo comes, its hello
constexpr std::uint64_t Held   = 4200;    // ms a word stays
constexpr float         Seldom = 45, Often = 120;   // seconds between words, at the usual pace

constexpr std::array<std::string_view, 9> Own{
    "blub",        "hi there",          "I'm a bubble with an eye", "don't pop me",     "watching your cells",
    "floating by", "boxes everywhere!", "*blinks*",                 "all good up here?",
};
constexpr std::array<std::string_view, 3> WhenBusy{"busy, busy!", "the cores are warm", "so much going on"};
constexpr std::array<std::string_view, 3> WhenQuiet{"so quiet here", "just floating", "nothing to do... nice"};

template <std::size_t Count> std::string_view OneOf(const std::array<std::string_view, Count> &Of, Random &Dice)
{
    return Of[std::min(Count - 1, static_cast<std::size_t>(Dice.Between(0.0f, static_cast<float>(Count))))];
}

}  // namespace

Words::Words(gui::Area &Screen)
    : Screen(Screen), Bubble(Screen.Group()), Plate(Bubble.Rectangle(gui::Role::Panel)), Line(Bubble.Text("", gui::Role::Body))
{
    Plate.SetCorners(painter::core::Corners::All(12));
    Line.SetPlace(Padding);
    Bubble.SetOpacity(0);
}

void Words::Tune(bool Speaks, float Speed)
{
    Talk = Speaks;
    Pace = std::max(Speed, 0.05f);
}

std::string_view Words::Pick(Random &Dice)
{
    /* How busy the machine was since the last word. */
    system_resources_t Now{};
    float              Share = -1;
    if (system_resources(&Now) == 0) {
        if (Total != 0 && Now.cpu_total_tsc > Total)
            Share = static_cast<float>(Now.cpu_busy_tsc - Busy) / static_cast<float>(Now.cpu_total_tsc - Total);
        Busy = Now.cpu_busy_tsc, Total = Now.cpu_total_tsc;
    }
    if (Share >= 0.75f) return OneOf(WhenBusy, Dice);
    if (Share >= 0 && Share <= 0.05f && Dice.Chance(0.4f)) return OneOf(WhenQuiet, Dice);
    BoxTime Clock{};
    if (Dice.Chance(0.35f) && clock_boxtime_local(&Clock) == OK) {
        if (Clock.hour >= 5 && Clock.hour < 12) return "good morning";
        if (Clock.hour >= 12 && Clock.hour < 18) return "lovely afternoon";
        if (Clock.hour >= 18 && Clock.hour < 23) return "good evening";
        return "it's late. sleep soon?";
    }
    return OneOf(Own, Dice);
}

void Words::Say(std::string_view Word)
{
    const PointF Measured = gui::Measure(Screen, Word, gui::Role::Body);
    Size                  = PointF{Measured.X + 2 * Padding.X, Measured.Y + 2 * Padding.Y};
    if (Saying) {
        /* A word over a word: the old fading into the new, the bubble
         * growing or shrinking to it. */
        Screen.Animate(gui::Animation::InOut(300ms), [&] {
            Line.SetText(Word);
            Plate.SetSize(Size);
        });
        return;
    }
    /* Out of nothing beside Duppo, on a spring from its corner nearest. */
    Line.SetText(Word);
    Plate.SetSize(Size);
    Bubble.SetAnchor({0, Size.Y}).SetScale({0.6f, 0.6f});
    Screen.Animate(gui::Animation::Spring(550ms, 0.35f), [&] { Bubble.SetScale({1, 1}); });
    Screen.Animate(gui::Animation::Out(260ms), [&] { Bubble.SetOpacity(1); });
}

void Words::Step(std::uint64_t Now, PointF Left, PointF Right, Random &Dice, std::chrono::milliseconds Glide)
{
    if (!Greeted) {
        Greeted = true;
        NextAt  = Now + First;
    }
    if (Talk && !Saying && Now >= NextAt) {
        Say(Until == 0 ? std::string_view("hi! I'm Duppo") : Pick(Dice));
        Saying = true;
        Until  = Now + Held;
    }
    if (Saying && (Now >= Until || !Talk)) {
        Screen.Animate(gui::Animation::In(350ms), [&] { Bubble.SetOpacity(0).SetScale({0.92f, 0.92f}); });
        Saying = false;
        NextAt = Now + static_cast<std::uint64_t>(Dice.Between(Seldom, Often) * 1000.0f / Pace);
    }
    /* Beside Duppo's top right, or its top left where the glass ends first. */
    const float Wide = Screen.Size().X;
    PointF      At{Right.X, Right.Y - Size.Y};
    if (At.X + Size.X > Wide - Edge) At.X = Left.X - Size.X;
    At.X = std::clamp(At.X, Edge, std::max(Edge, Wide - Edge - Size.X));
    At.Y = std::max(At.Y, Edge);
    Screen.Animate(gui::Animation::Linear(Glide), [&] { Bubble.SetPlace(At); });
}

}  // namespace duppo
