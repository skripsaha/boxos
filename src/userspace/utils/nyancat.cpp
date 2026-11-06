/* nyancat — the cat with a pop-tart for a body, flying through space on a
 * rainbow, as big as the screen holds it.
 *
 *   nyancat          nyan
 *   up, down         fly higher, lower
 *   q, ESC           leave, and say for how long it nyaned
 *
 * The cat is drawn, not copied: every part of it is laid in units of the
 * screen's own scale, so a wide framebuffer gets a big cat and a text console
 * the classic one. Pixels are two to a cell's height (stage.h). */

#include <algorithm>
#include <format>
#include <new>
#include <optional>
#include <string_view>
#include <vector>

#include "box/clock.h"
#include "box/cpu.h"
#include "box/form.h"
#include "box/print.h"

#include "painter/core/color.h"
#include "painter/tui/stage.h"

BOX_FORM("");

namespace {

using Color = painter::core::Color;
using painter::tui::EventKind;
using painter::tui::Stage;
using painter::tui::StageError;
using painter::tui::TextStyle;

constexpr std::uint64_t FrameUs   = 33333;
constexpr float         CatFrames = 14.0f;   /* a second: legs, tail and bob */
constexpr int           CatWide   = 34;
constexpr int           CatHigh   = 21;

const Color Space    = Color::Rgb(0x00, 0x33, 0x66);
const Color Outline  = painter::core::Colors::Black;
const Color Crust    = Color::Rgb(0xFF, 0xCC, 0x99);
const Color Frosting = Color::Rgb(0xFF, 0x99, 0xFF);
const Color Sprinkle = Color::Rgb(0xFF, 0x33, 0x99);
const Color Fur      = Color::Rgb(0x99, 0x99, 0x99);
const Color Cheek    = Color::Rgb(0xFF, 0x99, 0x99);
const Color White    = painter::core::Colors::White;
const Color Bands[]  = {Color::Rgb(0xFF, 0x00, 0x00), Color::Rgb(0xFF, 0x99, 0x00), Color::Rgb(0xFF, 0xFF, 0x00),
                        Color::Rgb(0x33, 0xFF, 0x00), Color::Rgb(0x00, 0x99, 0xFF), Color::Rgb(0x66, 0x33, 0xFF)};

struct Star {
    float X, Y;
    float Twinkle;   /* its place in the twinkle's six steps */
};

/* The picture: pixels at a scale, every part laid in cat units. */
class Picture {
public:
    void Resize(int Wide, int High, int By)
    {
        Width  = Wide;
        Height = High;
        Scale  = By;
        Dots.assign(static_cast<std::size_t>(Width) * Height, Space);
    }

    void Clear() { std::fill(Dots.begin(), Dots.end(), Space); }

    void Dot(int X, int Y, Color Ink)
    {
        if (X >= 0 && Y >= 0 && X < Width && Y < Height) Dots[static_cast<std::size_t>(Y) * Width + X] = Ink;
    }

    /* A block of cat units at (Ux, Uy) from the cat's corner (X, Y). */
    void Block(int X, int Y, int Ux, int Uy, int Uw, int Uh, Color Ink)
    {
        for (int Dy = 0; Dy < Uh * Scale; Dy++)
            for (int Dx = 0; Dx < Uw * Scale; Dx++) Dot(X + Ux * Scale + Dx, Y + Uy * Scale + Dy, Ink);
    }

    /* A box with its corners cut, in cat units. */
    void Rounded(int X, int Y, int Ux, int Uy, int Uw, int Uh, Color Fill)
    {
        Block(X, Y, Ux + 1, Uy, Uw - 2, Uh, Fill);
        Block(X, Y, Ux, Uy + 1, Uw, Uh - 2, Fill);
    }

    /* The same, outlined: the outline, then what fills it. */
    void Box(int X, int Y, int Ux, int Uy, int Uw, int Uh, Color Fill)
    {
        Rounded(X, Y, Ux, Uy, Uw, Uh, Outline);
        Block(X, Y, Ux + 1, Uy + 1, Uw - 2, Uh - 2, Fill);
    }

