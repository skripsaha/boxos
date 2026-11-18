/*
 * ripple — a pond of tiles: every key drops a stone, and a ring of light runs
 * out from where it fell, tile after tile by how far each lies, each popping
 * up as the ring reaches it and settling on a spring, keeping a little of the
 * stone's colour — so the pond is painted by its ripples, stone by stone.
 * The arrows move where the next stone falls, space drops it there, any
 * other key drops one anywhere; c clears the pond; escape leaves. Hundreds
 * of tiles moving at once, said in one step each time and played by the
 * display while ripple waits.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#include "painter/core/color.h"
#include "painter/gui/area.h"
#include "painter/gui/items.h"
#include "painter/tui/keyboard.h"

namespace {

using namespace std::chrono_literals;
namespace gui = painter::gui;
using painter::core::Color;
using painter::core::PointF;

constexpr int   Across = 16, Down = 9;
constexpr float Gap    = 6;     // between tiles, in points
constexpr float Margin = 64;    // round the pond, and above it the words
constexpr int   Stride = 55;    // milliseconds the ring takes from a tile to the next

const Color Water = Color::Rgb(0x16, 0x1C, 0x2A);

std::uint32_t Random()
{
    static std::uint32_t State = 0x68E31DA4u;
    State ^= State << 13;
    State ^= State >> 17;
    State ^= State << 5;
    return State;
}

/* A stone's colour: bright, of any hue, as the eye sees it. */
Color StoneColor()
{
    const float Turn = static_cast<float>(Random() % 6283) / 1000.0f;
    float       R = 0, G = 0, B = 0;
    painter::core::RgbOf(painter::core::Oklab{0.82f, 0.17f * std::cos(Turn), 0.17f * std::sin(Turn)}, R, G, B);
    const auto Byte = [](float Of) { return static_cast<std::uint8_t>(std::lround(std::clamp(Of, 0.0f, 1.0f) * 255)); };
    return Color::Rgb(Byte(R), Byte(G), Byte(B));
}

struct Tile {
    gui::Shape Plate;
    Color      Settled = Water;   // what it rests at between ripples
};

class Pond {
public:
    Pond(gui::Area Opened, painter::tui::Keyboard Heard)
        : Screen(std::move(Opened)), Keys(std::move(Heard)), Page(Screen.Rectangle(gui::Role::Page)),
          Title(Screen.Text("ripple", gui::Role::Heading)),
          Hint(Screen.Text("arrows  aim    space  drop it there    any key  a stone anywhere    c  clear    esc  leave", gui::Role::Hint)),
          Aim(Screen.Ellipse())
    {
        Keys.HearEnd();
    }

    void Run()
    {
        Make();
        for (;;) {
            const gui::Event Heard = Screen.Wait(&Keys);
            if (Heard.What == gui::Event::Kind::Gone || Heard.What == gui::Event::Kind::EndAsked) return;
            if (Heard.What == gui::Event::Kind::Key && !Key(Heard.Key)) return;
            if (Heard.What == gui::Event::Kind::Area) Screen.Animate(gui::Animation::InOut(400ms), [&] { Lay(); });
            (void)Screen.Present();
        }
    }

private:
    float Side() const
    {
        const float Wide = (Screen.Size().X - 2 * Margin - (Across - 1) * Gap) / Across;
        const float Tall = (Screen.Size().Y - 2.5f * Margin - (Down - 1) * Gap) / Down;
        return std::max(8.0f, std::min(Wide, Tall));
    }
    PointF Corner() const
    {
        const float S = Side();
        return PointF{(Screen.Size().X - (Across * S + (Across - 1) * Gap)) / 2,
                      Margin * 1.3f + (Screen.Size().Y - 2.5f * Margin - (Down * S + (Down - 1) * Gap)) / 2};
    }
    PointF TileAt(int X, int Y) const
    {
        const PointF C = Corner();
        return PointF{C.X + static_cast<float>(X) * (Side() + Gap), C.Y + static_cast<float>(Y) * (Side() + Gap)};
    }

    void Lay()
    {
        const float S = Side();
        Page.SetPlace({0, 0}).SetSize(Screen.Size()).SetFill(gui::Fill::Of(Color::Rgb(0x0A, 0x0D, 0x14)));
        Title.SetPlace({32, 20});
        Hint.SetPlace({32, Screen.Size().Y - 36});
        for (int Y = 0; Y < Down; Y++)
            for (int X = 0; X < Across; X++)
                Tiles[static_cast<std::size_t>(Y * Across + X)]
                    .Plate.SetPlace(TileAt(X, Y))
                    .SetSize({S, S})
                    .SetAnchor({S / 2, S / 2})
                    .SetCorners(painter::core::Corners::All(S * 0.18f));
        const PointF At = TileAt(AimX, AimY);
        Aim.SetPlace({At.X - 4, At.Y - 4}).SetSize({S + 8, S + 8});
    }

