#include "painter/tui/fine_canvas.h"

#include <limits>

namespace painter::tui {

namespace {

float Length(float Across, float Down)
{
    return std::sqrt(Across * Across + Down * Down);
}

/* How far a point is from the segment between two. */
float SegmentDistance(core::PointF At, core::PointF From, core::PointF To)
{
    const float Along = To.X - From.X, Down = To.Y - From.Y;
    const float Span = Along * Along + Down * Down;
    const float Reach = Span > 0.0f ? std::clamp(((At.X - From.X) * Along + (At.Y - From.Y) * Down) / Span, 0.0f, 1.0f) : 0.0f;
    return Length(At.X - From.X - Reach * Along, At.Y - From.Y - Reach * Down);
}

}  // namespace

core::Rect FineCanvas::Reaching(float Left, float Top, float Right, float Bottom) noexcept
{
    const auto Floor = [](float Value) { return static_cast<std::int32_t>(std::floor(Value)); };
    const std::int32_t From = Floor(Left) - 1, Up = Floor(Top / 2.0f) - 1;
    return core::Rect{From, Up, Floor(Right) + 2 - From, Floor(Bottom / 2.0f) + 2 - Up};
}

bool FineCanvas::PutMosaic(core::Point Cell, const Mosaic &Wanted)
{
    return Target->Put(Cell, ResolveMosaic(Wanted, Target->Glass(), Target->Mosaics()));
}

void FineCanvas::Fill(float Left, float Top, float Across, float Down, core::Color Ink, float Opacity)
{
    Rounded(Left, Top, Across, Down, 0.0f, Ink, Opacity);
}

void FineCanvas::Disc(float Across, float Down, float Radius, core::Color Ink, float Opacity)
{
    Cover(Reaching(Across - Radius, Down - Radius, Across + Radius, Down + Radius), Ink,
          [&](float X, float Y) { return Length(X - Across, Y - Down) - Radius; }, Opacity);
}

void FineCanvas::Ellipse(float Across, float Down, float ReachAcross, float ReachDown, core::Color Ink, float Opacity)
{
    const float RadiusAcross = std::max(ReachAcross, 0.25f), RadiusDown = std::max(ReachDown, 0.5f);
    Cover(Reaching(Across - RadiusAcross, Down - RadiusDown, Across + RadiusAcross, Down + RadiusDown), Ink,
          [&](float X, float Y) {
              const float Over = (X - Across) / RadiusAcross, Under = (Y - Down) / RadiusDown;
              const float Far = Length(Over, Under);
              const float Slope = Length(Over / RadiusAcross, Under / RadiusDown);
              return Far < 0.001f ? -std::min(RadiusAcross, RadiusDown) : (Far - 1.0f) * Far / Slope;
          },
          Opacity);
}

void FineCanvas::Ring(float Across, float Down, float Outer, float Inner, core::Color Ink, float Opacity)
{
    Cover(Reaching(Across - Outer, Down - Outer, Across + Outer, Down + Outer), Ink,
          [&](float X, float Y) {
              const float Far = Length(X - Across, Y - Down);
              return std::max(Far - Outer, Inner - Far);
          },
          Opacity);
}

void FineCanvas::Rounded(float Left, float Top, float Across, float Down, float Radius, core::Color Ink, float Opacity)
{
    const float HalfAcross = Across / 2.0f, HalfDown = Down / 2.0f;
    const float MiddleAcross = Left + HalfAcross, MiddleDown = Top + HalfDown;
    Cover(Reaching(Left, Top, Left + Across, Top + Down), Ink,
          [&](float X, float Y) {
              const float OverAcross = std::fabs(X - MiddleAcross) - (HalfAcross - Radius);
              const float OverDown   = std::fabs(Y - MiddleDown) - (HalfDown - Radius);
              return Length(std::max(OverAcross, 0.0f), std::max(OverDown, 0.0f)) + std::min(std::max(OverAcross, OverDown), 0.0f) - Radius;
          },
          Opacity);
}

void FineCanvas::Line(core::PointF From, core::PointF To, float Thick, core::Color Ink, float Opacity)
{
    const float Half = Thick / 2.0f;
    Cover(Reaching(std::min(From.X, To.X) - Half, std::min(From.Y, To.Y) - Half, std::max(From.X, To.X) + Half,
                   std::max(From.Y, To.Y) + Half),
          Ink, [&](float X, float Y) { return SegmentDistance({X, Y}, From, To) - Half; }, Opacity);
}

void FineCanvas::Polygon(std::span<const core::PointF> Corners, core::Color Ink, float Opacity)
{
    if (Corners.size() < 3) return;
    core::PointF Low = Corners[0], High = Corners[0];
    for (const core::PointF Corner : Corners) {
        Low  = {std::min(Low.X, Corner.X), std::min(Low.Y, Corner.Y)};
        High = {std::max(High.X, Corner.X), std::max(High.Y, Corner.Y)};
    }
    Cover(Reaching(Low.X, Low.Y, High.X, High.Y), Ink,
          [&](float X, float Y) {
              float        Near = std::numeric_limits<float>::max();
              bool         Within = false;
              core::PointF Last = Corners.back();
              for (const core::PointF Corner : Corners) {
                  Near = std::min(Near, SegmentDistance({X, Y}, Last, Corner));
                  if ((Corner.Y > Y) != (Last.Y > Y) && X < (Last.X - Corner.X) * (Y - Corner.Y) / (Last.Y - Corner.Y) + Corner.X) Within = !Within;
                  Last = Corner;
              }
              return Within ? -Near : Near;
          },
          Opacity);
}

}  // namespace painter::tui
