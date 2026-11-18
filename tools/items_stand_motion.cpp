/*
 * items_stand_motion.cpp — things in time, held on the host: the curves and
 * the spring, a colour walked as the eye sees it, a property's numbers read
 * and said back, and an area's animations played by a clock the stand turns —
 * from where a property stands to what was said, cut where they stand, one
 * waiting on another, a spring handing on its speed, shown and hidden, one
 * look faded into another, less motion, a text page, and frames only while
 * something moves.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "items_stand.h"
#include "painter/core/easing.h"
#include "painter/items/between.h"

namespace stand {

namespace {

using namespace std::chrono_literals;
using painter::core::Color;
using painter::core::Easing;
using painter::core::Rect;
using painter::gui::Animation;
using painter::gui::Changes;
namespace gui = painter::gui;

constexpr std::uint64_t Ms = 1000;   // a millisecond of the clock's microseconds

/* ---- The curves --------------------------------------------------------- */

void CheckCurves(Tally &Count)
{
    bool Ends = true, Monotone = true;
    for (Easing::Kind Kind : {Easing::Kind::Linear, Easing::Kind::In, Easing::Kind::Out, Easing::Kind::InOut, Easing::Kind::Spring}) {
        Easing Curve;
        Curve.What = Kind;
        Ends       = Ends && Curve.At(0) == 0 && Curve.At(1) == 1;
        float Was  = 0;
        for (int I = 1; I <= 1000; I++) {
            const float Now = Curve.At(static_cast<float>(I) / 1000);
            Monotone        = Monotone && Now >= Was - 1e-6f;
            Was             = Now;
        }
    }
    Easing In, Out, InOut, Steps, Own;
    In.What = Easing::Kind::In, Out.What = Easing::Kind::Out, InOut.What = Easing::Kind::InOut;
    Steps.What = Easing::Kind::Steps, Steps.Steps = 4;
    Own.What = Easing::Kind::Own, Own.X1 = 0.25f, Own.Y1 = 0.1f, Own.X2 = 0.25f, Own.Y2 = 1;
    Count.Check(Ends && Monotone && In.At(0.5f) < 0.5f && Out.At(0.5f) > 0.5f && std::fabs(InOut.At(0.5f) - 0.5f) < 0.02f &&
                    Steps.At(0.3f) == 0.25f && Steps.At(0.99f) == 0.75f && std::fabs(Own.At(0.5f) - 0.8024f) < 0.002f,
                "curves: every one from 0 to 1, none but a spring that bounces going back, slowly at first, slowing at the end, "
                "both, in four jumps, a Bézier of one's own as CSS's ease");

    /* A spring that bounces passes its end by its bounce, and one that does
     * not never does; each settles in its time. */
    bool Bounces = true, Settles = true;
    for (float Bounce : {0.0f, 0.1f, 0.3f, 0.6f}) {
        Easing Spring;
        Spring.What   = Easing::Kind::Spring;
        Spring.Bounce = Bounce;
        float Most    = 0;
        for (int I = 0; I <= 4000; I++) Most = std::max(Most, Spring.At(static_cast<float>(I) / 4000));
        Bounces = Bounces && std::fabs((Most - 1) - Bounce) < (Bounce == 0 ? 1e-3f : 0.02f);
        for (int I = 990; I < 1000; I++) Settles = Settles && std::fabs(Spring.At(static_cast<float>(I) / 1000) - 1) < 2e-3f;
    }
    /* Its speed is how fast its place changes. */
    bool       Speeds = true;
    const auto Swing  = painter::core::Spring::For(0.4f, 0.3f);
    for (float T = 0.01f; T < 0.4f; T += 0.013f) {
        const float H = 1e-4f, Slope = (Swing.At(-100, 250, T + H).Offset - Swing.At(-100, 250, T - H).Offset) / (2 * H);
        Speeds        = Speeds && std::fabs(Slope - Swing.At(-100, 250, T).Speed) < 0.01f * std::fabs(Slope) + 1.0f;
    }
    Count.Check(Bounces && Settles && Speeds,
                "spring: past its end by its bounce (0, 10, 30, 60 of a hundred), settled within a five-hundredth by its time, its "
                "speed how fast its place changes");
}

