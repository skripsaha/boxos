/* scene.cpp — the round's picture and its timeline (scene.h). */

#include "scene.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "backdrop.h"

namespace hangman {

namespace {

namespace tui = painter::tui;

struct PieceTime {
    float Start;
    float Length;
};

constexpr PieceTime Building[PieceCount] = {{0.0f, 0.4f}, {0.35f, 0.5f}, {0.8f, 0.55f}, {1.25f, 0.45f}};
constexpr float PartSeconds = 0.5f;
constexpr float FreedSeconds = 0.7f;
constexpr float CheerAt = 0.8f;
constexpr float BuildingSeconds = 2.0f;
constexpr float EndingSeconds = 6.0f;
constexpr float SettleSeconds = 1.0f;
constexpr int   CheerBursts = 4;
constexpr float CheerGap = 1.1f;

// The cells of From laid over those of To, which are as many.
void CopyCells(const tui::Frame &From, tui::Frame &To)
{
    const painter::core::Size Size = From.Size();
    for (std::int32_t Row = 0; Row < Size.Height; Row++)
        for (std::int32_t Column = 0; Column < Size.Width; Column++) To.Put({Column, Row}, From.Read({Column, Row}));
}

}  // namespace

void Scene::Begin(std::uint64_t Now)
{
    PartAt.fill(0);
    Begun = Now;
    Ended = 0;
    Over = Ending::None;
    Appeared = 0;
    Cheers = 0;
    Flying.Clear();
}

void Scene::Mistake(int Index, std::uint64_t Now)
{
    if (Index < 0 || Index >= PartCount) return;
    PartAt[static_cast<std::size_t>(Index)] = Now;
    Appeared = std::max(Appeared, Index + 1);
    Speed += 1.4f;
}

void Scene::Free(std::uint64_t Now)
{
    Over = Ending::Freed;
    Ended = Now;
}

void Scene::Lose(std::uint64_t Now)
{
    Over = Ending::Lost;
    Ended = Now;
}

float Scene::SecondsSince(std::uint64_t Moment, std::uint64_t Now) const
{
    return Now > Moment ? static_cast<float>(Now - Moment) / 1000.0f : 0.0f;
}

Posture Scene::Posed(std::uint64_t Now) const
{
    Posture Pose;
    const float Age = SecondsSince(Begun, Now);
    for (int Piece = 0; Piece < PieceCount; Piece++)
        Pose.Built[static_cast<std::size_t>(Piece)] = Clamped((Age - Building[Piece].Start) / Building[Piece].Length);
    const float Since = Over == Ending::None ? 0.0f : SecondsSince(Ended, Now);
    for (int Part = 0; Part < PartCount; Part++) {
        const std::uint64_t Moment = PartAt[static_cast<std::size_t>(Part)];
        float Progress = 0.0f;
        if (Moment != 0) Progress = SecondsSince(Moment, Now) / PartSeconds;
        else if (Over == Ending::Freed) Progress = (Since - 0.09f * static_cast<float>(Part - Appeared)) / (PartSeconds * 0.8f);
        Pose.Part[static_cast<std::size_t>(Part)] = Clamped(Progress, 0.0f, 1.0f);
    }
    Pose.Swing = Angle;
    Pose.Seconds = Since;
    if (Over == Ending::Freed) {
        Pose.Fall = Smooth(0.0f, FreedSeconds, Since);
        Pose.Rope = 1.0f - Smooth(0.0f, 0.4f, Since);
        Pose.Cheer = Smooth(FreedSeconds, FreedSeconds + 0.4f, Since);
        Pose.Jump = 5.0f * std::fabs(Wave((Since - FreedSeconds) * 0.8f)) * Pose.Cheer;
        Pose.Face = Pose.Fall > 0.3f ? Mouth::Smile : Mouth::Plain;
    } else if (Over == Ending::Lost || Appeared >= 4) {
        Pose.Face = Mouth::Sad;
    }
    return Pose;
}

void Scene::Cheer(Luck &Chance, float Seconds, const Look &Shown, const View &Where)
{
    if (Cheers >= CheerBursts || Seconds < CheerAt + CheerGap * static_cast<float>(Cheers)) return;
    const Timbers Wood = Timbers::For(Where.Across);
    Flying.Confetti(Chance, Wood.RopeAt - 10.0f, 48.0f, 46);
    Flying.Sparks(Chance, Wood.RopeAt - 10.0f, 44.0f, 14, Shown.Sun);
    Cheers++;
}

void Scene::Step(std::uint32_t ElapsedMs, std::uint64_t Now, Luck &Chance, const Look &Shown, painter::core::Rect Cells)
{
    const float Seconds = static_cast<float>(std::min<std::uint32_t>(ElapsedMs, 120)) / 1000.0f;
    const float Gust = WindAt(Now);
    const bool  Calm = Over == Ending::Lost;
    const float Push = Calm ? 0.15f : 0.9f * Gust * (0.6f + 0.4f * Wave(static_cast<float>(Now % 3100) / 3100.0f));
    Speed += (Push - 20.0f * Angle - (Calm ? 4.5f : 1.7f) * Speed) * Seconds;
    Angle += Speed * Seconds;

    const float DuskTarget = Over == Ending::Lost ? 1.0f : 0.0f;
    const float JoyTarget = Over == Ending::Freed ? 1.0f : 0.0f;
    Dusk += std::clamp(DuskTarget - Dusk, -0.5f * Seconds, 0.5f * Seconds);
    Joy += std::clamp(JoyTarget - Joy, -1.0f * Seconds, 1.0f * Seconds);

    const View Where = View::Fit(Cells);
    if (Over == Ending::Freed) Cheer(Chance, SecondsSince(Ended, Now), Shown, Where);

    LeafMs += ElapsedMs;
    if (LeafMs >= static_cast<std::uint32_t>(1300.0f / Gust)) {
        LeafMs = 0;
        const Timbers Wood = Timbers::For(Where.Across);
        const Tones Tone = TonesFor(Shown, Dusk, Joy);
        Flying.Leaf(Chance, TreeAt + Chance.Between(-6.0f, 8.0f), HillAt(TreeAt, Wood) - CrownUp + Chance.Between(-6.0f, 6.0f),
                    Mixed(Tone.Leaf, Lighter(Tone.Leaf, 0.3f), Chance.Unit()));
    }
    Flying.Step(ElapsedMs, Gust);
}

// The sky and the hill, kept as they were drawn while nothing in them moves, and drawn over again
// only when their colours or their size change.
void Scene::PaintBackground(tui::Frame &Page, const View &Where, const Timbers &Wood, const Tones &Tone, std::uint64_t Now)
{
    const painter::core::Size Size = Page.Size();
    const bool Still = Dusk < StarsFrom;
    if (Still && Backdrop && Backdrop->Size() == Size && Painted == Tone) {
        CopyCells(*Backdrop, Page);
        return;
    }
    PaintBackdrop(Page, Where, Wood, Tone, Dusk, Now);
    if (!Still) return;
    if (!Backdrop || Backdrop->Size() != Size) {
        box::result<tui::Frame> Made = tui::Frame::Create(Size);
        if (!Made) return;
        Backdrop.emplace(std::move(*Made));
    }
    CopyCells(Page, *Backdrop);
    Painted = Tone;
}

bool Scene::Busy(std::uint64_t Now) const
{
    if (SecondsSince(Begun, Now) < BuildingSeconds || (Over != Ending::None && SecondsSince(Ended, Now) < EndingSeconds)) return true;
    for (const std::uint64_t Moment : PartAt)
        if (Moment != 0 && SecondsSince(Moment, Now) < PartSeconds + SettleSeconds) return true;
    return std::fabs(Speed) > 0.2f;
}

void Scene::Paint(tui::Frame &Out, painter::core::Rect Cells, const Look &Shown, std::uint64_t Now)
{
    const painter::core::Size Size{Cells.Width, Cells.Height};
    if (!Canvas || Canvas->Size() != Size) {
        box::result<tui::Frame> Made = tui::Frame::Create(Size);
        if (!Made) return;
        Canvas.emplace(std::move(*Made));
    }
    tui::Frame &Page = *Canvas;
    Page.SetGlass(Out.Glass());
    Page.SetMosaics(Out.Mosaics());
    const View    Where = View::Fit({0, 0, Cells.Width, Cells.Height});
    const Timbers Wood = Timbers::For(Where.Across);
    const Tones   Tone = TonesFor(Shown, Dusk, Joy);
    const Posture Pose = Posed(Now);

    PaintBackground(Page, Where, Wood, Tone, Now);
    tui::FineCanvas Fine(Page);
    PaintSun(Fine, Where, Tone, Dusk, Joy, Now);
    PaintClouds(Fine, Where, Tone, Now);
    PaintBirds(Fine, Where, Tone, Joy, Now);
    PaintTree(Fine, Where, Wood, Tone, Now);
    PaintGrass(Fine, Where, Wood, Tone, Now);
    PaintGallows(Fine, Where, Wood, Tone, Pose);
    PaintMan(Fine, Where, Wood, Tone, Pose);
    Flying.Paint(Fine, Where);

    for (std::int32_t Row = 0; Row < Size.Height; Row++)
        for (std::int32_t Column = 0; Column < Size.Width; Column++)
            Out.Put({Cells.X + Column, Cells.Y + Row}, Page.Read({Column, Row}));
}

}  // namespace hangman
