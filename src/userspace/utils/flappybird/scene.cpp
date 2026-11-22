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

/* A share of a speed, read slow to fast: the clouds alone in the sky, the
 * far bank, the near one, the city, the trees and the bushes. */
constexpr float FloatShare = 0.03f, FarShare = 0.045f, CloudShare = 0.07f, CityShare = 0.16f, BushShare = 0.36f;

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
    Stars = Screen.Outline(skyline::Placed(skyline::Stars(Wide / Scale, field::Ground * 0.62f), Scale));
    Stars.SetFill(gui::Fill::Of(ink::White));
    /* The stars breathe, slowly, the display keeping time. */
    gui::Animation Twinkle = gui::Animation::InOut(2400ms);
    Twinkle.Repeats        = gui::Animation::Forever;
    Twinkle.Back           = true;
    Screen.Animate(Twinkle, [&] { Stars.SetOpacity(0.45f); });
}

void Scene::MakeSkyline(const Sky &Shown)
{
    const int  Count = static_cast<int>(std::ceil(Wide / (skyline::Period * Scale))) + 1;
    const auto Layer = [&](float Share) -> gui::Group & {
        Skyline.push_back(Scene::Layer{Screen.Group(), Share});
        return Skyline.back().Whole;
    };
    const auto Paint = [&](gui::Group &Into, const skyline::Pattern &Of, gui::Fill With) {
        Keep(Into.Outline(skyline::Tiled(Of, Scale, Count))).SetFill(std::move(With));
    };
    /* Clumps: the pattern in its shade, its lights over it, and their lights
     * over them. */
    const auto Clumps = [&](gui::Group &Into, const skyline::Pattern &Of, float Share, Color Shade, Color Lit, Color Light) {
        const skyline::Pattern Lights = skyline::Lights(Of, Share);
        Paint(Into, Of, gui::Fill::Of(Shade));
        Paint(Into, Lights, gui::Fill::Of(Lit));
        Paint(Into, skyline::Lights(Lights, 0.5f), gui::Fill::Of(Light));
    };
    Skyline.reserve(5);

    gui::Group            &High  = Layer(FloatShare);
    const skyline::Pattern Small = skyline::Floating();
    Paint(High, Small, gui::Fill::Of(Shown.CloudShade));
    Paint(High, skyline::Lights(Small, 0.74f), gui::Fill::Of(Shown.Cloud));

    Paint(Layer(FarShare), skyline::FarClouds(),
          paint::Down(280 * Scale, 330 * Scale, {{0, Shown.CloudFar}, {1, Color::Transition(Shown.CloudFar, Shown.Horizon, 0.5f)}}));

    gui::Group            &Near = Layer(CloudShare);
    const skyline::Pattern Bank = skyline::Clouds();
    Paint(Near, Bank, gui::Fill::Of(Shown.CloudShade));
    Paint(Near, skyline::Lights(Bank, 0.74f), gui::Fill::Of(Shown.Cloud));

    gui::Group            &City   = Layer(CityShare);
    const skyline::Pattern Houses = skyline::City();
    Paint(City, Houses, gui::Fill::Of(Shown.City));
    Paint(City, skyline::Windows(Houses), gui::Fill::Of(Shown.Windows));

    gui::Group          &Green = Layer(BushShare);
    const skyline::Woods Trees = skyline::Trees();
    Paint(Green, Trees.Trunks, gui::Fill::Of(Shown.Trunk));
    Clumps(Green, Trees.Crowns, 0.66f, Shown.Crown, Shown.CrownLit, Shown.CrownLight);
    Clumps(Green, skyline::Bushes(), 0.64f, Shown.Bush, Shown.BushLit, Shown.BushLight);
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
