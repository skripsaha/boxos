/*
 * bloom — a flower that lives on the whole screen, drawn as items and moved by
 * the display while bloom sleeps: its petals open one after another on
 * springs, it turns slowly and breathes, pollen drifts about it. Space turns
 * the season — every colour walks to the next as the eye sees it, the
 * season's name fades into the next; the arrows give it more petals or fewer,
 * new ones springing out of nothing and old ones withering; enter blows a
 * gust that throws the petals out, and they spring back; h hides the words.
 * bloom only says where things go and how; the display plays it all, and with
 * less motion asked, the flower simply is.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
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

constexpr std::size_t PetalsLeast = 5, PetalsMost = 24, PetalsFirst = 12, Pollen = 18;

/* A season: what the page, the petals from base to tip, the heart and the
 * pollen are coloured. */
struct Season {
    const char *Name;
    Color       Page, Base, Tip, Heart, Pollen;
};

constexpr std::array<Season, 5> Seasons{{
    {"spring", Color::Rgb(0x14, 0x10, 0x1C), Color::Rgb(0xE0, 0x4E, 0x8A), Color::Rgb(0xFF, 0xE4, 0xF0), Color::Rgb(0xFF, 0xD0, 0x4A), Color::Rgb(0xFF, 0xF4, 0xB0)},
    {"summer", Color::Rgb(0x0C, 0x16, 0x1E), Color::Rgb(0xFF, 0x7A, 0x1A), Color::Rgb(0xFF, 0xF0, 0x6A), Color::Rgb(0x7A, 0x3A, 0x10), Color::Rgb(0xFF, 0xE8, 0x80)},
    {"autumn", Color::Rgb(0x1A, 0x0E, 0x08), Color::Rgb(0xA8, 0x1E, 0x10), Color::Rgb(0xFF, 0xB8, 0x40), Color::Rgb(0x3A, 0x1C, 0x0A), Color::Rgb(0xFF, 0xC8, 0x70)},
    {"winter", Color::Rgb(0x08, 0x10, 0x1C), Color::Rgb(0x3A, 0x7C, 0xE0), Color::Rgb(0xF0, 0xFA, 0xFF), Color::Rgb(0xC8, 0xE8, 0xFF), Color::Rgb(0xFF, 0xFF, 0xFF)},
    {"night", Color::Rgb(0x06, 0x04, 0x10), Color::Rgb(0x6A, 0x2C, 0xD8), Color::Rgb(0x50, 0xF0, 0xE0), Color::Rgb(0xF0, 0x60, 0xD0), Color::Rgb(0x90, 0xFF, 0xF0)},
}};

/* Numbers the display tells back: the last petal open, the heart open, a
 * gust's way out, a withered petal's end. */
constexpr std::uint32_t Opened = 1, HeartOpen = 2, GustOut = 3, Withered = 1000;

std::uint32_t Random()
{
    static std::uint32_t State = 0x9E3779B9u;
    State ^= State << 13;
    State ^= State >> 17;
    State ^= State << 5;
    return State;
}

float Between(float Low, float High) { return Low + (High - Low) * static_cast<float>(Random() % 10000) / 10000.0f; }

class Bloom {
public:
    Bloom(gui::Area Opened, painter::tui::Keyboard Heard)
        : Screen(std::move(Opened)), Keys(std::move(Heard)), Page(Screen.Rectangle(gui::Role::Page)), Flower(Screen.Group()),
          Leaves(Flower.Subgroup()), Middle(Flower.Ellipse()), Title(Screen.Text("bloom", gui::Role::Heading)), Name(Screen.Text(Seasons[0].Name, gui::Role::Hint)),
          Hint(Screen.Text("space  season    \xE2\x86\x90 \xE2\x86\x92  petals    enter  a gust    h  hide this    q  leave", gui::Role::Hint))
    {
        Keys.HearEnd();
    }

