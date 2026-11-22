/* scene.cpp — the sky, the skyline, the pipes and the ground. See scene.h;
 * the bird is bird.cpp's. */

#include "scene.h"

#include <chrono>
#include <cmath>
#include <initializer_list>

#include "paint.h"
#include "skyline.h"
#include "world.h"

namespace flappybird {

namespace {

using namespace std::chrono_literals;
using painter::core::PointF;

constexpr float StripeEvery = 24;   // the grass's stripes, in units
constexpr float Grass       = 13;   // how deep the grass is
constexpr float Rim         = 2;    // the dark line along the ground's top

/* A share of a speed, read slow to fast: the clouds, the city, the bushes. */
constexpr float CloudShare = 0.06f, CityShare = 0.16f, BushShare = 0.36f;

}  // namespace

Scene::Scene(gui::Area &On, const World &Game, const Sky &Shown)
    : Screen(On), Scale(On.Size().Y / field::Tall), Wide(Game.Wide() * Scale)
{
    MakeSky(Shown);
    MakeSkyline(Shown);
    /* The pipes lie between the skyline and the ground: made as the round
     * needs them (Lay), each in a group of its own under the ground. */
    Under = Screen.Group();
    MakeGround(Shown);
    MakeBird();
    Lay(Game);
}

void Scene::MakeSky(const Sky &Shown)
{
    const float Low = field::Ground * Scale;
    Backdrop        = Screen.Rectangle();
    Backdrop.SetPlace({0, 0}).SetSize({Wide, Low}).SetFill(paint::Down(0, Low * 0.8f, {{0, Shown.Top}, {1, Shown.Horizon}}));
    if (!Shown.Stars) return;
    Stars = Screen.Outline(skyline::Tiled(skyline::Stars(Wide / Scale, field::Ground * 0.62f), Scale, 1));
    Stars.SetFill(gui::Fill::Of(ink::White));
    /* The stars breathe, slowly, the display keeping time. */
    gui::Animation Twinkle = gui::Animation::InOut(2400ms);
    Twinkle.Repeats        = gui::Animation::Forever;
    Twinkle.Back           = true;
    Screen.Animate(Twinkle, [&] { Stars.SetOpacity(0.45f); });
}

void Scene::MakeSkyline(const Sky &Shown)
{
    const int   Count = static_cast<int>(std::ceil(Wide / (skyline::Period * Scale))) + 1;
    const float Low   = field::Ground * Scale;
    const auto  Layer = [&](float Share) -> gui::Group & {
        Skyline.push_back(Scene::Layer{Screen.Group(), Share});
        return Skyline.back().Whole;
    };
    Skyline.reserve(3);

    gui::Group &Clouds = Layer(CloudShare);
    Keep(Clouds.Outline(skyline::Tiled(skyline::Clouds(), Scale, Count)))
        .SetFill(paint::Down(296 * Scale, Low, {{0, Shown.Cloud}, {1, Shown.CloudFoot}}));

    gui::Group            &City   = Layer(CityShare);
    const skyline::Pattern Houses = skyline::City();
    Keep(City.Outline(skyline::Tiled(Houses, Scale, Count))).SetFill(gui::Fill::Of(Shown.City));
    Keep(City.Outline(skyline::Tiled(skyline::Windows(Houses), Scale, Count))).SetFill(gui::Fill::Of(Shown.Windows));

    gui::Group &Bushes = Layer(BushShare);
    Keep(Bushes.Outline(skyline::Tiled(skyline::Bushes(), Scale, Count)))
        .SetFill(paint::Down(366 * Scale, Low, {{0, Shown.Bush}, {1, Shown.BushEdge}}));
}

void Scene::MakeGround(const Sky &Shown)
{
    const float Top  = field::Ground * Scale;
    const float Deep = Screen.Size().Y - Top;
    Ground           = Screen.Group();
    Ground.SetPlace({0, Top});

    const Color Sand = Shown.Sand;
    Keep(Ground.Rectangle())
        .SetPlace({0, (Rim + Grass) * Scale})
        .SetSize({Wide, Deep})
        .SetFill(paint::Down(0, Deep, {{0, Color::Transition(Sand, ink::White, 0.12f)}, {1, Color::Transition(Sand, ink::Outline, 0.12f)}}));
    Keep(Ground.Rectangle()).SetPlace({0, 0}).SetSize({Wide, Rim * Scale}).SetFill(gui::Fill::Of(ink::Outline));

    Band = Ground.Subgroup();
    Band.SetPlace({0, Rim * Scale}).SetSize({Wide, Grass * Scale}).SetClip(true);
    Keep(Band.Rectangle()).SetSize({Wide, Grass * Scale}).SetFill(gui::Fill::Of(ink::Grass));
    /* Slanted stripes, a stripe and a gap every StripeEvery units, one more
     * than the width holds: the group slides them by less than a stripe. */
    painter::core::Path Slants;
    const float         Half = StripeEvery / 2 * Scale, Lean = 7 * Scale, Down = Grass * Scale;
    for (float X = -Lean; X < Wide + StripeEvery * Scale; X += StripeEvery * Scale)
        Slants.MoveTo({X + Lean, 0}).LineTo({X + Lean + Half, 0}).LineTo({X + Half, Down}).LineTo({X, Down}).Close();
    Stripes = Band.Outline(Slants);
    Stripes.SetFill(gui::Fill::Of(ink::GrassLight));
    Keep(Band.Rectangle()).SetSize({Wide, 1.5f * Scale}).SetFill(gui::Fill::Of(ink::GrassLight));
    Keep(Band.Rectangle()).SetPlace({0, Down - 2.5f * Scale}).SetSize({Wide, 2.5f * Scale}).SetFill(gui::Fill::Of(ink::GrassDark));
}

gui::Shape &Scene::Keep(gui::Shape Made)
{
    Still.push_back(std::move(Made));
    return Still.back();
}

void Scene::Lay(const World &Game)
{
    const float Gone = Game.Travelled();
    for (Layer &One : Skyline) One.Whole.SetPlace({-std::fmod(Gone * One.Share, skyline::Period) * Scale, 0});
    Stripes.SetPlace({-std::fmod(Gone, StripeEvery) * Scale, 0});

    const std::vector<Pipe> &Row = Game.Pipes();
    while (Pipes.size() < Row.size()) {
        PipePair Made{Under.Subgroup(), {}, {}, {}, {}};
        Made.Upper    = Made.Whole.Rectangle();
        Made.UpperCap = Made.Whole.Rectangle();
        Made.Lower    = Made.Whole.Rectangle();
        Made.LowerCap = Made.Whole.Rectangle();
        for (gui::Shape *Part : {&Made.Upper, &Made.Lower}) Part->SetStroke(paint::Edge(2.2f * Scale));
        for (gui::Shape *Part : {&Made.UpperCap, &Made.LowerCap})
            Part->SetStroke(paint::Edge(2.2f * Scale)).SetCorners(painter::core::Corners::All(2.5f * Scale));
        Pipes.push_back(std::move(Made));
    }
    for (std::size_t At = 0; At < Pipes.size(); At++) {
        if (At < Row.size()) LayPipe(Pipes[At], Row[At]);
        else Pipes[At].Whole.Hide();
    }
    LayBird(Game.Flier(), Game.Now());
}

void Scene::ShowPipes(const World &Game)
{
    for (std::size_t At = 0; At < Game.Pipes().size() && At < Pipes.size(); At++) {
        Pipes[At].Whole.SetOpacity(0);
        gui::Animation Rise = gui::Animation::Out(420ms);
        Rise.Delay          = std::chrono::milliseconds(70 * static_cast<int>(At));
        Screen.Animate(Rise, [&] { Pipes[At].Whole.SetOpacity(1); });
    }
}

/* A pair at its place: the bodies run off the top and under the ground, and
 * each ends in a cap a little wider than itself at its gap. */
void Scene::LayPipe(PipePair &Pair, const Pipe &Of)
{
    const float Over  = (field::CapWide - field::PipeWide) / 2;
    const float Upper = Of.Middle - field::Gap / 2, Lower = Of.Middle + field::Gap / 2;
    const float Body  = field::PipeWide * Scale, Cap = field::CapWide * Scale;
    Pair.Whole.Show().SetPlace({Of.X * Scale, 0});
    Pair.Upper.SetPlace({0, -8 * Scale}).SetSize({Body, (Upper - field::CapTall + 8) * Scale}).SetFill(paint::Pipe(Body));
    Pair.UpperCap.SetPlace({-Over * Scale, (Upper - field::CapTall) * Scale}).SetSize({Cap, field::CapTall * Scale}).SetFill(paint::Pipe(Cap));
    Pair.LowerCap.SetPlace({-Over * Scale, Lower * Scale}).SetSize({Cap, field::CapTall * Scale}).SetFill(paint::Pipe(Cap));
    Pair.Lower.SetPlace({0, (Lower + field::CapTall) * Scale})
        .SetSize({Body, (field::Ground - Lower - field::CapTall + 4) * Scale})
        .SetFill(paint::Pipe(Body));
}

}  // namespace flappybird