/* ---- Colour as the eye sees it ------------------------------------------ */

void CheckTransition(Tally &Count)
{
    bool Back = true;
    for (int I = 0; I < 4000; I++) {
        const float R = static_cast<float>(Random() % 256) / 255, G = static_cast<float>(Random() % 256) / 255,
                    B = static_cast<float>(Random() % 256) / 255;
        float R2 = 0, G2 = 0, B2 = 0;
        painter::core::RgbOf(painter::core::OklabOf(R, G, B), R2, G2, B2);
        Back = Back && std::fabs(R - R2) < 0.5f / 255 && std::fabs(G - G2) < 0.5f / 255 && std::fabs(B - B2) < 0.5f / 255;
    }
    const Color Red = Color::Rgb(255, 0, 0), Green = Color::Rgb(0, 255, 0);
    const Color Middle = Color::Transition(Red, Green, 0.5f);
    const auto  L      = [](Color Of) { return painter::core::OklabOf(Of.Red() / 255.0f, Of.Green() / 255.0f, Of.Blue() / 255.0f).L; };
    const Color Faint  = Color::Transition(Color::Through(), Red, 0.5f);
    Count.Check(Back && std::fabs(L(Middle) - (L(Red) + L(Green)) / 2) < 0.01f && Faint.Red() == 255 && Faint.Green() == 0 &&
                    Faint.Blue() == 0 && std::abs(static_cast<int>(Faint.Opacity()) - 128) <= 1 &&
                    Color::Transition(Red, Green, 0) == Red && Color::Transition(Red, Green, 1) == Green,
                "colour: OKLab there and back within half a step, red to green's middle as light as their middle, red out of "
                "see-through red and fainter, never dark");
}

/* ---- A property's numbers ----------------------------------------------- */

void CheckNumbers(Tally &Count)
{
    using namespace painter::items;
    const Theme Look;
    Properties  Of;
    Of.Place   = ItemPoint{1234, -567};
    Of.Size    = ItemPoint{40 * ITEM_POINT, 20 * ITEM_POINT};
    Of.Opacity = 40000;
    Of.Turn    = 3 * ITEM_TURN + 77;
    Of.Scale   = ItemPoint{ITEM_ONE / 2, 2 * ITEM_ONE};
    Of.Corners = std::array<std::int32_t, 4>{1, 2, 3, 4};
    Of.Shape   = painter::items::Shape{ITEM_FORM_PATH, 0, {ITEM_STEP_MOVE, ITEM_STEP_LINE, ITEM_STEP_CLOSE}, {{0, 0}, {512, 256}}};
    Fill Solid, Line;
    Solid.Color = ItemColor{65535, 30000, 1000, 50000};
    Line.Kind   = ITEM_FILL_LINEAR, Line.From = {0, 0}, Line.To = {2560, 0};
    Line.Stops  = {ItemStop{0, 0, ItemColor{0, 0, 65535, 65535}}, ItemStop{65535, 0, ItemColor{65535, 65535, 0, 65535}}};
    Of.Fills    = std::vector<Fill>{Solid, Line};
    Of.Stroke   = Stroke{};
    Of.Stroke->Width = 3 * ITEM_POINT, Of.Stroke->Dashes = {256, 512}, Of.Stroke->With = Solid;
    Of.Shadows  = std::vector<Shadow>{Shadow{{0, 1024}, 4096, 0, ItemColor{0, 0, 0, 20000}, false}};
    Of.Style    = StyleOf(Of, ITEM_ROLE_BODY, Look);
    bool Same = true;
    for (std::uint16_t Property : {ITEM_PROP_PLACE, ITEM_PROP_SIZE, ITEM_PROP_OPACITY, ITEM_PROP_TURN, ITEM_PROP_SCALE,
                                   ITEM_PROP_CORNERS, ITEM_PROP_SHAPE, ITEM_PROP_FILLS, ITEM_PROP_STROKE, ITEM_PROP_SHADOWS,
                                   ITEM_PROP_TEXT_STYLE}) {
        Numbers    First, Again;
        Properties Said;
        const bool Read = NumbersOf(Property, Of, Look, First);
        SayNumbers(Property, First.Values, Of, Look, Said);
        const bool Reread = NumbersOf(Property, Said, Look, Again);
        bool       Near   = Read && Reread && First.Form == Again.Form && First.Values.size() == Again.Values.size();
        for (std::size_t I = 0; Near && I < First.Values.size(); I++) Near = std::fabs(First.Values[I] - Again.Values[I]) < 1e-3;
        if (!Near) std::printf("  numbers of property %u not said back\n", Property);
        Same = Same && Near;
    }
    Properties Bare, Shaded = Of;
    Bare.Shadows = std::vector<Shadow>{};
    MakeAlike(ITEM_PROP_SHADOWS, Bare, Shaded);
    Numbers A, B;
    Count.Check(Same && NumbersOf(ITEM_PROP_SHADOWS, Bare, Look, A) && NumbersOf(ITEM_PROP_SHADOWS, Shaded, Look, B) && A.Form == B.Form &&
                    Bare.Shadows->front().Color.Alpha == 0,
                "numbers: a place, a size, opacity, a turn of three and more, a scale, corners, a path, fills of a colour and a "
                "gradient, a dashed stroke, a shadow and a text's style read and said back to the very numbers; a shadow coming "
                "in from nothing of the other's form, seen through");
}

