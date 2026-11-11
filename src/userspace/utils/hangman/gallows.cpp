/* gallows.cpp — the timber and the man (gallows.h). */

#include "gallows.h"

#include <cmath>

namespace hangman {

namespace {

namespace tui = painter::tui;
using Point = painter::core::PointF;

// Down the rope from the beam to the man's head, and the man's body from the pivot, in the scene's units.
constexpr float RopeLength = 8.0f;
constexpr float HeadRadius = 5.5f;
constexpr float HeadDown = RopeLength + HeadRadius;
constexpr float NeckDown = HeadDown + HeadRadius;
constexpr float ShoulderDown = NeckDown + 3.5f;
constexpr float HipDown = ShoulderDown + 14.5f;
constexpr float FeetDown = HipDown + 16.0f;
constexpr float ArmReach = 8.5f;
constexpr float StageHeight = 6.0f;
constexpr float PostHeight = Timbers::StageTop - Timbers::BeamTop;
constexpr float PostWidth = 4.0f;

// A move that overshoots and settles: a part is put up, not slid in.
float Settled(float Progress)
{
    const float Back = Progress - 1.0f;
    return 1.0f + 2.70158f * Back * Back * Back + 1.70158f * Back * Back;
}

float Grown(float Progress)
{
    return Smooth(0.0f, 1.0f, Progress);
}

Point Along(Point From, Point To, float Part)
{
    return {From.X + (To.X - From.X) * Part, From.Y + (To.Y - From.Y) * Part};
}

void PaintStage(tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, float Progress)
{
    if (Progress <= 0.0f) return;
    const float Middle = (Wood.StageLeft + Wood.StageRight) / 2.0f;
    const float Half = (Wood.StageRight - Wood.StageLeft) / 2.0f * Grown(Progress);
    const Point Corner = Where.At(Middle - Half, Timbers::StageTop);
    Fine.Fill(Corner.X, Corner.Y, Where.Span(Half * 2.0f), Where.Span(StageHeight), Tone.Timber);
    Fine.Fill(Corner.X, Corner.Y, Where.Span(Half * 2.0f), Where.Span(1.6f), Lighter(Tone.Timber, 0.25f));
    const Point Foot = Where.At(Middle - Half, Timbers::StageTop + StageHeight - 1.4f);
    Fine.Fill(Foot.X, Foot.Y, Where.Span(Half * 2.0f), Where.Span(1.4f), Darker(Tone.Timber, 0.35f));
    for (float Seam = Wood.StageLeft + 10.0f; Seam < Wood.StageRight - 2.0f; Seam += 10.0f) {
        if (Seam < Middle - Half || Seam > Middle + Half) continue;
        const Point Cut = Where.At(Seam, Timbers::StageTop + 1.6f);
        Fine.Fill(Cut.X, Cut.Y, Where.Span(0.6f), Where.Span(StageHeight - 3.0f), Darker(Tone.Timber, 0.4f), 0.7f);
    }
}

void PaintPost(tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, float Progress)
{
    if (Progress <= 0.0f) return;
    const float Height = PostHeight * Grown(Progress);
    const Point Top = Where.At(Wood.Post - PostWidth / 2.0f, Timbers::StageTop - Height);
    Fine.Fill(Top.X, Top.Y, Where.Span(PostWidth), Where.Span(Height), Tone.Timber);
    Fine.Fill(Top.X, Top.Y, Where.Span(1.3f), Where.Span(Height), Lighter(Tone.Timber, 0.25f));
    const Point Shade = Where.At(Wood.Post + PostWidth / 2.0f - 1.0f, Timbers::StageTop - Height);
    Fine.Fill(Shade.X, Shade.Y, Where.Span(1.0f), Where.Span(Height), Darker(Tone.Timber, 0.3f));
}

void PaintBeam(tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, float Progress)
{
    if (Progress <= 0.0f) return;
    const float Start = Wood.Post - PostWidth / 2.0f;
    const float Length = (Wood.BeamEnd - Start) * Grown(Progress);
    const float Thick = Timbers::BeamBottom - Timbers::BeamTop;
    const Point Corner = Where.At(Start, Timbers::BeamTop);
    Fine.Fill(Corner.X, Corner.Y, Where.Span(Length), Where.Span(Thick), Tone.Timber);
    Fine.Fill(Corner.X, Corner.Y, Where.Span(Length), Where.Span(1.3f), Lighter(Tone.Timber, 0.25f));
    const Point Under = Where.At(Start, Timbers::BeamBottom - 1.0f);
    Fine.Fill(Under.X, Under.Y, Where.Span(Length), Where.Span(1.0f), Darker(Tone.Timber, 0.3f));
    const float Brace = Clamped((Progress - 0.55f) * 2.2f);
    if (Brace <= 0.0f) return;
    const Point Low = Where.At(Wood.Post + PostWidth / 2.0f, 30.0f);
    const Point High = Where.At(Wood.Post + 15.0f, Timbers::BeamBottom);
    Fine.Line(Low, Along(Low, High, Grown(Brace)), std::max(1.0f, Where.Span(2.4f)), Tone.Timber);
}

// The man's own measures, turned as he swings from the rope and moved as he drops and jumps.
struct Hang {
    const View    &Where;
    const Timbers &Wood;
    const Posture &Now;
    float          Sine;
    float          Cosine;

    Hang(const View &Shown, const Timbers &Timber, const Posture &Pose)
        : Where(Shown), Wood(Timber), Now(Pose), Sine(std::sin(Pose.Swing * (1.0f - Clamped(Pose.Fall)))),
          Cosine(std::cos(Pose.Swing * (1.0f - Clamped(Pose.Fall))))
    {
    }

