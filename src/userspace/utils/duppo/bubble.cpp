/*
 * bubble.cpp — the bubble Duppo's words are said in. See bubble.h.
 */

#include "bubble.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace duppo {

namespace {

using painter::core::PointF;

constexpr float Pi = std::numbers::pi_v<float>;

/* An outline written as Duppo on the bubble's left would have it, turned
 * about its middle when Duppo is on its right. */
struct Writer {
    painter::core::Path Made;
    bool                Turned = false;
    float               Wide   = 0;

    PointF At(PointF Of) const { return Turned ? PointF{Wide - Of.X, Of.Y} : Of; }
    void   Move(PointF To) { Made.MoveTo(At(To)); }
    void   Line(PointF To) { Made.LineTo(At(To)); }
    void   Cubic(PointF A, PointF B, PointF To) { Made.CubicTo(At(A), At(B), At(To)); }

    /* An arc of the ellipse about Middle of radii R, from angle From to To —
     * radians, down being a quarter turn — in steps of at most a quarter turn. */
    void Arc(PointF Middle, PointF R, float From, float To)
    {
        const int   Count = std::max(1, static_cast<int>(std::ceil(std::fabs(To - From) / (Pi / 2) - 1e-4f)));
        const float Step  = (To - From) / static_cast<float>(Count);
        const float Pull  = 4.0f / 3.0f * std::tan(Step / 4);
        for (int I = 0; I < Count; I++) {
            const float  A = From + Step * static_cast<float>(I), B = A + Step;
            const PointF P0{Middle.X + R.X * std::cos(A), Middle.Y + R.Y * std::sin(A)};
            const PointF P3{Middle.X + R.X * std::cos(B), Middle.Y + R.Y * std::sin(B)};
            const PointF D0{-R.X * std::sin(A), R.Y * std::cos(A)}, D3{-R.X * std::sin(B), R.Y * std::cos(B)};
            Cubic({P0.X + Pull * D0.X, P0.Y + Pull * D0.Y}, {P3.X - Pull * D3.X, P3.Y - Pull * D3.Y}, P3);
        }
    }

    /* The tail: from where it leaves the bubble, Out, down to its point and
     * back up to where it meets it again, In — its sides bowed in, as a
     * comic's are. */
    void Tail(PointF Out, PointF Tip, PointF In, float Long)
    {
        Cubic({Out.X, Out.Y + Long * 0.35f}, {Tip.X + Long * 0.22f, Tip.Y - Long * 0.18f}, Tip);
        Cubic({Tip.X + Long * 0.42f, Tip.Y - Long * 0.06f}, {In.X - Long * 0.12f, In.Y + Long * 0.22f}, In);
    }
};

PointF OnEllipse(PointF Middle, PointF R, float Angle) { return {Middle.X + R.X * std::cos(Angle), Middle.Y + R.Y * std::sin(Angle)}; }

}  // namespace

BubbleShape BubbleFor(const BubbleLook &Look, PointF Measured, bool TailLeft)
{
    const PointF Pad{Look.Size * 0.9f, Look.Size * 0.5f};
    const float  Edge = std::max(Look.Thickness, 0.0f) / 2 + 1;   // the line kept within the bubble's own
    BubbleShape  Made;
    Writer       Out;

    if (Look.Shape == BubbleLook::Form::Oval) {
        /* An ellipse the words' box lies within, corner to corner, a little
         * room round it; never flatter than a comic draws one. */
        const float  A = Measured.X / 2 * std::numbers::sqrt2_v<float> + Pad.X * 0.5f;
        const float  B = std::max(Measured.Y / 2 * std::numbers::sqrt2_v<float> + Pad.Y * 0.5f, A * 0.42f);
        const PointF Middle{Edge + A, Edge + B}, R{A, B};
        const float  Long = std::max(Look.Size * 0.9f, B * 0.55f);
        Made.Corner = PointF{Middle.X - Measured.X / 2, Middle.Y - Measured.Y / 2};
        Made.Tip    = Look.Tail ? PointF{Middle.X - A * 0.86f, Middle.Y + B + Long * 0.85f} : OnEllipse(Middle, R, 0.75f * Pi);
        Made.Reach  = PointF{2 * (A + Edge), (Look.Tail ? Made.Tip.Y : Middle.Y + B) + Edge};
        Out.Turned  = !TailLeft;
        Out.Wide    = Made.Reach.X;
        if (Look.Tail) {
            const float In = 112.0f / 180.0f * Pi, Away = 140.0f / 180.0f * Pi;
            Out.Move(OnEllipse(Middle, R, In));
            Out.Arc(Middle, R, In, Away - 2 * Pi);
            Out.Tail(OnEllipse(Middle, R, Away), Made.Tip, OnEllipse(Middle, R, In), Long);
        } else {
            Out.Move(OnEllipse(Middle, R, 0));
            Out.Arc(Middle, R, 0, -2 * Pi);
        }
    } else {
        /* A box round the words, its corners rounded — much, or a little. */
        const float W = Measured.X + 2 * Pad.X, H = Measured.Y + 2 * Pad.Y;
        const float R = std::min(H / 2, Look.Shape == BubbleLook::Form::Rounded ? Look.Size : Look.Size * 0.2f);
        const float Long = std::max(Look.Size * 0.9f, H * 0.45f);
        const float Left = Edge, Top = Edge, Right = Edge + W, Bottom = Edge + H;
        const PointF Away{Left + std::max(R, W * 0.12f), Bottom};
        const PointF In{Away.X + std::max(Look.Size * 0.9f, W * 0.14f), Bottom};
        Made.Corner = PointF{Left + Pad.X, Top + Pad.Y};
        Made.Tip    = Look.Tail ? PointF{Away.X - Long * 0.35f, Bottom + Long * 0.85f} : PointF{Left, Bottom};
        Made.Reach  = PointF{W + 2 * Edge, (Look.Tail ? Made.Tip.Y : Bottom) + Edge};
        Out.Turned  = !TailLeft;
        Out.Wide    = Made.Reach.X;
        Out.Move(Look.Tail ? In : PointF{Left + R, Bottom});
        Out.Line({Right - R, Bottom});
        Out.Arc({Right - R, Bottom - R}, {R, R}, Pi / 2, 0);
        Out.Line({Right, Top + R});
        Out.Arc({Right - R, Top + R}, {R, R}, 0, -Pi / 2);
        Out.Line({Left + R, Top});
        Out.Arc({Left + R, Top + R}, {R, R}, -Pi / 2, -Pi);
        Out.Line({Left, Bottom - R});
        Out.Arc({Left + R, Bottom - R}, {R, R}, -Pi, -3 * Pi / 2);
        if (Look.Tail) {
            Out.Line(Away);
            Out.Tail(Away, Made.Tip, In, Long);
        }
    }
    Out.Made.Close();
    Made.Outline = std::move(Out.Made);
    if (!TailLeft) {
        Made.Tip    = PointF{Made.Reach.X - Made.Tip.X, Made.Tip.Y};
        Made.Corner = PointF{Made.Reach.X - Made.Corner.X - Measured.X, Made.Corner.Y};
    }
    return Made;
}

}  // namespace duppo