/* ---- An area's animations ----------------------------------------------- */

struct Scene {
    explicit Scene(const items::Motion &Wanted = items::AsItComes)
        : Program(HostProgram(Look, 1.0f)), Shown(Program, ITEM_RING_BYTES, Wanted)
    {
        Shown.Area.Place = Rect{0, 0, 320, 200};
        Shown.Area.Shown = true;
    }
    items::AreaState *Areas[1] = {nullptr};

    // Everything said stepped at Moment, the animations brought there.
    void At(std::uint64_t Moment)
    {
        Shown.Clock = Moment;
        (void)Shown.Step(Say);
        Shown.Advance(Moment, Look);
    }
    // A frame composed: the rectangles composed again, and their pixels.
    std::uint64_t Frame()
    {
        Areas[0] = &Shown.Area;
        Composer.Take(Areas, Look);
        std::uint64_t Pixels = 0;
        for (const Rect &Each : Composer.Rects()) {
            Screen.Clear(Each, Color::Rgb(20, 22, 30));
            Composer.Compose(Screen, painter::core::Point{}, Each, Areas, Look);
            Pixels += static_cast<std::uint64_t>(Each.Width) * static_cast<std::uint64_t>(Each.Height);
        }
        return Pixels;
    }
    const items::Item &Of(std::uint32_t Id) { return *Shown.Area.Items.Find(Id); }

    items::Theme          Look;
    items::ProgramState   Program;
    Bench                 Shown;
    Changes               Say;
    items::Compositor     Composer{Rect{0, 0, 320, 200}};
    painter::core::Canvas Screen = *painter::core::Canvas::Make(320, 200);
};

/* A plain red rectangle, item 1. */
void Plate(Scene &On, float Opacity = 1)
{
    On.Say.Create(1, gui::Kind::Shape);
    On.Say.Rectangle(1);
    On.Say.Place(1, {10, 10});
    On.Say.Size(1, {40, 20});
    const gui::Fill Red = gui::Fill::Of(Color::Rgb(255, 0, 0));
    On.Say.Fills(1, std::span<const gui::Fill>(&Red, 1));
    On.Say.Opacity(1, Opacity);
    On.At(Ms);
    (void)On.Frame();
}

Animation Numbered(Animation How, std::uint32_t Number, std::uint32_t After = 0)
{
    How.Number = Number;
    How.After  = After;
    return How;
}