    // A point at Across from the rope and Down from the beam, on the glass.
    Point At(float Across, float Down) const
    {
        const float Fall = Clamped(Now.Fall);
        const float Shift = -10.0f * Fall;
        const float Lower = (Timbers::StageTop - (Timbers::BeamBottom + FeetDown)) * Fall - Now.Jump;
        const float X = Wood.RopeAt + Across * Cosine + Down * Sine + Shift;
        const float Y = Timbers::BeamBottom - Across * Sine + Down * Cosine + Lower;
        return Where.At(X, Y);
    }
};

void PaintFace(tui::FineCanvas &Fine, const View &Where, const Hang &Hung, const Tones &Tone, Mouth Face)
{
    const Color Ink = Lighter(Tone.Figure, 0.9f);
    const float Eye = std::max(0.5f, Where.Span(0.9f));
    for (const float Side : {-2.1f, 2.1f}) {
        const Point At = Hung.At(Side, HeadDown - 1.0f);
        Fine.Disc(At.X, At.Y, Eye, Ink);
    }
    const float Corner = Face == Mouth::Smile ? -0.9f : (Face == Mouth::Sad ? 0.9f : 0.0f);
    const Point Left = Hung.At(-2.4f, HeadDown + 2.3f + Corner);
    const Point Middle = Hung.At(0.0f, HeadDown + 2.3f - Corner);
    const Point Right = Hung.At(2.4f, HeadDown + 2.3f + Corner);
    const float Thin = std::max(0.5f, Where.Span(0.8f));
    Fine.Line(Left, Middle, Thin, Ink);
    Fine.Line(Middle, Right, Thin, Ink);
}

}  // namespace

Timbers Timbers::For(float Across)
{
    const float Middle = Across * 0.56f;
    return Timbers{.Post = Middle - 16.0f,
                   .StageLeft = Middle - 30.0f,
                   .StageRight = Middle + 30.0f,
                   .BeamEnd = Middle + 22.0f,
                   .RopeAt = Middle + 18.0f,
                   .Pivot = Middle};
}

void PaintGallows(tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, const Posture &Now)
{
    PaintStage(Fine, Where, Wood, Tone, Now.Built[0]);
    PaintPost(Fine, Where, Wood, Tone, Now.Built[1]);
    PaintBeam(Fine, Where, Wood, Tone, Now.Built[2]);
    const float Length = RopeLength * Grown(Now.Built[3]) * Clamped(Now.Rope);
    if (Length <= 0.0f) return;
    const Hang Hung(Where, Wood, Now);
    const Point Top = Where.At(Wood.RopeAt, Timbers::BeamBottom);
    const Point Bottom = Where.At(Wood.RopeAt + Length * Hung.Sine, Timbers::BeamBottom + Length * Hung.Cosine);
    const Color Rope = Mixed(Rgb(204, 176, 116), Tone.Timber, 0.3f);
    Fine.Line(Top, Bottom, std::max(0.8f, Where.Span(1.2f)), Rope);
}

void PaintMan(tui::FineCanvas &Fine, const View &Where, const Timbers &Wood, const Tones &Tone, const Posture &Now)
{
    const Hang  Hung(Where, Wood, Now);
    const float Thick = std::max(1.1f, Where.Span(2.6f));
    const float Wave1 = Wave(Now.Seconds * 1.8f);
    const float Wave2 = Wave(Now.Seconds * 1.8f + 0.5f);

    if (Now.Part[0] > 0.0f) {
        const Point Head = Hung.At(0.0f, HeadDown);
        Fine.Disc(Head.X, Head.Y, Where.Span(HeadRadius * std::max(0.0f, Settled(Now.Part[0]))), Tone.Figure);
        if (Now.Part[0] >= 0.85f) PaintFace(Fine, Where, Hung, Tone, Now.Face);
    }
    if (Now.Part[1] > 0.0f)
        Fine.Line(Hung.At(0.0f, NeckDown - 0.5f), Along(Hung.At(0.0f, NeckDown - 0.5f), Hung.At(0.0f, HipDown), Grown(Now.Part[1])),
                  Thick, Tone.Figure);

    const Point Shoulder = Hung.At(0.0f, ShoulderDown);
    for (int Side = 0; Side < 2; Side++) {
        const float Sign = Side == 0 ? -1.0f : 1.0f;
        const float Sway = Side == 0 ? Wave1 : Wave2;
        const float ReachDown = ShoulderDown + 11.0f;
        const float ReachUp = ShoulderDown - 12.0f;
        const float Lift = Now.Cheer;
        const Point Hand = Hung.At(Sign * (ArmReach + 1.5f * Lift * Sway), ReachDown + (ReachUp - ReachDown) * Lift + 1.8f * Lift * Sway);
        if (Now.Part[2 + Side] > 0.0f) Fine.Line(Shoulder, Along(Shoulder, Hand, Grown(Now.Part[2 + Side])), Thick, Tone.Figure);

        const float Spread = 5.5f + 2.5f * Now.Cheer * std::fabs(Wave(Now.Seconds * 0.9f));
        const Point Hip = Hung.At(0.0f, HipDown);
        const Point Foot = Hung.At(Sign * Spread, FeetDown);
        if (Now.Part[4 + Side] > 0.0f) Fine.Line(Hip, Along(Hip, Foot, Grown(Now.Part[4 + Side])), Thick, Tone.Figure);
    }
}

}  // namespace hangman