    void Run()
    {
        Begin();
        for (;;) {
            const gui::Event Heard = Screen.Wait(&Keys);
            switch (Heard.What) {
            case gui::Event::Kind::Key:
                if (!Key(Heard.Key)) return;
                break;
            case gui::Event::Kind::Area:
                Screen.Animate(gui::Animation::InOut(400ms), [&] { Lay(); });
                break;
            case gui::Event::Kind::AnimationEnd:
                Wither(Heard.Number);
                break;
            case gui::Event::Kind::Gone:
            case gui::Event::Kind::EndAsked:
                return;
            default:
                break;
            }
            (void)Screen.Present();
        }
    }

private:
    /* ---- Where things lie, in points ---- */

    PointF Centre() const { return PointF{Screen.Size().X / 2, Screen.Size().Y / 2 + 10}; }
    float  Length() const { return std::min(Screen.Size().X, Screen.Size().Y) * 0.22f; }
    float  Width() const { return Length() * 0.42f; }
    float  Angle(std::size_t Index, std::size_t Of) const { return 360.0f * static_cast<float>(Index) / static_cast<float>(Of); }

    /* A petal's corner in the flower: its base at the flower's heart, the
     * petal pushed Out further along where it points. */
    PointF PetalPlace(float Degrees, float Out = 0) const
    {
        const float Turn = Degrees * std::numbers::pi_v<float> / 180.0f;
        return PointF{-Width() / 2 + Out * std::sin(Turn), -Length() - Out * std::cos(Turn)};
    }

    gui::Fill PetalFill(const Season &Of, std::size_t Index) const
    {
        gui::Fill Made;
        Made.What  = gui::Fill::Kind::Linear;
        Made.From  = PointF{Width() / 2, Length()};
        Made.To    = PointF{Width() / 2, 0};
        const float Shade = 0.85f + 0.15f * static_cast<float>(Index % 3) / 2.0f;
        Made.Stops = {gui::Stop{0, Color::Transition(Of.Page, Of.Base, Shade)}, gui::Stop{1, Of.Tip}};
        return Made;
    }

    gui::Fill HeartFill(const Season &Of) const
    {
        gui::Fill Made;
        const float Side = Width() * 0.9f;
        Made.What   = gui::Fill::Kind::Radial;
        Made.From   = PointF{Side / 2, Side / 2};
        Made.Focus  = PointF{Side * 0.35f, Side * 0.35f};
        Made.Radius = Side / 2;
        Made.Stops  = {gui::Stop{0, Color::Transition(Of.Heart, Color::Rgb(255, 255, 255), 0.5f)}, gui::Stop{1, Of.Heart}};
        return Made;
    }

    /* Everything where it lies for the area as it is now. */
    void Lay()
    {
        const Season &Now = Seasons[Turned];
        Page.SetPlace({0, 0}).SetSize(Screen.Size()).SetFill(gui::Fill::Of(Now.Page));
        Flower.SetPlace(Centre());
        for (std::size_t I = 0; I < Petals.size(); I++) LayPetal(Petals[I], I, Petals.size());
        const float Side = Width() * 0.9f;
        Middle.SetPlace({-Side / 2, -Side / 2}).SetSize({Side, Side}).SetAnchor({Side / 2, Side / 2});
        Title.SetPlace({32, 24});
        Name.SetPlace({34, 58});
        Hint.SetPlace({32, Screen.Size().Y - 36});
    }

    void LayPetal(gui::Shape &Petal, std::size_t Index, std::size_t Of)
    {
        const float Degrees = Angle(Index, Of);
        Petal.SetSize({Width(), Length()}).SetAnchor({Width() / 2, Length()}).SetPlace(PetalPlace(Degrees)).SetTurn(Degrees);
    }

    gui::Shape NewPetal(std::size_t Index)
    {
        gui::Shape Petal = Leaves.Ellipse();
        Petal.SetFill(PetalFill(Seasons[Turned], Index));
        gui::Shadow Soft;
        Soft.Offset = PointF{0, 4};
        Soft.Blur   = 14;
        Soft.Color  = Color::Rgba(0, 0, 0, 80);
        Petal.SetShadow(Soft);
        return Petal;
    }

    /* ---- What happens ---- */