void CheckPlayed(Tally &Count)
{
    /* From where it stands to what was said, by the clock; its end told; no
     * more frames once it is still. */
    Scene On;
    Plate(On, 0);
    On.Say.Animate(Numbered(Animation::Linear(200ms), 5), [&] { On.Say.Opacity(1, 1); });
    On.At(1000 * Ms);
    const std::uint16_t Start = *On.Of(1).Said.Opacity;
    const std::uint64_t Frame = On.Shown.Area.Moving.Due(true), Unseen = On.Shown.Area.Moving.Due(false);
    const std::uint64_t Pixels = On.Frame();
    On.Shown.Advance(1100 * Ms, On.Look);
    const std::uint16_t Half = *On.Of(1).Said.Opacity;
    const std::uint64_t Moving = On.Frame();
    On.Shown.Advance(1200 * Ms, On.Look);
    const bool Told = On.Shown.Ends.size() == 1 && On.Shown.Ends[0].Number == 5 && On.Shown.Ends[0].Finished == 1;
    Count.Check(Start == 0 && std::abs(static_cast<int>(Half) - 32768) < 400 && *On.Of(1).Said.Opacity == ITEM_OPAQUE && Told &&
                    Frame == 1000 * Ms + items::FrameMicros && Unseen == 1200 * Ms && On.Shown.Area.Moving.Due(true) == 0 &&
                    Pixels <= 42 * 22 && Moving <= 42 * 22,
                "played: opacity none to whole in 200 ms — none as it begins, half at 100, whole at 200 and its end told; a frame "
                "each sixtieth of a second while it is seen, only its moment of ending while it is not, nothing once it is "
                "still; each frame composes only the plate (" + std::to_string(Moving) + " pixels)");
}

void CheckCut(Tally &Count)
{
    /* Cut by another: from where it stands, no jump. */
    Scene On;
    Plate(On);
    On.Say.Animate(Numbered(Animation::Linear(1000ms), 1), [&] { On.Say.Place(1, {110, 10}); });
    On.At(1000 * Ms);
    On.Shown.Advance(1500 * Ms, On.Look);
    const std::int32_t Midway = On.Of(1).Said.Place->X;
    On.Say.Animate(Animation::Linear(1000ms), [&] { On.Say.Place(1, {10, 10}); });
    On.At(1500 * Ms);
    const std::int32_t Taken = On.Of(1).Said.Place->X;
    On.Shown.Advance(2000 * Ms, On.Look);
    const std::int32_t Back = On.Of(1).Said.Place->X;
    const bool         Cut  = On.Shown.Ends.size() == 1 && On.Shown.Ends[0].Number == 1 && On.Shown.Ends[0].Finished == 0;

    /* Said plainly: at once what was said, its animation cut. */
    On.Say.Place(1, {5, 7});
    On.At(2100 * Ms);
    Count.Check(std::abs(Midway - 60 * ITEM_POINT) < ITEM_POINT && Taken == Midway &&
                    std::abs(Back - (Midway + 10 * ITEM_POINT) / 2) < ITEM_POINT && Cut && On.Of(1).Said.Place->X == 5 * ITEM_POINT &&
                    On.Of(1).Said.Place->Y == 7 * ITEM_POINT && On.Shown.Area.Moving.Count() == 0 && On.Shown.Area.Moving.Due(true) == 0,
                "cut: a move cut halfway by another goes on from where it stood, no jump, the first told cut; said plainly, at once "
                "what was said, nothing moving");
}

