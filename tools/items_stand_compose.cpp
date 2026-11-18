/* items stand — the compositor: a scene of every kind of item — a page, a
 * panel raised by its shadow, texts of styles and coloured stretches, a
 * gradient, a group that clips, a group seen through, a dashed outline, a
 * picture, an item hidden, a text sized by its content — in two areas of two
 * programs, changed at random step after step; after each step the screen
 * composed only where it changed holds the very pixels of the screen composed
 * whole from nothing. And: one word changed composes about that word alone;
 * an opaque item hides what lies beneath; a text sized by its content says
 * its size; what is kept is kept and what is plain is drawn straight. */

#include <cmath>
#include <cstdio>
#include <optional>
#include <string>

#include "boxos_pixel.h"
#include "items_stand.h"

namespace stand {

namespace {

using painter::core::Color;
using painter::core::PointF;
using painter::core::Rect;
using painter::gui::Changes;
using painter::gui::Fill;
using painter::gui::Kind;
using painter::gui::Role;

const Color Beneath = Color::Rgb(24, 26, 40);

std::shared_ptr<const painter::core::Picture> Checks()
{
    static const std::shared_ptr<const painter::core::Picture> Made = [] {
        std::vector<std::uint8_t> Rgba(32u * 32u * 4u);
        for (std::uint32_t Y = 0; Y < 32; Y++)
            for (std::uint32_t X = 0; X < 32; X++) {
                std::uint8_t *P = &Rgba[(Y * 32 + X) * 4];
                const bool    On = ((X / 4) + (Y / 4)) % 2;
                P[0] = On ? 250 : 40, P[1] = static_cast<std::uint8_t>(X * 8), P[2] = static_cast<std::uint8_t>(Y * 8), P[3] = 255;
            }
        return painter::core::Picture::FromRgba(Rgba, 32, 32, 128);
    }();
    return Made;
}

/* A scene of every kind of item, in an area W x H points. */
void Scene(Changes &Say, float W, float H)
{
    Say.Create(1, Kind::Shape, ITEM_AREA, ITEM_TOP, Role::Page);
    Say.Size(1, {W, H});
    Say.Create(10, Kind::Group);
    Say.Place(10, {16, 16});
    Say.Size(10, {W * 0.55f, H * 0.6f});
    Say.Corners(10, painter::core::Corners::All(14));
    Say.Clip(10, true);
    Say.Create(2, Kind::Shape, 10, ITEM_TOP, Role::Panel);
    Say.Size(2, {W * 0.55f, H * 0.6f});
    Say.Create(3, Kind::Text, 10, ITEM_TOP, Role::Heading);
    Say.Place(3, {14, 10});
    Say.Text(3, "Items, composed");
    Say.Create(4, Kind::Text, 10, ITEM_TOP, Role::Body);
    Say.Place(4, {14, 44});
    Say.Size(4, {W * 0.5f, painter::gui::ByContent});
    Say.Text(4, "What a program says is kept and drawn only where it changed \xE2\x80\x94 \xD0\xBA\xD0\xB0\xD0\xBA "
                "\xD1\x83\xD0\xB3\xD0\xBE\xD0\xB4\xD0\xBD\xD0\xBE, of any size.");
    const painter::gui::Span Colored{4, 11, painter::gui::TextStyle{.Weight = 700, .Color = Color::Rgb(255, 120, 80)}};
    Say.Spans(4, std::span(&Colored, 1));
    Say.Create(5, Kind::Shape);
    Say.Ellipse(5);
    Say.Place(5, {W * 0.62f, 24});
    Say.Size(5, {90, 70});
    Fill Round;
    Round.What   = Fill::Kind::Radial;
    Round.From   = {45, 35};
    Round.Radius = 50;
    Round.Focus  = {35, 25};
    Round.Stops  = {{0, Color::Rgb(255, 240, 200)}, {1, Color::Rgb(120, 60, 180)}};
    Say.Fills(5, std::span(&Round, 1));
    Say.Create(6, Kind::Group);
    Say.Place(6, {W * 0.62f, H * 0.42f});
    Say.Opacity(6, 0.6f);
    Say.Create(7, Kind::Shape, 6);
    Say.Size(7, {70, 50});
    const Fill Blue = Fill::Of(Color::Rgb(60, 140, 255));
    Say.Fills(7, std::span(&Blue, 1));
    Say.Create(8, Kind::Shape, 6);
    Say.Place(8, {35, 25});
    Say.Size(8, {70, 50});
    const Fill Gold = Fill::Of(Color::Rgb(255, 200, 60));
    Say.Fills(8, std::span(&Gold, 1));
    Say.Corners(8, painter::core::Corners::All(10));
    Say.Create(9, Kind::Shape);
    Say.Place(9, {W * 0.1f, H * 0.7f});
    Say.Outline(9, *painter::core::Path::FromSvg("M 30 0 L 37 22 L 60 22 L 41 36 L 48 58 L 30 44 L 12 58 L 19 36 L 0 22 L 23 22 Z"));
    painter::gui::Stroke Line;
    Line.Width  = 2.5f;
    Line.Dashes = {5, 3};
    Line.Join   = ITEM_JOIN_ROUND;
    Line.With   = Fill::OfTheme(painter::gui::ThemeColor::Accent);
    Say.Stroke(9, Line);
    Say.Size(9, {60, 60});
    Say.Create(11, Kind::Picture);
    Say.Place(11, {W * 0.3f, H * 0.7f});
    Say.Size(11, {64, 48});
    Say.Picture(11, 1, ITEM_FIT_CONTAIN, true);
    Say.Create(12, Kind::Text);
    Say.Text(12, "never seen");
    Say.Hidden(12, true);
    Say.Create(13, Kind::Text, ITEM_AREA, ITEM_TOP, Role::Label);
    Say.Place(13, {W * 0.55f, H * 0.82f});
    Say.Text(13, "12:46");
}

std::uint64_t Differ(const painter::core::Canvas &A, const painter::core::Canvas &B)
{
    std::uint64_t Count = 0;
    for (std::uint32_t Y = 0; Y < A.Height(); Y++)
        for (std::uint32_t X = 0; X < A.Width(); X++) Count += !(A.Row(Y)[X] == B.Row(Y)[X]);
    return Count;
}

struct Screenful {
    Screenful(std::uint32_t W, std::uint32_t H)
        : Screen(*painter::core::Canvas::Make(W, H)), Whole(Rect{0, 0, static_cast<std::int32_t>(W), static_cast<std::int32_t>(H)}),
          Composer(Whole)
    {
        Screen.Clear(Beneath);   /* what lies beneath every area, as the console's cells would */
    }
    painter::core::Canvas Screen;
    Rect                  Whole;
    items::Compositor     Composer;
    std::uint64_t         Composed = 0;   /* pixels composed, rectangle by rectangle */

