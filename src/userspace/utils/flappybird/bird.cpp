/* bird.cpp — the bird: a round yellow body lit from above, a wing that
 * beats, a big eye and two orange lips, all in one group that turns as the
 * bird climbs and dives. Its parts are laid in units of its own 34 x 24 box. */

#include <cmath>

#include "paint.h"
#include "scene.h"

namespace flappybird {

namespace {

using painter::core::Corners;

constexpr float WingSwing = 28;   // degrees up and down from level, at the top and bottom of a beat

}  // namespace

void Scene::MakeBird()
{
    const float U = Scale;
    Flier         = Screen.Group();
    Flier.SetAnchor({field::BirdWide / 2 * U, field::BirdTall / 2 * U});

    Keep(Flier.Ellipse())
        .SetSize({field::BirdWide * U, field::BirdTall * U})
        .SetFill(paint::Round({13 * U, 8 * U}, 25 * U, {10 * U, 4 * U}, {{0, ink::FeatherLit}, {0.42f, ink::Feather}, {1, ink::FeatherDim}}))
        .SetStroke(paint::Edge(1.8f * U));
    Wing = Flier.Ellipse();
    Wing.SetPlace({-2 * U, 8 * U})
        .SetSize({17 * U, 11 * U})
        .SetAnchor({15 * U, 5.5f * U})
        .SetFill(paint::Down(0, 11 * U, {{0, ink::Wing}, {1, ink::WingShade}}))
        .SetStroke(paint::Edge(1.6f * U));
    Keep(Flier.Ellipse()).SetPlace({19 * U, 0.5f * U}).SetSize({13 * U, 13 * U}).SetFill(gui::Fill::Of(ink::White)).SetStroke(paint::Edge(1.6f * U));
    Keep(Flier.Rectangle()).SetPlace({26 * U, 4 * U}).SetSize({4 * U, 6.5f * U}).SetCorners(Corners::All(2 * U)).SetFill(gui::Fill::Of(ink::Black));
    Keep(Flier.Rectangle())
        .SetPlace({20 * U, 17.4f * U})
        .SetSize({14 * U, 5 * U})
        .SetCorners(Corners::All(2.5f * U))
        .SetFill(gui::Fill::Of(ink::LipBottom))
        .SetStroke(paint::Edge(1.5f * U));
    Keep(Flier.Rectangle())
        .SetPlace({21 * U, 13 * U})
        .SetSize({16 * U, 5.5f * U})
        .SetCorners(Corners::All(2.7f * U))
        .SetFill(gui::Fill::Of(ink::LipTop))
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