    int                Width = 0, Height = 0, Scale = 1;
    std::vector<Color> Dots;
};

void DrawStar(Picture &View, const Star &One)
{
    int X = static_cast<int>(One.X), Y = static_cast<int>(One.Y);
    int Step = static_cast<int>(One.Twinkle) % 6;
    auto Arm = [&](int Reach, bool Middle) {
        if (Middle) View.Block(X, Y, 0, 0, 1, 1, White);
        for (int R = 1; R <= Reach; R++) {
            View.Block(X, Y, R, 0, 1, 1, White);
            View.Block(X, Y, -R, 0, 1, 1, White);
            View.Block(X, Y, 0, R, 1, 1, White);
            View.Block(X, Y, 0, -R, 1, 1, White);
        }
    };
    switch (Step) {
    case 0: View.Block(X, Y, 0, 0, 1, 1, White); break;
    case 1: Arm(1, false); break;
    case 2: Arm(2, true); break;
    case 3: Arm(3, false); break;
    case 4:
        for (int R : {-3, 3}) {
            View.Block(X, Y, R, 0, 1, 1, White);
            View.Block(X, Y, 0, R, 1, 1, White);
        }
        break;
    default: break;   /* the sixth step is dark: the star blinks */
    }
}

/* The rainbow from the left edge to the pop-tart, its bands stepping up and
 * down in blocks of eight that march as the cat flies. */
void DrawRainbow(Picture &View, int CatX, int CatY, int Frame)
{
    int S = View.Scale, Band = 3 * S, Step = 8 * S;
    int End = CatX + 7 * S;
    for (int X = End - 1; X >= 0; X--) {
        int  Chunk = (End - 1 - X) / Step;
        bool Up    = ((Chunk + Frame / 2) % 2) == 0;
        int  Top   = CatY + 1 * S + (Up ? 0 : S);
        for (int B = 0; B < 6; B++)
            for (int Dy = 0; Dy < Band; Dy++) View.Dot(X, Top + B * Band + Dy, Bands[B]);
    }
}

/* The cat at (X, Y), its frame 0..5: the pop-tart bobs, the legs and tail
 * swing, the head leads. */
void DrawCat(Picture &View, int X, int Y, int Frame)
{
    static const int Bob[6]  = {0, 0, 1, 1, 1, 0};
    static const int Paws[6] = {0, 1, 1, 0, -1, -1};
    static const int Tail[6] = {0, 1, 2, 1, 0, -1};
    int Lift = Bob[Frame], Swing = Paws[Frame];

    /* Tail: a stub of fur behind the tart. */
    View.Box(X, Y, 0, 10 + Tail[Frame], 8, 4, Fur);

    /* Legs under the tart, two at the back and two at the front. */
    for (int Leg : {7, 11, 21, 25}) View.Box(X, Y, Leg + Swing, 17, 4, 4, Fur);

    /* The pop-tart: outline and crust, the frosting inset, sprinkles. */
    View.Box(X, Y, 6, 0 + Lift, 22, 18, Crust);
    View.Rounded(X, Y, 8, 2 + Lift, 18, 14, Frosting);
    static const int Sprinkles[][2] = {{11, 4}, {15, 3}, {19, 5}, {23, 4}, {10, 8}, {14, 10}, {18, 7},
                                       {22, 9}, {12, 13}, {17, 12}, {21, 13}, {24, 12}};
    for (const auto &At : Sprinkles) View.Block(X, Y, At[0], At[1] + Lift, 1, 1, Sprinkle);

    /* The head: ears, face, eyes, cheeks, mouth. */
    int Hx = 20 + (Frame == 3 || Frame == 4 ? 1 : 0), Hy = 6 + Lift;
    View.Block(X, Y, Hx + 1, Hy - 2, 3, 3, Outline);
    View.Block(X, Y, Hx + 2, Hy - 1, 1, 2, Fur);
    View.Block(X, Y, Hx + 10, Hy - 2, 3, 3, Outline);
    View.Block(X, Y, Hx + 11, Hy - 1, 1, 2, Fur);
    View.Box(X, Y, Hx, Hy, 14, 11, Fur);
    View.Block(X, Y, Hx + 3, Hy + 3, 2, 2, Outline);
    View.Block(X, Y, Hx + 3, Hy + 3, 1, 1, White);
    View.Block(X, Y, Hx + 9, Hy + 3, 2, 2, Outline);
    View.Block(X, Y, Hx + 9, Hy + 3, 1, 1, White);
    View.Block(X, Y, Hx + 7, Hy + 5, 1, 1, Outline);
    View.Block(X, Y, Hx + 1, Hy + 6, 2, 2, Cheek);
    View.Block(X, Y, Hx + 11, Hy + 6, 2, 2, Cheek);
    for (int Mx : {4, 6, 8, 10}) View.Block(X, Y, Hx + Mx, Hy + 7, 1, 1, Outline);
    for (int Mx : {5, 9}) View.Block(X, Y, Hx + Mx, Hy + 8, 1, 1, Outline);
    View.Block(X, Y, Hx + 7, Hy + 8, 1, 1, Outline);
}

struct Flight {
    std::optional<Stage> Screen;
    Picture            View;
    std::vector<Star>  Stars;
    std::uint64_t      Seed   = cpu_rdtsc() | 1;
    float              Height = 0.45f;   /* where the cat flies, 0 top .. 1 bottom */