void CheckChain(Tally &Count)
{
    /* One after another on one property: the second begins the moment the
     * first ends, from where the first left it. */
    Scene On;
    Plate(On);
    On.Say.Animate(Numbered(Animation::Linear(200ms), 1), [&] { On.Say.Place(1, {100, 10}); });
    On.Say.Animate(Numbered(Animation::Linear(200ms), 2, 1), [&] { On.Say.Place(1, {100, 100}); });
    On.At(1000 * Ms);
    On.Shown.Advance(1100 * Ms, On.Look);
    const ItemPoint First = *On.Of(1).Said.Place;
    On.Shown.Advance(1300 * Ms, On.Look);
    const ItemPoint Second = *On.Of(1).Said.Place;
    On.Shown.Advance(1400 * Ms, On.Look);
    const bool Both = On.Shown.Ends.size() == 2 && On.Shown.Ends[0].Number == 1 && On.Shown.Ends[1].Number == 2;
    Count.Check(std::abs(First.X - 55 * ITEM_POINT) < ITEM_POINT && First.Y == 10 * ITEM_POINT && Second.X == 100 * ITEM_POINT &&
                    std::abs(Second.Y - 55 * ITEM_POINT) < ITEM_POINT && On.Of(1).Said.Place->Y == 100 * ITEM_POINT && Both,
                "chain: right, then down — said in one step on one place, the second waiting on the first's number, begun the very "
                "moment the first ended, from where it left it, each end told in turn");

    /* Delayed, at the person's speed. */
    items::Motion Twice;
    Twice.Speed = 200;
    Scene Fast(Twice);
    Plate(Fast, 0);
    Animation Later = Animation::Linear(200ms);
    Later.Delay     = 100ms;
    Fast.Say.Animate(Later, [&] { Fast.Say.Opacity(1, 1); });
    Fast.At(1000 * Ms);
    Fast.Shown.Advance(1049 * Ms, Fast.Look);
    const std::uint16_t Waiting = Fast.Of(1).Said.Opacity.value_or(0);
    Fast.Shown.Advance(1100 * Ms, Fast.Look);
    const std::uint16_t Half = *Fast.Of(1).Said.Opacity;
    Fast.Shown.Advance(1150 * Ms, Fast.Look);
    Count.Check(Waiting == 0 && std::abs(static_cast<int>(Half) - 32768) < 400 && *Fast.Of(1).Said.Opacity == ITEM_OPAQUE,
                "speed: at twice the speed, 100 ms of delay is 50 and 200 ms of fading is 100");
}

void CheckSpring(Tally &Count)
{
    /* A spring cut by another hands on its speed. */
    Scene On;
    Plate(On);
    On.Say.Animate(Animation::Spring(400ms), [&] { On.Say.Place(1, {210, 10}); });
    On.At(1000 * Ms);
    On.Shown.Advance(1099 * Ms, On.Look);
    const double Before = On.Of(1).Said.Place->X;
    On.Say.Animate(Animation::Spring(400ms), [&] { On.Say.Place(1, {10, 10}); });
    On.At(1100 * Ms);
    const double Now = On.Of(1).Said.Place->X;
    On.Shown.Advance(1101 * Ms, On.Look);
    const double After = On.Of(1).Said.Place->X;
    const double In = Now - Before, Out = After - Now;
    Count.Check(In > 0 && std::fabs(Out - In) < 0.15 * std::fabs(In) + 2,
                "spring: cut while moving on, the spring that takes it from there keeps its speed — " + std::to_string(In / 256) +
                    " points a millisecond before, " + std::to_string(Out / 256) + " after");
}

void CheckSeen(Tally &Count)
{
    /* Hidden in time: seen less and less, hidden only at its end; shown
     * again from nothing. */
    Scene On;
    Plate(On);
    On.Say.Animate(Animation::Linear(200ms), [&] { On.Say.Hidden(1, true); });
    On.At(1000 * Ms);
    On.Shown.Advance(1100 * Ms, On.Look);
    const float Fading = On.Of(1).Seen;
    const bool  Still  = !(On.Of(1).Said.Hidden && *On.Of(1).Said.Hidden);
    On.Shown.Advance(1200 * Ms, On.Look);
    const bool Hidden = On.Of(1).Said.Hidden && *On.Of(1).Said.Hidden && On.Of(1).Seen == 1;
    On.Say.Animate(Animation::Linear(200ms), [&] { On.Say.Hidden(1, false); });
    On.At(1300 * Ms);
    const bool Comes = On.Of(1).Said.Hidden && !*On.Of(1).Said.Hidden && On.Of(1).Seen == 0;
    On.Shown.Advance(1400 * Ms, On.Look);
    Count.Check(std::fabs(Fading - 0.5f) < 0.01f && Still && Hidden && Comes && std::fabs(On.Of(1).Seen - 0.5f) < 0.01f,
                "seen: hidden in 200 ms — half seen at 100, hidden at 200; shown again from nothing, half seen halfway");
}

