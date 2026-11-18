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

constexpr float         Edge   = 8;       // kept from the glass's edge, in points
constexpr float         Near   = 4;       // between the tail's point and Duppo
constexpr std::uint64_t First  = 1600;    // ms after Duppo comes, its hello
constexpr std::uint64_t Held   = 4200;    // ms a word stays
constexpr float         Soonest = 45, Latest = 120;   // seconds between words, as often as usual

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

gui::TextStyle WordsStyle(const BubbleLook &Look)
{
    gui::TextStyle Made;
    Made.Size   = Look.Size;
    Made.Weight = 500;
    return Made;
}

}  // namespace

Words::Words(gui::Area &Screen) : Screen(Screen), Bubble(Screen.Group()), Paper(Bubble.Rectangle()), Line(Bubble.Text(""))
{
    Line.SetBox(gui::TextBox{ITEM_TEXT_CENTER, ITEM_OVERFLOW_GROW, false, 0});
    Bubble.SetOpacity(0);
}

void Words::Tune(bool Speaks, float HowOften, const BubbleLook &Wanted)
{
    Talk  = Speaks;
    Often = std::max(HowOften, 0.05f);
    Look  = Wanted;
    /* A look set anew while a word is in the bubble: taken in time. */
    if (Saying) Screen.Animate(gui::Animation::InOut(400ms), [&] { Dress(); });
    else Dress();
}

/* The bubble as the look says: its paper, its line, its shadow, the words'
 * colour and size — and its shape round the word in it. */
void Words::Dress()
{
    Paper.SetFill(gui::Fill::Of(Look.Paper));
    gui::Stroke Round;
    Round.Width = std::max(Look.Thickness, 0.0f);
    Round.Join  = ITEM_JOIN_ROUND;
    Round.With  = gui::Fill::Of(Look.Line);
    Paper.SetStroke(Round);
    if (Look.Shadow) {
        gui::Shadow Soft;
        Soft.Offset = PointF{0, 3};
        Soft.Blur   = 12;
        Soft.Color  = painter::core::Color::Rgba(0, 0, 0, 70);
        Paper.SetShadow(Soft);
    } else {
        Paper.SetShadows({});
    }
    Line.SetFill(gui::Fill::Of(Look.Ink)).SetStyle(WordsStyle(Look));
    if (!Said.empty()) {
        Measured = gui::Measure(Screen, Said, gui::Role::Body, gui::ByContent, WordsStyle(Look));
        Shape();
    }
}

/* Its outline round the word, its tail to Duppo's side, the words in their
 * box, and its anchor at the tail's point, where it springs out of. */
void Words::Shape()
{
    Shaped = BubbleFor(Look, Measured, TailLeft);
    Paper.SetOutline(Shaped.Outline);
    Line.SetPlace(Shaped.Corner).SetSize(Measured);
    Bubble.SetAnchor(Shaped.Tip);
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
    const bool Over = Saying;
    Said     = Word;
    Measured = gui::Measure(Screen, Said, gui::Role::Body, gui::ByContent, WordsStyle(Look));
    if (Over) {
        /* A word over a word: the old fading into the new, the bubble
         * growing or shrinking to it. */
        Screen.Animate(gui::Animation::InOut(320ms), [&] {
            Line.SetText(Said);
            Shape();
        });
        return;
    }
    /* Out of its tail, on a spring. */
    Line.SetText(Said);
    Shape();
    Bubble.SetScale({0.3f, 0.3f});
    Screen.Animate(gui::Animation::Spring(600ms, 0.35f), [&] { Bubble.SetScale({1, 1}); });
    Screen.Animate(gui::Animation::Out(220ms), [&] { Bubble.SetOpacity(1); });
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
        Screen.Animate(gui::Animation::In(320ms), [&] { Bubble.SetOpacity(0).SetScale({0.85f, 0.85f}); });
        Saying = false;
        NextAt = Now + static_cast<std::uint64_t>(Dice.Between(Soonest, Latest) * 1000.0f / Often);
    }
    if (Said.empty()) return;

    /* Its tail's point beside Duppo's top right — or its top left, the
     * bubble turned, where the glass ends first; turned back only once there
     * is room to spare, so it does not toss from side to side at the edge. */
    const float Wide   = Screen.Size().X;
    const float Beyond = Shaped.Reach.X - (TailLeft ? Shaped.Tip.X : Shaped.Reach.X - Shaped.Tip.X);   // past its tail
    const float Spare  = TailLeft ? 0.0f : 24.0f;
    const bool  RoomRight = Right.X + Near + Beyond + Spare <= Wide - Edge;
    const bool  RoomLeft  = Left.X - Near - Beyond >= Edge;
    const bool  WantLeft  = RoomRight || !RoomLeft;
    if (WantLeft != TailLeft) {
        TailLeft = WantLeft;
        Screen.Animate(gui::Animation::InOut(300ms), [&] { Shape(); });
    }
    const PointF Beside = TailLeft ? PointF{Right.X + Near, Right.Y} : PointF{Left.X - Near, Left.Y};
    PointF       At{Beside.X - Shaped.Tip.X, Beside.Y - Shaped.Tip.Y};
    At.X = std::clamp(At.X, Edge, std::max(Edge, Wide - Edge - Shaped.Reach.X));
    At.Y = std::max(At.Y, Edge);
    Screen.Animate(gui::Animation::Linear(Glide), [&] { Bubble.SetPlace(At); });
}

}  // namespace duppo
