// paint.h — the picture of the sky in half-block pixels: what each body drew
// behind it, the bodies, the flares of touches and the aim. It knows the sky
// and the look and nothing of keys; show.cpp says when to draw.
//
// The trails are a glow kept apart from the bodies: every pixel a body has
// passed holds the brightest colour laid on it, and the whole fades together,
// so a trail is a comet's tail whatever the speed of time.
#ifndef ORBITS_PAINT_H
#define ORBITS_PAINT_H

#include <vector>

#include "painter/core/geometry.h"
#include "painter/tui/frame.h"

#include "look.h"
#include "sky.h"

namespace orbits {

// What the next throw shows: where it starts, which way, and where it would go.
struct Aiming {
    bool             Shown = false;
    Vec              Launcher;
    double           Angle = 0.0;
    std::vector<Vec> Places;
    bool             Ends = false;
};

class Canvas {
public:
    // Made for a glass of Cells; the trails are cleared.
    void Fit(painter::core::Size Cells);
    // The view To times as near as at the start; the trails are cleared, they were laid for the old one.
    void Zoom(double To);
    double Zoomed() const { return ZoomFactor; }
    void Clear();
    // Time has passed: the trails fade, the flares age.
    void Age(double Seconds, const Look &Shown);
    // The trail of a body from where it was last drawn to where it is.
    void Trail(Body &One, const Look &Shown);
    void Flare(const Collision &Hit, const Look &Shown);
    void Paint(painter::tui::Frame &Out, const Sky &Bodies, const Look &Shown, const Aiming &Aim);

private:
    struct Glow {
        float Red = 0, Green = 0, Blue = 0;
    };
    struct Burst {
        Vec    At;
        double Age  = 0.0;
        double Size = 0.0;
        Color  Ink;
    };
    struct Spot {
        double X, Y;
    };

    Spot Where(Vec At) const;
    void Lay(int X, int Y, Color Ink, double Strength);
    void Blend(int X, int Y, Color Ink, double Weight);
    void Disc(Spot At, double Radius, Color Ink, double Weight);
    void Halo(Spot At, double Inner, double Outer, Color Ink, double Strength);
    void Background(const Look &Shown);
    void PaintStar(const Body &One, const Look &Shown);
    void PaintBody(const Body &One, const Look &Shown);
    void PaintBursts();
    void PaintAim(const Aiming &Aim, const Look &Shown);

    int                Wide = 0;
    int                High = 0;
    double             Scale = 1.0;
    double             Reach = 1.0;   // how far the glows reach, by how many pixels the glass has
    double             ZoomFactor = 1.0;
    double             Time = 0.0;
    std::vector<Glow>  Glows;
    std::vector<Color> Pixels;
    std::vector<Burst> Bursts;
};

}  // namespace orbits

#endif  // ORBITS_PAINT_H