void CheckFade(Tally &Count)
{
    /* One text fading into another: the two looks laid as one, every pixel
     * between the old look's and the new one's, and the new alone at its end. */
    const auto Picture = [](const char *Words) {
        Scene Only;
        Only.Say.Create(1, gui::Kind::Text, ITEM_AREA, ITEM_TOP, gui::Role::Heading);
        Only.Say.Place(1, {20, 20});
        Only.Say.Text(1, Words);
        Only.At(Ms);
        (void)Only.Frame();
        return std::move(Only.Screen);
    };
    const painter::core::Canvas Old = Picture("12:46"), New = Picture("12:47");
    Scene On;
    On.Say.Create(1, gui::Kind::Text, ITEM_AREA, ITEM_TOP, gui::Role::Heading);
    On.Say.Place(1, {20, 20});
    On.Say.Text(1, "12:46");
    On.At(Ms);
    (void)On.Frame();
    On.Say.Animate(Animation::Linear(200ms), [&] { On.Say.Text(1, "12:47"); });
    On.At(1000 * Ms);
    On.Shown.Advance(1100 * Ms, On.Look);
    const bool          Fading = On.Of(1).Fading != nullptr;
    const std::uint64_t Pixels = On.Frame();
    bool                Between = true, Differs = false;
    for (std::uint32_t Y = 0; Y < 200; Y++)
        for (std::uint32_t X = 0; X < 320; X++) {
            const painter::core::Pixel A = Old.Row(Y)[X], B = New.Row(Y)[X], M = On.Screen.Row(Y)[X];
            const auto Within = [](int Low, int High, int Of) { return Of >= std::min(Low, High) - 2 && Of <= std::max(Low, High) + 2; };
            Between = Between && Within(A.Red, B.Red, M.Red) && Within(A.Green, B.Green, M.Green) && Within(A.Blue, B.Blue, M.Blue);
            Differs = Differs || !(A == B);
        }
    On.Shown.Advance(1200 * Ms, On.Look);
    (void)On.Frame();
    bool Done = On.Of(1).Fading == nullptr;
    for (std::uint32_t Y = 0; Y < 200 && Done; Y++)
        for (std::uint32_t X = 0; X < 320 && Done; X++) Done = On.Screen.Row(Y)[X] == New.Row(Y)[X];
    Count.Check(Fading && Differs && Between && Done && Pixels < 320 * 200 / 4,
                "fade: 12:46 into 12:47 — halfway every pixel between the two looks, only where the text lies composed (" +
                    std::to_string(Pixels) + " pixels), 12:47 alone, pixel for pixel, at its end");
}