    /* The pond rises out of nothing, tile after tile from its middle. */
    void Make()
    {
        Tiles.reserve(Across * Down);
        for (int I = 0; I < Across * Down; I++) {
            Tile Made{Screen.Rectangle(), Water};
            Made.Plate.SetFill(gui::Fill::Of(Water)).SetScale({0, 0});
            Tiles.push_back(std::move(Made));
        }
        gui::Stroke Ring;
        Ring.Width = 2;
        Ring.With  = gui::Fill::Of(Color::Rgba(255, 255, 255, 170));
        Aim.SetFills({}).SetStroke(Ring).SetOpacity(0);
        Hint.SetOpacity(0);
        Lay();
        for (int Y = 0; Y < Down; Y++)
            for (int X = 0; X < Across; X++) {
                gui::Animation Rise = gui::Animation::Spring(700ms, 0.3f);
                Rise.Delay          = std::chrono::milliseconds(static_cast<int>(Distance(X, Y, Across / 2, Down / 2) * 40));
                Screen.Animate(Rise, [&] { Tiles[static_cast<std::size_t>(Y * Across + X)].Plate.SetScale({1, 1}); });
            }
        gui::Animation Late = gui::Animation::Out(800ms);
        Late.Delay          = 600ms;
        Screen.Animate(Late, [&] {
            Hint.SetOpacity(1);
            Aim.SetOpacity(1);
        });
        (void)Screen.Present();
    }

    static float Distance(int X, int Y, int FromX, int FromY)
    {
        return std::sqrt(static_cast<float>((X - FromX) * (X - FromX) + (Y - FromY) * (Y - FromY)));
    }

    /* A stone at X, Y: the ring runs out, each tile popping up and lit as it
     * arrives, settling on a spring and keeping some of the stone's colour,
     * the more the nearer. */
    void Drop(int AtX, int AtY)
    {
        const Color Stone = StoneColor();
        for (int Y = 0; Y < Down; Y++)
            for (int X = 0; X < Across; X++) {
                Tile       &One  = Tiles[static_cast<std::size_t>(Y * Across + X)];
                const float Far  = Distance(X, Y, AtX, AtY);
                const int   When = static_cast<int>(Far * Stride);
                const float Kept = 0.45f * std::exp(-Far / 5.0f);
                One.Settled      = Color::Transition(One.Settled, Stone, Kept);
                const float Pop  = 1.0f + 0.32f * std::exp(-Far / 7.0f);
                gui::Animation Up = gui::Animation::Out(150ms);
                Up.Delay          = std::chrono::milliseconds(When);
                Screen.Animate(Up, [&] {
                    One.Plate.SetScale({Pop, Pop}).SetFill(gui::Fill::Of(Color::Transition(One.Settled, Stone, 0.85f)));
                });
                gui::Animation Settle = gui::Animation::Spring(800ms, 0.35f);
                Settle.Delay          = std::chrono::milliseconds(When + 150);
                Screen.Animate(Settle, [&] { One.Plate.SetScale({1, 1}).SetFill(gui::Fill::Of(One.Settled)); });
            }
    }

    /* Calm water again, washing out from the middle. */
    void Clear()
    {
        for (int Y = 0; Y < Down; Y++)
            for (int X = 0; X < Across; X++) {
                Tile &One  = Tiles[static_cast<std::size_t>(Y * Across + X)];
                One.Settled = Water;
                gui::Animation Wash = gui::Animation::InOut(700ms);
                Wash.Delay          = std::chrono::milliseconds(static_cast<int>(Distance(X, Y, Across / 2, Down / 2) * 30));
                Screen.Animate(Wash, [&] { One.Plate.SetFill(gui::Fill::Of(Water)).SetScale({1, 1}); });
            }
    }

    void MoveAim(int Dx, int Dy)
    {
        AimX = std::clamp(AimX + Dx, 0, Across - 1);
        AimY = std::clamp(AimY + Dy, 0, Down - 1);
        const PointF At = TileAt(AimX, AimY);
        Screen.Animate(gui::Animation::Spring(350ms, 0.25f), [&] { Aim.SetPlace({At.X - 4, At.Y - 4}); });
    }

    bool Key(const painter::tui::KeyEvent &Pressed)
    {
        if (Pressed.IsExtended()) {
            switch (Pressed.Scancode) {
            case painter::tui::KeyCodes::Left: MoveAim(-1, 0); break;
            case painter::tui::KeyCodes::Right: MoveAim(1, 0); break;
            case painter::tui::KeyCodes::Up: MoveAim(0, -1); break;
            case painter::tui::KeyCodes::Down: MoveAim(0, 1); break;
            default: break;
            }
            return true;
        }
        switch (Pressed.Character) {
        case painter::tui::KeyCodes::Escape: return false;
        case U' ': Drop(AimX, AimY); return true;
        case U'c':
        case U'C': Clear(); return true;
        default:
            if (Pressed.HasCharacter()) Drop(static_cast<int>(Random() % Across), static_cast<int>(Random() % Down));
            return true;
        }
    }

    gui::Area              Screen;
    painter::tui::Keyboard Keys;
    gui::Shape             Page;
    gui::Text              Title, Hint;
    gui::Shape             Aim;
    std::vector<Tile>      Tiles;
    int                    AimX = Across / 2, AimY = Down / 2;
};

}  // namespace

int main()
{
    std::expected<gui::Area, box::error> Opened = gui::Area::Open(gui::Placement::Whole);
    if (!Opened || Opened->On() != gui::Glass::Pixels) {
        std::printf("ripple draws on a framebuffer's pixels; this screen is a text page\n");
        return 1;
    }
    std::expected<painter::tui::Keyboard, painter::tui::StageError> Keys = painter::tui::Keyboard::Open();
    if (!Keys) {
        std::printf("ripple: the keyboard is not to be had\n");
        return 1;
    }
    Pond(std::move(*Opened), std::move(*Keys)).Run();
    return 0;
}