    /* The flower opens: each petal from nothing, turning out to its place on
     * a spring, one after another; then its heart; then it lives on. */
    void Begin()
    {
        for (std::size_t I = 0; I < PetalsFirst; I++) Petals.push_back(NewPetal(I));
        Middle.SetFill(HeartFill(Seasons[Turned])).SetScale({0, 0});
        Name.SetOpacity(0);
        Hint.SetOpacity(0);
        Lay();
        for (std::size_t I = 0; I < Petals.size(); I++) {
            Petals[I].SetScale({0, 0}).SetTurn(0);
            gui::Animation Open = gui::Animation::Spring(900ms, 0.3f);
            Open.Delay          = std::chrono::milliseconds(70 * I);
            Open.Number         = I + 1 == Petals.size() ? Opened : 0;
            Screen.Animate(Open, [&] { Petals[I].SetScale({1, 1}).SetTurn(Angle(I, Petals.size())); });
        }
        gui::Animation Then = gui::Animation::Spring(700ms, 0.5f);
        Then.After          = Opened;
        Then.Number         = HeartOpen;
        Screen.Animate(Then, [&] { Middle.SetScale({1, 1}); });
        gui::Animation Words = gui::Animation::Out(900ms);
        Words.After          = Opened;
        Screen.Animate(Words, [&] {
            Name.SetOpacity(1);
            Hint.SetOpacity(1);
        });
        Live();
        (void)Screen.Present();
    }

    /* Forever: the flower turns once in half a minute and breathes, its heart
     * beats, the pollen drifts. */
    void Live()
    {
        gui::Animation Turn = gui::Animation::Linear(30s);
        Turn.Repeats        = gui::Animation::Forever;
        Screen.Animate(Turn, [&] { Flower.SetTurn(360); });
        gui::Animation Breath = gui::Animation::InOut(2600ms);
        Breath.Repeats        = gui::Animation::Forever;
        Breath.Back           = true;
        Screen.Animate(Breath, [&] { Flower.SetScale({1.06f, 1.06f}); });
        gui::Animation Beat = gui::Animation::InOut(900ms);
        Beat.Repeats        = gui::Animation::Forever;
        Beat.Back           = true;
        Beat.After          = HeartOpen;
        Screen.Animate(Beat, [&] { Middle.SetScale({1.14f, 1.14f}); });
        for (std::size_t I = 0; I < Pollen; I++) {
            gui::Shape Dot = Screen.Ellipse();
            const float Side = Between(3, 7);
            Dot.SetSize({Side, Side}).SetFill(gui::Fill::Of(Seasons[Turned].Pollen)).SetOpacity(0);
            Dot.SetPlace({Between(0, Screen.Size().X), Between(0, Screen.Size().Y)});
            gui::Animation Drift = gui::Animation::InOut(std::chrono::milliseconds(static_cast<int>(Between(4000, 9000))));
            Drift.Repeats        = gui::Animation::Forever;
            Drift.Back           = true;
            Drift.Delay          = std::chrono::milliseconds(static_cast<int>(Between(0, 2000)));
            Screen.Animate(Drift, [&] {
                Dot.SetPlace({Between(0, Screen.Size().X), Between(0, Screen.Size().Y)}).SetOpacity(Between(0.3f, 0.9f));
            });
            Dust.push_back(std::move(Dot));
        }
    }

    /* The next season: every colour walks to its own, petal after petal, and
     * the season's name fades into the next. */
    void NextSeason()
    {
        Turned            = (Turned + 1) % Seasons.size();
        const Season &Now = Seasons[Turned];
        Screen.Animate(gui::Animation::InOut(1200ms), [&] {
            Page.SetFill(gui::Fill::Of(Now.Page));
            Middle.SetFill(HeartFill(Now));
            Name.SetText(Now.Name);
            for (gui::Shape &Dot : Dust) Dot.SetFill(gui::Fill::Of(Now.Pollen));
        });
        for (std::size_t I = 0; I < Petals.size(); I++) {
            gui::Animation Walk = gui::Animation::InOut(900ms);
            Walk.Delay          = std::chrono::milliseconds(40 * I);
            Screen.Animate(Walk, [&] { Petals[I].SetFill(PetalFill(Now, I)); });
        }
    }