void CheckWishes(Tally &Count)
{
    /* Less motion: a move at once, a fade short. */
    items::Motion Less;
    Less.Less = true;
    Scene On(Less);
    Plate(On, 0);
    On.Say.Animate(Animation::Out(1000ms), [&] {
        On.Say.Place(1, {200, 100});
        On.Say.Opacity(1, 1);
    });
    On.At(1000 * Ms);
    const bool Moved = On.Of(1).Said.Place->X == 200 * ITEM_POINT;
    On.Shown.Advance(1200 * Ms, On.Look);
    const bool Faded = *On.Of(1).Said.Opacity == ITEM_OPAQUE && On.Shown.Area.Moving.Count() == 0;

    /* A text page: every change at once, its end told. */
    items::Motion Page;
    Page.Instant = true;
    Scene Text(Page);
    Plate(Text, 0);
    Text.Say.Animate(Numbered(Animation::Linear(500ms), 9), [&] { Text.Say.Opacity(1, 1); });
    Text.At(1000 * Ms);
    Count.Check(Moved && Faded && *Text.Of(1).Said.Opacity == ITEM_OPAQUE && Text.Shown.Ends.size() == 1 && Text.Shown.Ends[0].Number == 9,
                "wishes: less motion moves at once and fades in at most 200 ms; a text page shows every change at once and tells "
                "its end");

    /* Again and again, back and forth: on until it is cut, its item gone. */
    Scene Loop;
    Plate(Loop, 0);
    Animation Pulse = Numbered(Animation::Linear(100ms), 3);
    Pulse.Repeats   = Animation::Forever;
    Pulse.Back      = true;
    Loop.Say.Animate(Pulse, [&] { Loop.Say.Opacity(1, 1); });
    Loop.At(1000 * Ms);
    Loop.Shown.Advance(1150 * Ms, Loop.Look);
    const std::uint16_t Back = *Loop.Of(1).Said.Opacity;
    Loop.Shown.Advance(10'000 * Ms, Loop.Look);
    const bool Still = Loop.Shown.Area.Moving.Count() == 1;
    Loop.Say.Remove(1);
    Loop.At(10'001 * Ms);
    Count.Check(std::abs(static_cast<int>(Back) - 32768) < 400 && Still && Loop.Shown.Ends.size() == 1 && Loop.Shown.Ends[0].Finished == 0 &&
                    Loop.Shown.Area.Moving.Count() == 0,
                "forever: back and forth, halfway back at 150 ms, still on after nine seconds, cut with its item and told so");
}

void CheckTurning(Tally &Count)
{
    /* Turned in time: its kept picture laid through the turn while it moves
     * — near the very text drawn turned — and drawn sharp, pixel for pixel,
     * once still. */
    const auto Heading = [](Scene &On) {
        On.Say.Create(1, gui::Kind::Text, ITEM_AREA, ITEM_TOP, gui::Role::Heading);
        On.Say.Place(1, {110, 80});
        On.Say.Anchor(1, {30, 12});
        On.Say.Text(1, "Turning");
    };
    Scene On;
    Heading(On);
    On.At(Ms);
    (void)On.Frame();
    On.Say.Animate(Animation::Linear(400ms), [&] { On.Say.Turn(1, 90); });
    On.At(1000 * Ms);
    On.Shown.Advance(1200 * Ms, On.Look);
    const bool Laid = On.Of(1).Turning;
    (void)On.Frame();
    Scene Drawn;
    Heading(Drawn);
    Drawn.Say.Turn(1, 45);
    Drawn.At(Ms);
    (void)Drawn.Frame();
    double Off = 0;
    for (std::uint32_t Y = 0; Y < 200; Y++)
        for (std::uint32_t X = 0; X < 320; X++) {
            const painter::core::Pixel A = On.Screen.Row(Y)[X], B = Drawn.Screen.Row(Y)[X];
            Off += std::abs(A.Red - B.Red) + std::abs(A.Green - B.Green) + std::abs(A.Blue - B.Blue);
        }
    Off /= 320.0 * 200.0 * 3.0 * 65535.0;
    On.Shown.Advance(1400 * Ms, On.Look);
    (void)On.Frame();
    painter::core::Canvas Fresh = *painter::core::Canvas::Make(320, 200);
    Fresh.Clear(Color::Rgb(20, 22, 30));
    items::AreaState *const Areas[] = {&On.Shown.Area};
    On.Composer.Compose(Fresh, painter::core::Point{}, Rect{0, 0, 320, 200}, Areas, On.Look);
    bool Sharp = !On.Of(1).Turning;
    for (std::uint32_t Y = 0; Y < 200 && Sharp; Y++)
        for (std::uint32_t X = 0; X < 320 && Sharp; X++) Sharp = On.Screen.Row(Y)[X] == Fresh.Row(Y)[X];
    char Share[32];
    std::snprintf(Share, sizeof Share, "%.4f", Off * 100);
    Count.Check(Laid && Off < 0.002 && Sharp,
                std::string("turning: halfway its kept picture laid through the turn, ") + Share +
                    " of a hundred off the text drawn turned; once still drawn sharp, the very pixels composed whole");
}

}  // namespace

void CheckMotion(Tally &Count)
{
    CheckCurves(Count);
    CheckTransition(Count);
    CheckNumbers(Count);
    CheckPlayed(Count);
    CheckCut(Count);
    CheckChain(Count);
    CheckSpring(Count);
    CheckSeen(Count);
    CheckFade(Count);
    CheckWishes(Count);
    CheckTurning(Count);
}

}  // namespace stand
