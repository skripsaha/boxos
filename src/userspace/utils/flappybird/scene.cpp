/* scene.cpp — the sky, the skyline, the pipes and the ground. See scene.h;
 * the bird is bird.cpp's. */

#include "scene.h"

#include <chrono>
#include <cmath>
#include <initializer_list>
#include <new>

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

/* A share of a speed, read slow to fast: the banks of clouds, the city, the
 * trees and the bushes. The small clouds high in the sky stand still. */
constexpr float StillShare = 0, CloudShare = 0.06f, CityShare = 0.16f, BushShare = 0.36f;

/* Below this the bushes' band hides whatever is behind: the clouds and the
 * city need not be drawn lower. */
constexpr float Hidden = 386;

painter::core::Fill Solid(Color Of)
{
    return painter::core::Fill(Of);
}

}  // namespace

Scene::Scene(gui::Area &On, const World &Game, const Sky &Shown)
    : Screen(On), Scale(On.Size().Y / field::Tall), Wide(Game.Wide() * Scale), Pixels(Scale * On.Scale()),
      Apart(std::round(skyline::Period * Pixels))
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

Scene::~Scene()
{
    /* The pictures go before the blocks they show. */
    Skyline.clear();
    for (const gui::PictureBlock &One : Blocks) Screen.CloseBlock(One.Id);
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
    const auto Layer = [&](float Share, std::initializer_list<Piece> Pieces, float Top, float Bottom) {
        Skyline.push_back(Scene::Layer{Painted(Pieces, Top, Bottom), Share, Top * Scale, -1});
    };
    Skyline.reserve(4);

    const skyline::Pattern Small = skyline::Floating(), SmallLit = skyline::Lights(Small, 0.74f);
    Layer(StillShare, {{Small, Solid(Shown.CloudShade)}, {SmallLit, Solid(Shown.Cloud)}}, skyline::Top(Small) - 1, skyline::Bottom(Small) + 1);

    /* The far bank behind the near one, in one picture: what moves every few
     * frames is laid once over, not twice. */
    const skyline::Pattern Far = skyline::FarClouds(), Bank = skyline::Clouds(), BankLit = skyline::Lights(Bank, 0.74f);
    const float            Top  = skyline::Top(Far) - 1;
    const Color            Haze = Color::Transition(Shown.CloudFar, Shown.Horizon, 0.5f);
    Layer(CloudShare,
          {{Far, painter::core::Gradient::Linear({0, (280 - Top) * Pixels}, {0, (330 - Top) * Pixels}, {{0, Shown.CloudFar}, {1, Haze}})},
           {Bank, Solid(Shown.CloudShade)},
           {BankLit, Solid(Shown.Cloud)}},
          Top, Hidden);

    const skyline::Pattern Houses = skyline::City(), Windows = skyline::Windows(Houses);
    Layer(CityShare, {{Houses, Solid(Shown.City)}, {Windows, Solid(Shown.Windows)}}, skyline::Top(Houses) - 1, Hidden);

    /* Clumps: a pattern in its shade, its lights over it, and their lights
     * over them. */
    const skyline::Woods   Trees    = skyline::Trees();
    const skyline::Pattern CrownLit = skyline::Lights(Trees.Crowns, 0.66f), CrownLight = skyline::Lights(CrownLit, 0.5f);
    const skyline::Pattern Bushes = skyline::Bushes(), BushLit = skyline::Lights(Bushes, 0.64f), BushLight = skyline::Lights(BushLit, 0.5f);
    Layer(BushShare,
          {{Trees.Trunks, Solid(Shown.Trunk)}, {Trees.Crowns, Solid(Shown.Crown)}, {CrownLit, Solid(Shown.CrownLit)},
           {CrownLight, Solid(Shown.CrownLight)}, {Bushes, Solid(Shown.Bush)}, {BushLit, Solid(Shown.BushLit)},
           {BushLight, Solid(Shown.BushLight)}},
          std::min(skyline::Top(Trees.Crowns), skyline::Top(Bushes)) - 1, field::Ground);
}

gui::Picture Scene::Painted(std::initializer_list<Piece> Pieces, float Top, float Bottom)
{
    const float         Across = Wide / Scale * Pixels + Apart;   // the screen and a period, in pixels
    const std::uint32_t W      = static_cast<std::uint32_t>(std::ceil(Across)) + 1;
    const std::uint32_t H      = static_cast<std::uint32_t>(std::ceil((Bottom - Top) * Pixels));
    std::optional<painter::core::Canvas>         Page  = painter::core::Canvas::Make(W, H);
    std::expected<gui::PictureBlock, box::error> Block = Screen.OpenPicture(W, H);
    if (!Page || !Block) throw std::bad_alloc();
    const int Count = static_cast<int>(std::ceil(Across / Apart)) + 1;
    for (const Piece &One : Pieces) (void)Page->Fill(skyline::Tiled(One.Of, Pixels, Count, Apart, Top), One.With);
    /* The canvas's light is multiplied by its opacity; a picture's is not. */
    std::span<std::uint8_t> Bytes = Block->Pixels();
    for (std::uint32_t Y = 0; Y < H; Y++) {
        const painter::core::Pixel *Row = Page->Row(Y);
        std::uint8_t              *Out = &Bytes[static_cast<std::size_t>(Y) * W * 4];
        for (std::uint32_t X = 0; X < W; X++, Out += 4) {
            const painter::core::Pixel One = Row[X];
            if (One.Alpha == 0) {
                Out[0] = Out[1] = Out[2] = Out[3] = 0;
                continue;
            }
            const auto Light = [&](std::uint16_t Of) {
                return static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (static_cast<std::uint32_t>(Of) * 255 + One.Alpha / 2) / One.Alpha));
            };
            Out[0] = Light(One.Red), Out[1] = Light(One.Green), Out[2] = Light(One.Blue);
            Out[3] = static_cast<std::uint8_t>((static_cast<std::uint32_t>(One.Alpha) * 255 + 32767) / 65535);
        }
    }
    Screen.BlockChanged(Block->Id, painter::core::Rect{0, 0, static_cast<std::int32_t>(W), static_cast<std::int32_t>(H)});
    gui::Picture Made = Screen.Picture(*Block, ITEM_FIT_STRETCH, true);
    Made.SetPlace({0, Top * Scale}).SetSize({static_cast<float>(W) / Screen.Scale(), static_cast<float>(H) / Screen.Scale()});
    Blocks.push_back(std::move(*Block));
    return Made;
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
    /* A layer moves only when it has come a whole pixel: the display lays it
     * again only then. */
    for (Layer &One : Skyline) {
        const int Shift = static_cast<int>(std::lround(std::fmod(Gone * One.Share * Pixels, Apart)));
        if (Shift == One.Shifted) continue;
        One.Shifted = Shift;
        One.Image.SetPlace({-static_cast<float>(Shift) / Screen.Scale(), One.Top});
    }
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