    /* The marks taken, and only the damage composed. */
    void Frame(std::span<items::AreaState *const> Areas, const items::Theme &Look)
    {
        Composer.Take(Areas, Look);
        for (const Rect &Each : Composer.Rects()) {
            Screen.Clear(Each, Beneath);
            Composer.Compose(Screen, Each, Areas, Look);
            Composed += static_cast<std::uint64_t>(Each.Width) * static_cast<std::uint64_t>(Each.Height);
        }
    }
    /* The screen composed whole from nothing, as it should be. */
    painter::core::Canvas Fresh(std::span<items::AreaState *const> Areas, const items::Theme &Look)
    {
        painter::core::Canvas Made = *painter::core::Canvas::Make(Screen.Width(), Screen.Height());
        Made.Clear(Beneath);
        Composer.Compose(Made, Whole, Areas, Look);
        return Made;
    }
};

/* One random change of the scene. */
void Change(Changes &Say, float W, float H, int &Made)
{
    const std::uint32_t Items[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13};
    const std::uint32_t Some    = Items[Random() % (sizeof Items / sizeof Items[0])];
    switch (Random() % 11) {
    case 0: Say.Place(Some, {static_cast<float>(Random() % static_cast<std::uint32_t>(W * 0.8f)), static_cast<float>(Random() % static_cast<std::uint32_t>(H * 0.8f))}); break;
    case 1: Say.Text(4, std::string("Changed ") + std::to_string(Random() % 1000) + " \xE2\x80\x94 \xD1\x81\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB0 and again, long enough to wrap twice or more."); break;
    case 2: Say.Text(13, std::to_string(Random() % 24) + ":" + std::to_string(10 + Random() % 50)); break;
    case 3: Say.Hidden(Some, Random() % 3 == 0); break;
    case 4: Say.Opacity(Some, static_cast<float>(Random() % 101) / 100.0f); break;
    case 5: Say.Restack(Some, Some == 7 || Some == 8 ? 6 : Some == 2 || Some == 3 || Some == 4 ? 10 : ITEM_AREA, ITEM_TOP); break;
    case 6:
        if (Made) {
            Say.Remove(50);
            Made = 0;
        } else {
            Say.Create(50, Kind::Shape);
            Say.Place(50, {static_cast<float>(Random() % 200), static_cast<float>(Random() % 150)});
            Say.Size(50, {40, 30});
            const Fill Pink = Fill::Of(Color::Rgba(255, 80, 160, 200));
            Say.Fills(50, std::span(&Pink, 1));
            Made = 1;
        }
        break;
    case 7: {
        const Fill Plain = Fill::Of(Color::Rgb(Random() % 256, Random() % 256, Random() % 256));
        Say.Fills(5, std::span(&Plain, 1));
        break;
    }
    case 8: Say.Size(2, {W * (0.3f + static_cast<float>(Random() % 30) / 100.0f), H * 0.5f}); break;
    case 9: Say.Turn(9, static_cast<float>(Random() % 360)), Say.Anchor(9, {30, 30}); break;
    default: Say.BlockChanged(1, Rect{0, 0, 32, 32}); break;
    }
}

void CheckSteps(Tally &Count, float Scale)
{
    const items::Theme  Look;
    items::ProgramState One = HostProgram(Look, Scale), Two = HostProgram(Look, Scale);
    One.From.PictureOf = Two.From.PictureOf = [](std::uint32_t Block) { return Block == 1 ? Checks() : nullptr; };
    Bench      A(One), B(Two);
    Screenful  Screen(640, 400);
    A.Area.Place = Rect{20, 20, 600, 360};
    B.Area.Place = Rect{330, 200, 280, 180};
    A.Area.Shown = B.Area.Shown = true;
    std::vector<std::pair<std::uint32_t, ItemPoint>> Sized;
    A.Area.Sized = [&](std::uint32_t Item, ItemPoint Size) { Sized.emplace_back(Item, Size); };
    items::AreaState *const Areas[] = {&A.Area, &B.Area};
    Changes SayA, SayB;
    Scene(SayA, 600 / Scale, 360 / Scale);
    Scene(SayB, 280 / Scale, 180 / Scale);
    (void)A.Step(SayA), (void)B.Step(SayB);
    Screen.Frame(Areas, Look);
    std::uint64_t Worst = Differ(Screen.Screen, Screen.Fresh(Areas, Look));
    int           MadeA = 0, MadeB = 0, Steps = 0;
    for (; Steps < 200; Steps++) {
        for (int K = 0, Many = 1 + static_cast<int>(Random() % 4); K < Many; K++) Change(SayA, 600 / Scale, 360 / Scale, MadeA);
        if (Random() % 3 == 0) Change(SayB, 280 / Scale, 180 / Scale, MadeB);
        if (Random() % 23 == 0) B.Area.Place = Rect{static_cast<std::int32_t>(Random() % 300), static_cast<std::int32_t>(Random() % 200), 280, 180};
        if (Random() % 31 == 0) B.Area.Shown = !B.Area.Shown;
        (void)A.Step(SayA), (void)B.Step(SayB);
        Screen.Frame(Areas, Look);
        Worst = std::max(Worst, Differ(Screen.Screen, Screen.Fresh(Areas, Look)));
    }
    const double Share = static_cast<double>(Screen.Composed) / (static_cast<double>(Steps + 1) * 640.0 * 400.0);
    char         Line[320];
    std::snprintf(Line, sizeof Line,
                  "at a scale of %.2f: %d steps of random changes in two areas, each composed only where it changed — "
                  "%.0f%% of the screen a step — and every pixel the screen composed whole from nothing (%llu differ); "
                  "refused %zu",
                  Scale, Steps, Share * 100, static_cast<unsigned long long>(Worst), A.Refusals.size() + B.Refusals.size());
    Count.Check(Worst == 0 && A.Refusals.empty() && B.Refusals.empty() && Share < 0.6, Line);
    if (Scale == 1.0f) {
        const items::Item *Label = A.Area.Items.Find(13);
        bool Said = false;
        for (const auto &[Item, Size] : Sized) Said = Said || (Item == 13 && Size.X > 0 && Size.Y > 0);
        Count.Check(Said && Label && Label->Drawn && Label->Drawn->Laid &&
                        std::abs(Label->Drawn->Measured.X - static_cast<std::int32_t>(Label->Drawn->Laid->Width() * ITEM_POINT)) <= 1,
                    "a text sized by its content says its size, the size its layout came to");
        const items::Item *Text = A.Area.Items.Find(4), *Page = A.Area.Items.Find(1);
        Count.Check(Text && Text->Drawn && Text->Drawn->Kept && Page && Page->Drawn && !Page->Drawn->Kept && Page->Drawn->Opaque,
                    "a text's picture kept, a plain page drawn straight and known to hide what lies beneath it");
    }
}

void CheckDamage(Tally &Count)
{
    const items::Theme  Look;
    items::ProgramState One = HostProgram(Look, 1);
    Bench               A(One);
    Screenful           Screen(640, 400);
    A.Area.Place = Rect{0, 0, 640, 400};
    A.Area.Shown = true;
    items::AreaState *const Areas[] = {&A.Area};
    Changes Say;
    Scene(Say, 640, 400);
    (void)A.Step(Say);
    Screen.Frame(Areas, Look);
    Screen.Composed = 0;
    Say.Text(13, "12:47");
    (void)A.Step(Say);
    Screen.Frame(Areas, Look);
    const std::uint64_t Clock = Screen.Composed;
    Count.Check(Clock > 0 && Clock < 640 * 400 / 30 && Differ(Screen.Screen, Screen.Fresh(Areas, Look)) == 0,
                "one word changed composes about that word alone: " + std::to_string(Clock) + " pixels of " +
                    std::to_string(640 * 400));
    Count.Check(Screen.Composer.Hidden(Rect{100, 100, 50, 50}, Areas) && !Screen.Composer.Hidden(Rect{0, 0, 641, 10}, Areas),
                "a plain opaque page hides whatever lies beneath it — the console's cells, other areas — never past its edge");
}

}  // namespace

void CheckCompose(Tally &Count)
{
    CheckSteps(Count, 1.0f);
    CheckSteps(Count, 1.5f);
    CheckDamage(Count);
}

bool WriteSheet(const char *Out)
{
    const items::Theme  Look = items::ThemeOf(0xD8D8E0, 0x181A28);
    items::ProgramState One  = HostProgram(Look, 1);
    One.From.PictureOf       = [](std::uint32_t Block) { return Block == 1 ? Checks() : nullptr; };
    Bench     A(One);
    Screenful Screen(960, 600);
    A.Area.Place = Rect{0, 0, 960, 600};
    A.Area.Shown = true;
    items::AreaState *const Areas[] = {&A.Area};
    Changes Say;
    Scene(Say, 960, 600);
    (void)A.Step(Say);
    Screen.Frame(Areas, Look);
    std::vector<std::uint32_t> Glass(960u * 600u);
    Screen.Screen.CopyOut(Rect{0, 0, 960, 600}, Glass.data(), 960, BOX_PIXEL_BGR, painter::core::Dither::On);
    std::FILE *File = std::fopen(Out, "wb");
    if (!File) return false;
    std::fprintf(File, "P6\n960 600\n255\n");
    for (std::uint32_t Pixel : Glass) {
        const unsigned char Rgb[3] = {static_cast<unsigned char>(Pixel >> 16), static_cast<unsigned char>(Pixel >> 8),
                                      static_cast<unsigned char>(Pixel)};
        std::fwrite(Rgb, 1, 3, File);
    }
    return std::fclose(File) == 0;
}

}  // namespace stand
