/* bird.cpp — the bird: a plump teal finch lit from above, a cream breast, a
 * crest of two feathers, a forked tail, a pointed beak, a dark bright eye and
 * a blush under it, and a wing that beats — all in one group that turns as
 * the bird climbs and dives. Its parts are laid in units of its own 34 x 24
 * box, the beak and the tail reaching out of it. */

#include <cmath>

#include "paint.h"
#include "scene.h"

namespace flappybird {

namespace {

using painter::core::PointF;

constexpr float WingSwing = 28;   // degrees up and down from level, at the top and bottom of a beat

}  // namespace

void Scene::MakeBird()
{
    const float U  = Scale;
    const auto  At = [U](float X, float Y) { return PointF{X * U, Y * U}; };
    Flier          = Screen.Group();
    Flier.SetAnchor({field::BirdWide / 2 * U, field::BirdTall / 2 * U});

    painter::core::Path Tail;
    Tail.MoveTo(At(5, 10)).QuadTo(At(-1, 6), At(-6, 4.5f)).QuadTo(At(-3, 9.5f), At(-5, 13)).QuadTo(At(-2, 15.5f), At(-6, 19.5f));
    Tail.QuadTo(At(0, 19), At(6, 16)).Close();
    Keep(Flier.Outline(Tail)).SetFill(paint::Down(4 * U, 20 * U, {{0, ink::Plume}, {1, ink::PlumeDim}})).SetStroke(paint::Edge(1.5f * U));

    painter::core::Path Crest;
    Crest.MoveTo(At(13, 3.5f)).QuadTo(At(10.5f, -1), At(12, -4.5f)).QuadTo(At(14.5f, -0.5f), At(17, 2.5f)).Close();
    Crest.MoveTo(At(16.5f, 2.5f)).QuadTo(At(16.5f, -1.5f), At(19.5f, -3.5f)).QuadTo(At(20, 0), At(20.5f, 3)).Close();
    Keep(Flier.Outline(Crest)).SetFill(paint::Down(-4.5f * U, 3 * U, {{0, ink::PlumeLit}, {1, ink::Plume}})).SetStroke(paint::Edge(1.4f * U));

    Keep(Flier.Ellipse())
        .SetPlace(At(0, 1))
        .SetSize({31 * U, 22 * U})
        .SetFill(paint::Round(At(13, 8), 24 * U, At(10, 4), {{0, ink::PlumeLit}, {0.45f, ink::Plume}, {1, ink::PlumeDim}}))
        .SetStroke(paint::Edge(1.8f * U));
    Keep(Flier.Ellipse())
        .SetPlace(At(12, 10.5f))
        .SetSize({16 * U, 11 * U})
        .SetFill(paint::Down(0, 11 * U, {{0, ink::Breast}, {1, ink::BreastDim}}));

    painter::core::Path Beak;
    Beak.MoveTo(At(28.5f, 8.5f)).QuadTo(At(34, 9.5f), At(38, 12.5f)).LineTo(At(28.5f, 12.5f)).Close();
    Keep(Flier.Outline(Beak)).SetFill(gui::Fill::Of(ink::Beak)).SetStroke(paint::Edge(1.3f * U));
    painter::core::Path Lower;
    Lower.MoveTo(At(28.5f, 12.5f)).LineTo(At(37, 12.5f)).QuadTo(At(33, 14.5f), At(28.5f, 15.5f)).Close();
    Keep(Flier.Outline(Lower)).SetFill(gui::Fill::Of(ink::BeakDim)).SetStroke(paint::Edge(1.3f * U));

    Keep(Flier.Ellipse()).SetPlace(At(22, 12)).SetSize({5.5f * U, 3.2f * U}).SetFill(gui::Fill::Of(ink::Blush)).SetOpacity(0.7f);
    Keep(Flier.Ellipse()).SetPlace(At(20.5f, 5)).SetSize({6 * U, 6.5f * U}).SetFill(gui::Fill::Of(ink::Outline));
    Keep(Flier.Ellipse()).SetPlace(At(23, 6)).SetSize({2.2f * U, 2.2f * U}).SetFill(gui::Fill::Of(ink::White));

    Wing = Flier.Ellipse();
    Wing.SetPlace(At(4, 9))
        .SetSize({16 * U, 9.5f * U})
        .SetAnchor(At(13, 4.75f))
        .SetFill(paint::Down(0, 9.5f * U, {{0, ink::Plume}, {1, ink::PlumeDim}}))
        .SetStroke(paint::Edge(1.5f * U));
}

void Scene::LayBird(const Bird &Of, Stage Now)
{
    Flier.SetPlace({(Of.X - field::BirdWide / 2) * Scale, (Of.Y - field::BirdTall / 2) * Scale}).SetTurn(Of.Tilt);
    /* The wing beats while the bird flies and hangs still once it is struck. */
    const float Beat = Now == Stage::Falling || Now == Stage::Over ? 0.0f : std::sin(Of.Wing * 6.2831853f);
    Wing.SetTurn(-WingSwing * Beat);
}

}  // namespace flappybird