    std::uint64_t Roll()
    {
        Seed ^= Seed << 13;
        Seed ^= Seed >> 7;
        Seed ^= Seed << 17;
        return Seed;
    }

    void SowStars()
    {
        int Many = View.Width * View.Height / (180 * View.Scale * View.Scale) + 6;
        for (int At = 0; At < Many; At++)
            Stars.push_back({static_cast<float>(Roll() % static_cast<std::uint64_t>(View.Width)),
                             static_cast<float>(Roll() % static_cast<std::uint64_t>(View.Height)),
                             static_cast<float>(Roll() % 6)});
    }

    void Step(float Seconds)
    {
        float Speed = static_cast<float>(View.Width) / 1.6f;
        for (Star &One : Stars) {
            One.X -= Speed * Seconds;
            One.Twinkle += Seconds * 10.0f;
            if (One.X < -4.0f * View.Scale) {
                One.X       = static_cast<float>(View.Width + static_cast<int>(Roll() % 40));
                One.Y       = static_cast<float>(Roll() % static_cast<std::uint64_t>(View.Height));
                One.Twinkle = static_cast<float>(Roll() % 6);
            }
        }
    }

    void Draw(float Seconds)
    {
        View.Clear();
        for (const Star &One : Stars) DrawStar(View, One);
        int Frame = static_cast<int>(Seconds * CatFrames) % 6;
        int CatX  = (View.Width - CatWide * View.Scale) / 2;
        int Room  = std::max(0, View.Height - CatHigh * View.Scale - 2);
        int CatY  = static_cast<int>(Height * static_cast<float>(Room));
        DrawRainbow(View, CatX, CatY, static_cast<int>(Seconds * CatFrames));
        DrawCat(View, CatX, CatY, Frame);

        Screen->FrameBuffer().PutPixels(View.Dots);
        std::string Count = std::format(" You have nyaned for {:.1f} seconds! ", Seconds);
        const painter::core::Size Dimensions = Screen->Size();
        Screen->FrameBuffer().Centre(Dimensions.Height - 1, Count, TextStyle{White, Space});
    }
};

void Say(std::string_view Text)
{
    std::string Line(Text);
    Line += '\n';
    print_to_screen(Line.c_str());
}

}

int main()
{
    try {
        Flight Show;
        std::expected<Stage, StageError> Opened = Stage::Open("nyancat", 40, 14);
        if (!Opened) {
            Say(Opened.error().Describe("nyancat"));
            return 1;
        }
        Show.Screen = std::move(*Opened);
        /* The classic cat on 80 x 25; twice and more where the screen is. */
        const painter::core::Size Dimensions = Show.Screen->Size();
        int Scale = std::max(1, std::min(Dimensions.Width / 80, Dimensions.Height * 2 / 50));
        Show.View.Resize(Dimensions.Width, Dimensions.Height * 2, Scale);
        Show.SowStars();

        std::uint64_t Began = clock_uptime_us();
        std::uint64_t Last  = Began;
        bool          Shown = true;
        for (;;) {
            std::uint64_t Now = clock_uptime_us();
            Show.Step(static_cast<float>(Now - Last) / 1e6f);
            Last = Now;
            Show.Draw(static_cast<float>(Now - Began) / 1e6f);
            if (auto Presented = Show.Screen->Present(); !Presented) {
                Shown = false;
                break;
            }

            std::uint64_t Spent = clock_uptime_us() - Now;
            painter::tui::Event Event = Show.Screen->Wait(Spent >= FrameUs ? 1 : static_cast<std::uint32_t>((FrameUs - Spent) / 1000 + 1));
            if (Event.Kind == EventKind::Gone) break;
            if (Event.Kind != EventKind::Key) continue;
            if (Event.Key.IsExtended()) {
                if (Event.Key.Scancode == painter::tui::KeyCodes::Up) Show.Height = std::max(0.0f, Show.Height - 0.1f);
                if (Event.Key.Scancode == painter::tui::KeyCodes::Down) Show.Height = std::min(1.0f, Show.Height + 0.1f);
            } else if (Event.Key.Ascii == 'q' || Event.Key.Ascii == 'Q' || Event.Key.Ascii == painter::tui::KeyCodes::Escape) {
                break;
            }
        }
        float Nyaned = static_cast<float>(clock_uptime_us() - Began) / 1e6f;
        (void)Show.Screen->Close();
        Say(Shown ? std::format("[NYANCAT] nyaned for {:.1f} seconds", Nyaned)
                  : "The screen stopped taking frames.");
        return Shown ? 0 : 1;
    } catch (const std::bad_alloc &) {
        Say("There is not enough memory for a cat the size of this screen.");
        return 1;
    }
}