    /* A petal more, springing out of nothing, the others making room; or one
     * fewer, withering and gone once it has. */
    void Petal(bool More)
    {
        if (More && Petals.size() < PetalsMost) {
            Petals.push_back(NewPetal(Petals.size()));
            gui::Shape &Born = Petals.back();
            LayPetal(Born, Petals.size() - 1, Petals.size());
            Born.SetScale({0, 0});
            Screen.Animate(gui::Animation::Spring(800ms, 0.4f), [&] { Born.SetScale({1, 1}); });
        } else if (!More && Petals.size() > PetalsLeast) {
            gui::Shape Going = std::move(Petals.back());
            Petals.pop_back();
            const std::uint32_t Number = Withered + static_cast<std::uint32_t>(Random() % 100000);
            gui::Animation      Fall   = gui::Animation::In(450ms);
            Fall.Number                = Number;
            Screen.Animate(Fall, [&] { Going.SetScale({0.2f, 0.2f}).SetOpacity(0); });
            Fading.push_back({Number, std::move(Going)});
        } else {
            return;
        }
        Screen.Animate(gui::Animation::Spring(700ms, 0.2f), [&] {
            for (std::size_t I = 0; I < Petals.size(); I++) LayPetal(Petals[I], I, Petals.size());
        });
    }

    void Wither(std::uint32_t Number)
    {
        std::erase_if(Fading, [Number](const std::pair<std::uint32_t, gui::Shape> &Each) { return Each.first == Number; });
    }

    /* A gust: every petal thrown out along where it points, then back on a
     * spring that overshoots, all together once all are out. */
    void Gust()
    {
        const float Out = Length() * 0.45f;
        gui::Animation Away = gui::Animation::Out(220ms);
        Away.Number         = GustOut;
        gui::Animation Back = gui::Animation::Spring(900ms, 0.45f);
        Back.After          = GustOut;
        for (std::size_t I = 0; I < Petals.size(); I++) {
            const float Degrees = Angle(I, Petals.size());
            Screen.Animate(Away, [&] { Petals[I].SetPlace(PetalPlace(Degrees, Out * Between(0.7f, 1.3f))); });
            Screen.Animate(Back, [&] { Petals[I].SetPlace(PetalPlace(Degrees)); });
        }
    }

    bool Key(const painter::tui::KeyEvent &Pressed)
    {
        if (Pressed.IsExtended()) {
            if (Pressed.Scancode == painter::tui::KeyCodes::Right || Pressed.Scancode == painter::tui::KeyCodes::Up) Petal(true);
            if (Pressed.Scancode == painter::tui::KeyCodes::Left || Pressed.Scancode == painter::tui::KeyCodes::Down) Petal(false);
            return true;
        }
        switch (Pressed.Character) {
        case ' ': NextSeason(); return true;
        case painter::tui::KeyCodes::Enter: Gust(); return true;
        case 'h':
        case 'H':
            Shown = !Shown;
            Screen.Animate(gui::Animation::Out(350ms), [&] {
                if (Shown) {
                    Hint.Show();
                    Name.Show();
                } else {
                    Hint.Hide();
                    Name.Hide();
                }
            });
            return true;
        case 'q':
        case 'Q':
        case painter::tui::KeyCodes::Escape: return false;
        default: return true;
        }
    }

    gui::Area                                     Screen;
    painter::tui::Keyboard                        Keys;
    gui::Shape                                    Page;
    gui::Group                                    Flower;
    gui::Group                                    Leaves;   // the petals, all beneath the heart
    gui::Shape                                    Middle;
    gui::Text                                     Title, Name, Hint;
    std::vector<gui::Shape>                       Petals;
    std::vector<gui::Shape>                       Dust;
    std::vector<std::pair<std::uint32_t, gui::Shape>> Fading;
    std::size_t                                   Turned = 0;
    bool                                          Shown  = true;
};

}  // namespace

int main()
{
    std::expected<gui::Area, box::error> Opened = gui::Area::Open(gui::Placement::Whole);
    if (!Opened || Opened->On() != gui::Glass::Pixels) {
        std::printf("bloom draws on a framebuffer's pixels; this screen is a text page\n");
        return 1;
    }
    std::expected<painter::tui::Keyboard, painter::tui::StageError> Keys = painter::tui::Keyboard::Open();
    if (!Keys) {
        std::printf("bloom: the keyboard is not to be had\n");
        return 1;
    }
    Bloom(std::move(*Opened), std::move(*Keys)).Run();
    return 0;
}
